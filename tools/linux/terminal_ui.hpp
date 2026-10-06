#pragma once
// A full-screen terminal menu without libraries (any distro's terminal): raw mode through
// termios, the alternate screen and ANSI codes. Arrow keys move, Space ticks a checkbox, ←/→
// change a value or choice, Enter acts, types a value or opens a choice's list, Esc goes back. The screen is
// redrawn whole on each change; menus are rebuilt from their state every time, so they always
// show what is true now.
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

namespace x4vr::tui {
enum Key { None = -1, Up = 1000, Down, Left, Right, Enter, Escape, Backspace, Home, End, PageUp, PageDown };

class Terminal {
public:
    Terminal() {
        ok_ = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO) && tcgetattr(STDIN_FILENO, &saved_) == 0;
        if (!ok_) return;
        termios raw = saved_;
        raw.c_lflag &= ~tcflag_t(ICANON | ECHO | ISIG | IEXTEN);
        raw.c_iflag &= ~tcflag_t(IXON | ICRNL);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
        put("\x1b[?1049h\x1b[?25l"); // alternate screen, cursor hidden
    }
    ~Terminal() {
        if (!ok_) return;
        put("\x1b[0m\x1b[?25h\x1b[?1049l");
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_);
    }
    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;
    bool ok() const { return ok_; }
    void put(const std::string& text) { (void)!::write(STDOUT_FILENO, text.data(), text.size()); }
    int columns() const { winsize w{}; return ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col ? w.ws_col : 80; }
    int rows() const { winsize w{}; return ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_row ? w.ws_row : 24; }
    // The next key, or None after `timeout_ms` (-1: wait).
    int key(int timeout_ms) {
        pollfd p{STDIN_FILENO, POLLIN, 0};
        if (poll(&p, 1, timeout_ms) <= 0) return None;
        unsigned char b[8]{};
        const auto n = ::read(STDIN_FILENO, b, sizeof b);
        if (n <= 0) return None;
        if (b[0] == 0x1b) {
            if (n == 1) return Escape;
            if (b[1] == '[' || b[1] == 'O') {
                switch (b[2]) {
                case 'A': return Up;   case 'B': return Down; case 'C': return Right; case 'D': return Left;
                case 'H': return Home; case 'F': return End;
                case '1': case '7': return Home; case '4': case '8': return End;
                case '5': return PageUp; case '6': return PageDown;
                }
            }
            return None;
        }
        if (b[0] == '\r' || b[0] == '\n') return Enter;
        if (b[0] == 127 || b[0] == 8) return Backspace;
        if (b[0] == 3) return None; // Ctrl+C: nothing (raw mode; quitting is the Quit item)
        return b[0];
    }
private:
    bool ok_ = false;
    termios saved_{};
};

// Colours: kept to a few, so the screen reads in any terminal theme.
inline constexpr const char* reset = "\x1b[0m", *bold = "\x1b[1m", *dim = "\x1b[2m", *green = "\x1b[32m", *yellow = "\x1b[33m",
                                *red = "\x1b[31m", *cyan = "\x1b[36m", *inverse = "\x1b[7m";

// Display width of UTF-8 text (one column per code point; the menu only uses narrow symbols).
inline size_t width(const std::string& s) { return size_t(std::count_if(s.begin(), s.end(), [](char c) { return (c & 0xc0) != 0x80; })); }
// `s` cut or padded to `columns`.
inline std::string fit(const std::string& s, size_t columns) {
    if (width(s) <= columns) return s+std::string(columns-width(s), ' ');
    std::string out; size_t w = 0;
    for (size_t i = 0; i < s.size() && w+1 < columns; ++i) {
        out += s[i]; ++w;
        while (i+1 < s.size() && (s[i+1] & 0xc0) == 0x80) out += s[++i];
    }
    return out+(columns ? "…" : "");
}
// `s` broken into lines of at most `columns`, at spaces.
inline std::vector<std::string> wrap(const std::string& s, size_t columns) {
    std::vector<std::string> lines;
    std::string line;
    size_t start = 0;
    while (start <= s.size()) {
        auto end = s.find(' ', start);
        if (end == std::string::npos) end = s.size();
        const auto word = s.substr(start, end-start);
        if (!line.empty() && width(line)+1+width(word) > columns) { lines.push_back(line); line.clear(); }
        line += (line.empty() ? "" : " ")+word;
        start = end+1;
    }
    if (!line.empty()) lines.push_back(line);
    return lines;
}
inline std::string format(double value, int decimals) { char t[32]; std::snprintf(t, sizeof t, "%.*f", decimals, value); return t; }

