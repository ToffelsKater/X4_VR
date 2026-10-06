#pragma once
// X4's Steam launch option, judged for the menu: whether it runs our x4vr-run, and which account's
// counts when several Steam accounts on the machine know X4.
#include <string>
#include <vector>

namespace x4vr::linux_port {
// Ours: our x4vr-run (`run_script`, as a whole word, quoted or not) followed by %command%, with
// anything around it (settings in front, X4 arguments after).
inline bool is_ours(const std::string& option, const std::string& run_script) {
    for (auto run = option.find(run_script); run != std::string::npos; run = option.find(run_script, run+1)) {
        const auto end = run+run_script.size();
        const bool starts = run == 0 || option[run-1] == ' ' || option[run-1] == '"' || option[run-1] == '\'';
        const bool ends = end < option.size() && (option[end] == ' ' || option[end] == '"' || option[end] == '\'');
        if (starts && ends && option.find("%command%", end) != std::string::npos) return true;
    }
    return false;
}
// state: 0 none set, 1 ours, 2 an x4vr-run elsewhere (an older build), 3 something else;
// `accounts` = how many accounts know X4. Ours in any account wins (another account on the machine
// may have its own), then 2, 3, 0.
struct LaunchOption { int state = 0; int accounts = 0; std::string value; };
inline LaunchOption judge_launch_options(const std::vector<std::string>& per_account, const std::string& run_script) {
    LaunchOption result;
    for (const auto& value : per_account) {
        ++result.accounts;
        const int state = value.empty() ? 0 : is_ours(value, run_script) ? 1 : value.find("x4vr-run") != std::string::npos ? 2 : 3;
        static constexpr int rank[] = {0, 3, 2, 1}; // by state
        if (result.accounts == 1 || rank[state] > rank[result.state]) { result.state = state; result.value = value; }
    }
    return result;
}
}
