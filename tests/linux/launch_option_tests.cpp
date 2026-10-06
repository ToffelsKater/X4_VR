// X4's Steam launch option as the menu judges it (tools/linux/launch_option.hpp): every form that
// has come up, and several Steam accounts on one machine.
#include "launch_option.hpp"
#include <cstdio>

namespace {
int failures = 0;
void check(bool ok, const char* what) { if (!ok) { std::printf("FAIL: %s\n", what); ++failures; } }
}

int main() {
    using x4vr::linux_port::is_ours;
    using x4vr::linux_port::judge_launch_options;
    const std::string run = "/home/u/x4/result/bin/x4vr-run";
    check(is_ours(run+" %command%", run), "plain");
    check(is_ours("X4VR_THEATER_OVERLAY=1 "+run+" %command%", run), "settings in front");
    check(is_ours("gamemoderun "+run+" %command% -debug all", run), "wrapper in front, X4 arguments after");
    check(is_ours("\""+run+"\" %command%", run), "double-quoted path");
    check(is_ours("'"+run+"' %command%", run), "single-quoted path");
    check(!is_ours(run, run), "without %command%");
    check(!is_ours("%command% "+run, run), "%command% before it");
    check(!is_ours("/opt"+run+" %command%", run), "another path ending in ours");
    check(!is_ours(run+"2 %command%", run), "another file starting with ours");
    check(!is_ours("/other/x4vr-run %command%", run), "another x4vr-run");
    const std::string spaced = "/home/a b/x4vr-run";
    check(is_ours("\""+spaced+"\" %command%", spaced), "path with a space, quoted");

    auto o = judge_launch_options({}, run);
    check(o.state == 0 && o.accounts == 0, "no account knows X4");
    o = judge_launch_options({""}, run);
    check(o.state == 0 && o.accounts == 1, "not set");
    o = judge_launch_options({"-skipintro"}, run);
    check(o.state == 3 && o.value == "-skipintro", "something else");
    o = judge_launch_options({"/old/x4vr-run %command%"}, run);
    check(o.state == 2, "an older x4vr-run");
    o = judge_launch_options({"-skipintro", run+" %command%", ""}, run);
    check(o.state == 1 && o.accounts == 3 && o.value == run+" %command%", "ours in one of three accounts wins");
    o = judge_launch_options({"", "/old/x4vr-run %command%", "-skipintro"}, run);
    check(o.state == 2, "an older x4vr-run outranks something else and none");

    if (failures) return 1;
    std::printf("launch option: all checks passed\n");
    return 0;
}