// One row of a menu.
struct Item {
    enum Kind { Section, Info, Status, Notice, Toggle, Choice, Number, Action } kind = Info;
    std::string id, label, value, help;
    std::string tag;                     // shown at the right: "live", "next launch", ...
    std::string mark;                    // Status: another symbol for the state (same colour)
    int state = 0;                       // Status: 0 ok, 1 warning, 2 problem, 3 neutral; Notice: 0 tip, 1 issue, 2 help
    bool on = false;                     // Toggle
    std::vector<std::string> choices;    // Choice
    int choice = 0;
    double number = 0, step = 0.1, low = 0, high = 100; // Number
    int decimals = 2;
    std::string unit;                    // Number: shown after the value
    std::string bullet = "▸ ";           // Action: in front of the label
    bool selectable() const { return kind == Toggle || kind == Choice || kind == Number || kind == Action; }
};
inline Item section(std::string label) { Item i; i.kind = Item::Section; i.label = std::move(label); return i; }
inline Item info(std::string label) { Item i; i.kind = Item::Info; i.label = std::move(label); return i; }
inline Item status(std::string label, int state, std::string value) {
    Item i; i.kind = Item::Status; i.label = std::move(label); i.state = state; i.value = std::move(value); return i;
}
inline Item notice(int kind, std::string text) { Item i; i.kind = Item::Notice; i.state = kind; i.label = std::move(text); return i; }
inline Item action(std::string id, std::string label, std::string help = {}, std::string tag = {}) {
    Item i; i.kind = Item::Action; i.id = std::move(id); i.label = std::move(label); i.help = std::move(help); i.tag = std::move(tag); return i;
}
inline Item toggle(std::string id, std::string label, bool on, std::string help, std::string tag) {
    Item i; i.kind = Item::Toggle; i.id = std::move(id); i.label = std::move(label); i.on = on; i.help = std::move(help); i.tag = std::move(tag); return i;
}
inline Item choice(std::string id, std::string label, std::vector<std::string> choices, int current, std::string help, std::string tag) {
    Item i; i.kind = Item::Choice; i.id = std::move(id); i.label = std::move(label); i.choices = std::move(choices);
    i.choice = std::clamp(current, 0, std::max(int(i.choices.size())-1, 0)); i.help = std::move(help); i.tag = std::move(tag); return i;
}
inline Item number(std::string id, std::string label, double value, double step, double low, double high, int decimals,
                   std::string unit, std::string help, std::string tag) {
    Item i; i.kind = Item::Number; i.id = std::move(id); i.label = std::move(label); i.number = value; i.step = step; i.low = low;
    i.high = high; i.decimals = decimals; i.unit = std::move(unit); i.help = std::move(help); i.tag = std::move(tag); return i;
}

// What happened on a menu: the item acted on (id) and how.
struct Event { enum Kind { Back, Activate, Changed } kind; std::string id; Item item; };

class Menu {
public:
    explicit Menu(Terminal& t) : t_(t) {}
    std::string title, title_right;
    std::string keys = "↑↓ move   Enter select   Space tick   ←→ change   Esc back";
    std::vector<std::string> messages; // shown above the help until the next key
    int timeout_ms = -1;               // redraw without a key after this (-1: wait), for live status
    size_t label_width = 30;           // columns for labels, before values

    // Draws, then handles one key; true with `event` set when something happened.
    bool step(std::vector<Item>& items, Event& event) {
        clamp(items);
        draw(items);
        const int k = t_.key(timeout_ms);
        if (k == None) return false;
        messages.clear();
        if (k == Escape) { event = {Event::Back, {}, {}}; return true; }
        if (k == Up || k == 'k') move(items, -1);
        else if (k == Down || k == 'j' || k == '\t') move(items, +1);
        else if (k == Home || k == PageUp) { cursor_ = -1; move(items, +1); }
        else if (k == End || k == PageDown) { cursor_ = int(items.size()); move(items, -1); }
        else if (cursor_ >= 0 && cursor_ < int(items.size())) {
            auto& item = items[size_t(cursor_)];
            if (item.kind == Item::Action && (k == Enter || k == ' ')) { event = {Event::Activate, item.id, item}; return true; }
            if (item.kind == Item::Toggle && (k == Enter || k == ' ')) { item.on = !item.on; event = {Event::Changed, item.id, item}; return true; }
            if (item.kind == Item::Choice && (k == Enter || k == ' ')) { // the whole list on its own screen
                if (const auto picked = pick(item); picked && *picked != item.choice) {
                    item.choice = *picked;
                    event = {Event::Changed, item.id, item};
                    return true;
                }
                return false;
            }
            if (item.kind == Item::Choice && (k == Left || k == Right)) {
                const int n = int(item.choices.size());
                if (n == 0) return false;
                item.choice = (item.choice+(k == Left ? n-1 : 1))%n;
                event = {Event::Changed, item.id, item};
                return true;
            }
            if (item.kind == Item::Number && (k == Left || k == Right)) {
                item.number = rounded(std::clamp(item.number+(k == Left ? -item.step : item.step), item.low, item.high), item.decimals);
                event = {Event::Changed, item.id, item};
                return true;
            }
            if (item.kind == Item::Number && k == Enter) {
                const auto range = format(item.low, item.decimals)+" to "+format(item.high, item.decimals)+(item.unit.empty() ? "" : " "+item.unit);
                if (const auto typed = prompt(items, item.label+" ("+range+")", format(item.number, item.decimals), "0123456789.,")) {
                    char* end = nullptr;
                    auto text = *typed;
                    std::replace(text.begin(), text.end(), ',', '.');
                    const double v = std::strtod(text.c_str(), &end);
                    if (text.empty() || *end) messages = {"Not a number: "+*typed};
                    else if (v < item.low || v > item.high) messages = {"Out of range: "+range};
                    else { item.number = rounded(v, item.decimals); event = {Event::Changed, item.id, item}; return true; }
                }
            }
        }
        return false;
    }
    // A choice's options as a list on their own screen; the index picked, or nothing on Esc.
    std::optional<int> pick(const Item& item) {
        Menu list(t_);
        list.title = title+"  ·  "+item.label;
        list.keys = "↑↓ move   Enter choose   Esc back";
        list.label_width = 60;
        std::vector<Item> items{section(item.label), info(item.help)};
        for (size_t i = 0; i < item.choices.size(); ++i)
        {
            items.push_back(action(std::to_string(i), (int(i) == item.choice ? "● " : "○ ")+item.choices[i]));
            items.back().bullet.clear();
        }
        list.select(items, std::to_string(item.choice));
        for (;;) {
            Event e;
            if (!list.step(items, e)) continue;
            if (e.kind == Event::Back) return std::nullopt;
            if (e.kind == Event::Activate) return std::atoi(e.id.c_str());
        }
    }
    // A line of text typed by the user (`allowed` characters only; empty: any printable), or
    // nothing if cancelled with Esc.
    std::optional<std::string> prompt(std::vector<Item>& items, const std::string& question, std::string text, const std::string& allowed = {}) {
        for (;;) {
            asking_ = question+": "+text+"▏";
            draw(items);
            const int k = t_.key(-1);
            if (k == Escape) { asking_.clear(); return std::nullopt; }
            if (k == Enter) { asking_.clear(); return text; }
            if (k == Backspace) { if (!text.empty()) { do text.pop_back(); while (!text.empty() && (text.back() & 0xc0) == 0x80); } }
            else if (k >= 32 && k < 127 && (allowed.empty() || allowed.find(char(k)) != std::string::npos)) text += char(k);
        }
    }
    void select(const std::vector<Item>& items, const std::string& id) {
        for (size_t i = 0; i < items.size(); ++i) if (items[i].id == id) cursor_ = int(i);
    }

private:
    Terminal& t_;
    int cursor_ = -1;
    std::string asking_; // the prompt line while typing
    static double rounded(double v, int decimals) { const double f = std::pow(10.0, decimals); return std::round(v*f)/f; }
    void clamp(const std::vector<Item>& items) {
        if (items.empty()) { cursor_ = -1; return; }
        if (cursor_ < 0 || cursor_ >= int(items.size()) || !items[size_t(cursor_)].selectable()) {
            const int from = std::clamp(cursor_, 0, int(items.size())-1);
            cursor_ = from-1; move(items, +1);
            if (cursor_ < 0 || !items[size_t(cursor_)].selectable()) { cursor_ = from+1; move(items, -1); }
        }
    }
    void move(const std::vector<Item>& items, int direction) {
        for (int i = cursor_+direction; i >= 0 && i < int(items.size()); i += direction)
            if (items[size_t(i)].selectable()) { cursor_ = i; return; }
    }
    // An item's screen lines (notices wrap).
    std::vector<std::string> render(const Item& item, bool selected, size_t columns) const {
        constexpr size_t tag_width = 13;
        const auto tag = [&](const std::string& t) {
            if (t.empty()) return std::string(tag_width, ' ');
            return std::string(t == "live" ? green : yellow)+dim+fit(t, tag_width)+reset;
        };
        const size_t value_width = columns > label_width+tag_width+6 ? columns-label_width-tag_width-6 : 4;
        switch (item.kind) {
        case Item::Section: {
            const auto text = "── "+item.label+" ";
            std::string rule;
            for (size_t i = width(text)+2; i < columns; ++i) rule += "─";
            return {"", std::string(bold)+cyan+text+rule+reset};
        }
        case Item::Info: {
            std::vector<std::string> lines;
            for (const auto& l : wrap(item.label, columns > 6 ? columns-6 : 1)) lines.push_back(std::string(dim)+"    "+l+reset);
            return lines;
        }
        case Item::Notice: {
            const std::string mark = item.state == 1 ? std::string(yellow)+"issue"+reset : item.state == 2 ? std::string(green)+"help "+reset
                                   : std::string(cyan)+"tip  "+reset;
            const auto lines = wrap(item.label, columns > 12 ? columns-12 : 1);
            std::vector<std::string> out;
            for (size_t i = 0; i < lines.size(); ++i) out.push_back("  "+(i ? std::string(5, ' ') : mark)+"  "+lines[i]);
            return out;
        }
        case Item::Status: {
            static const char* colours[] = {green, yellow, red, dim}, *symbols[] = {"✓", "!", "✗", "·"};
            const int state = std::clamp(item.state, 0, 3);
            const auto mark = std::string(colours[state])+(item.mark.empty() ? symbols[state] : item.mark)+reset;
            return {"  "+mark+" "+fit(item.label, label_width-2)+fit(item.value, value_width+tag_width+2)};
        }
        default: break;
        }
        std::string label, value;
        switch (item.kind) {
        case Item::Toggle: label = item.label; value = item.on ? "[x] on" : "[ ] off"; break;
        case Item::Choice: label = item.label; value = "‹ "+(item.choices.empty() ? std::string() : item.choices[size_t(item.choice)])+" ›"; break;
        case Item::Number: label = item.label; value = "‹ "+format(item.number, item.decimals)+(item.unit.empty() ? "" : " "+item.unit)+" ›"; break;
        case Item::Action: label = item.bullet+item.label; break;
        default: break;
        }
        const auto row = "  "+(item.kind == Item::Action ? fit(label, label_width+value_width) : fit(label, label_width)+fit(value, value_width))+"  ";
        return {(selected ? std::string(inverse)+row+reset : row)+tag(item.tag)};
    }
    void draw(const std::vector<Item>& items) {
        const size_t columns = size_t(std::clamp(t_.columns(), 40, 140));
        const int rows = std::max(t_.rows(), 12);
        // Lines of every item; scrolled so the cursor's line is visible.
        std::vector<std::string> lines;
        int cursor_line = 0;
        for (size_t i = 0; i < items.size(); ++i) {
            if (int(i) == cursor_) cursor_line = int(lines.size());
            for (auto& l : render(items[i], int(i) == cursor_, columns)) lines.push_back(std::move(l));
        }
        // Below the items: messages (or the prompt), the selected item's help, the keys.
        std::vector<std::string> bottom;
        if (!asking_.empty()) bottom.push_back(std::string(bold)+" "+asking_+reset+"   (Enter to set, Esc to cancel)");
        for (const auto& m : messages) for (const auto& l : wrap(m, columns-2)) bottom.push_back(std::string(bold)+" "+l+reset);
        std::vector<std::string> help;
        if (cursor_ >= 0 && cursor_ < int(items.size())) help = wrap(items[size_t(cursor_)].help, columns-2);
        help.resize(2);
        const int area = std::max(rows-2-int(bottom.size())-3, 3); // title + blank, rule + help (2) + keys
        int first = 0;
        if (cursor_line+1 > area) first = cursor_line+1-area;
        if (cursor_line <= 2) first = 0; // keep the first section heading in view
        std::string out = "\x1b[H\x1b[2J";
        const auto right = title_right.empty() ? std::string() : title_right+" ";
        out += std::string(bold)+inverse+fit(" "+title, columns-width(right))+right+reset+"\r\n";
        for (int i = first; i < int(lines.size()) && i < first+area; ++i) out += lines[size_t(i)]+"\r\n";
        out += "\x1b["+std::to_string(rows-3-int(bottom.size()))+";1H";
        for (const auto& b : bottom) out += b+"\r\n";
        std::string rule;
        for (size_t i = 0; i < columns; ++i) rule += "─";
        out += std::string(dim)+rule+reset+"\r\n";
        for (const auto& h : help) out += " "+h+"\r\n";
        out += std::string(dim)+fit(" "+keys, columns)+reset;
        t_.put(out);
    }
};
}
