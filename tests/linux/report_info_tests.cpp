// The bug report's readings (tools/linux/report_info.hpp): Steam's and SteamVR's versions from
// their manifests, where the mod's log stopped (issue #7's log among them), GPU vendors.
#include "report_info.hpp"
#include <cstdio>
#include <string>

namespace {
int failures = 0;
void check(bool ok, const char* what) { if (!ok) { std::printf("FAIL: %s\n", what); ++failures; } }
}

int main() {
    using namespace x4vr::report;

    const std::string manifest = R"("AppState"
{
	"appid"		"250820"
	"name"		"SteamVR"
	"buildid"		"20412345"
	"UserConfig"
	{
		"language"		"english"
		"BetaKey"		"beta"
	}
	"MountedConfig"
	{
		"language"		"english"
		"BetaKey"		"beta"
	}
}
)";
    check(vdf_value(manifest, {"AppState", "buildid"}) == "20412345", "buildid at the top level");
    check(vdf_value(manifest, {"appstate", "mountedconfig", "betakey"}) == "beta", "keys case-insensitive");
    check(!vdf_value(manifest, {"AppState", "BetaKey"}), "BetaKey only inside its block");
    check(!vdf_value(manifest, {"AppState", "language"}), "a nested key isn't a top-level one");
    check(steamvr_version(manifest) == "build 20412345, branch beta", "SteamVR on its beta");
    check(steamvr_version(R"("AppState" { "buildid" "7" "UserConfig" { "language" "english" } })") == "build 7, branch public",
          "SteamVR without a beta");
    check(steamvr_version("") == "not found", "SteamVR not installed");

    const std::string client = R"("ubuntu12"
{
	"version"		"1759461205"
	"bootstrapper_version"		"1.0.0"
}
)";
    check(steam_client_version(client, "") == "version 1759461205, stable", "stable client");
    check(steam_client_version(client, "publicbeta\n") == "version 1759461205, beta publicbeta", "client beta");
    check(steam_client_version("", "") == "version ?, stable", "no manifest");

    // Issue #7: the latest run stopped in the instance extensions query, X4 still running.
    const std::string issue7 =
        "== 2026-10-06 14:51:30 x4vr-run\n"
        "SteamVR is running\n"
        "X4VR bootstrap: initialize begin\n"
        "X4VR bootstrap: initialize complete\n"
        "X4VR bootstrap: instance extensions begin\n"
        "== 2026-10-06 14:59:12 X4 exited with status 130\n"
        "x4vr: X4's VR settings saved (config.xml.x4vr-vr), 2D settings restored\n";
    const auto previous = last_run(issue7);
    check(previous.unfinished == "X4VR bootstrap: instance extensions begin", "old-style begin line: unfinished");
    check(previous.exit == "X4 exited with status 130", "its exit");

    const auto current = last_run(
        "== 2026-10-06 14:51:30 x4vr-run\n"
        "X4VR bootstrap: instance extensions begin\n"
        "== 2026-10-06 14:59:12 X4 exited with status 130\n"
        "== 2026-10-06 14:59:26 x4vr-run\n"
        "X4VR bootstrap: initialize begin (thread 4242, 14:59:40.100)\n"
        "X4VR bootstrap: initialize complete (12 ms)\n"
        "X4VR bootstrap: instance extensions begin (thread 4242, 14:59:40.113)\n"
        "X4VR bootstrap: instance extensions still running after 10 s (thread 4242): the SteamVR call hasn't returned\n");
    check(current.unfinished && current.unfinished->find("still running after 10 s") != std::string::npos, "only the latest run counts");
    check(!current.exit, "the latest run hasn't exited");

    check(last_run("== 2026-10-06 14:59:26 x4vr-run\n"
                   "X4VR bootstrap: instance extensions waits (thread 4300, 14:59:40.200) for thread 4242 (instance extensions)\n")
              .unfinished.has_value(), "a step waiting for another thread's");
    check(!last_run("== 2026-10-06 14:59:26 x4vr-run\n"
                    "X4VR bootstrap: instance extensions begin (thread 4242, 14:59:40.113)\n"
                    "X4VR bootstrap: instance extensions complete (850 ms)\n"
                    "X4VR layer: device created on AMD Radeon RX 7900 XTX, VR queue: shared\n")
               .unfinished, "a run that got past the bootstrap");
    check(!last_run("== 2026-10-06 14:59:26 x4vr-run\nWARNING: SteamVR doesn't seem to be running\n").unfinished, "no bootstrap lines");
    check(!last_run("").unfinished, "empty log");

    // Steam Frame link problems in SteamVR's logs (vrserver.txt): Error 17, the link timing out.
    const auto link = link_problems(
        "Tue Oct 06 2026 14:52:01.120 - [vrlink] Session started\n"
        "Tue Oct 06 2026 14:52:03.441 - vrlink: incompatible protocol settings on host (17)\n"
        "Tue Oct 06 2026 14:52:04.000 - Loaded 3 drivers (17)\n"
        "Tue Oct 06 2026 14:53:10.270 - driver_vrlink: Connection inactive for 30.27 > 30 seconds. Sending quit event.\n");
    check(link.size() == 2, "Error 17 and the timeout found, a plain \"(17)\" not");
    check(link.size() == 2 && link[0].find("(17)") != std::string::npos && link[1].find("inactive") != std::string::npos, "in order");
    std::string many;
    for (int i = 0; i < 20; ++i) many += "DEAUTH_LEAVING "+std::to_string(i)+"\n";
    const auto last = link_problems(many, 3);
    check(last.size() == 3 && last.back() == "DEAUTH_LEAVING 19", "only the last matches kept");
    check(link_problems("[Info] - VR compositor started\n").empty(), "a healthy log");
    const auto start = link_problems(
        "x [Info] - Unable to load driver oculus_legacy. Primary driver shared library not found on filesystem\n"
        "x [Error] - Unable to load driver oculus_legacy because of error VRInitError_Init_FileNotFound(103). Skipping.\n"
        "x [Info] - error VRInitError_Init_LowPowerWatchdogNotSupported when initing driver lighthouse from x.so.\n"
        "x [Info] - No connected devices found. Returning best error VRInitError_Driver_WirelessHmdNotConnected\n"
        "x [Info] - Allowing driver load failure VRInitError_Driver_WirelessHmdNotConnected for application 1: vrmonitor\n"
        "x [Info] - Unable to load driver vrlink. Primary driver shared library not found on filesystem (for this architecture): .../linux32/driver_vrlink.so.\n");
    check(start.size() == 1 && start[0].find("driver vrlink") != std::string::npos, "SteamVR's start and other headsets' drivers left out, vrlink kept");

    check(gpu_vendor("0x10de\n") == "NVIDIA" && gpu_vendor("0x1002") == "AMD" && gpu_vendor("0x8086") == "Intel", "known vendors");
    check(gpu_vendor("0x1af4") == "0x1af4" && gpu_vendor("") == "?", "others as they are");

    // SteamVR's log: version and headset (the Steam Frame's lines, from issue #7's report).
    const auto frame = steamvr_log(
        "Wed Oct 07 2026 16:02:07.120913 [Info] - vrserver 2.17.9 startup with PID=11472, config=/home/x/.local/share/Steam/config\n"
        "Wed Oct 07 2026 16:02:42.549348 [Info] - vrserver 2.17.10 startup with PID=12100, config=/home/x/.local/share/Steam/config\n"
        "Wed Oct 07 2026 16:02:43.833075 [Info] - vrlink: ReceivedHMDStaticProps. Model Number: Deckard MP, Serial Number: FPRAH619000D1 (5)\n"
        "Wed Oct 07 2026 16:02:44.025343 [Info] - vrlink: ManufacturerName Valve\n"
        "Wed Oct 07 2026 16:02:44.025370 [Info] - vrlink: ModelNumber Deckard MP\n");
    check(frame.version == "2.17.10", "SteamVR's version: the newest startup");
    check(frame.headset == "Valve Deckard MP", "headset: maker and model");
    check(frame.headset.find("FPRAH") == std::string::npos, "no serial number");
    check(steamvr_log("x - HMD: Model Number: Valve Index\nx - ManufacturerName Valve\n").headset == "Valve Index", "model already named after the maker");
    check(steamvr_log("x - vrserver 2.18.2 startup with PID=1\n").headset.empty(), "no headset in the log");
    check(os_name("NAME=\"Nobara Linux\"\nVERSION_ID=44\nPRETTY_NAME=\"Nobara Linux 44 (KDE Plasma Desktop Edition)\"\n") ==
          "Nobara Linux 44 (KDE Plasma Desktop Edition)", "os-release: PRETTY_NAME, quotes removed");
    check(os_name("NAME=Arch\n") == "Arch" && os_name("").empty(), "os-release: NAME without PRETTY_NAME; nothing");

    // CPU, memory, compositor, X4's compatibility tool.
    check(cpu("processor\t: 0\nmodel name\t: 13th Gen Intel(R) Core(TM) i7-13700K\n\nprocessor\t: 1\nmodel name\t: 13th Gen Intel(R) Core(TM) i7-13700K\n") ==
          "13th Gen Intel(R) Core(TM) i7-13700K, 2 threads", "cpu: model and threads");
    check(cpu("").empty(), "cpu: nothing");
    check(memory("MemTotal:       65536000 kB\nMemFree:        1000 kB\n") == "62.5 GiB", "memory in GiB");
    check(compositor({"systemd", "Xwayland", "kwin_wayland", "plasmashell"}) == "kwin_wayland, Xwayland", "compositor and Xwayland");
    check(compositor({"gamescope", "Hyprland"}) == "Hyprland, gamescope" && compositor({"bash"}).empty(), "gamescope too; none known");
    const std::string config_vdf = R"("InstallConfigStore"
{
	"Software"
	{
		"Valve"
		{
			"Steam"
			{
				"CompatToolMapping"
				{
					"0"
					{
						"name"		"proton_experimental"
					}
					"392160"
					{
						"name"		"proton_9"
						"config"		""
						"priority"		"250"
					}
				}
			}
		}
	}
}
)";
    check(x4_compat_tool(config_vdf) == "proton_9", "X4's compatibility tool");
    check(x4_compat_tool(R"("InstallConfigStore" { "Software" { "Valve" { "Steam" { "CompatToolMapping" { "0" { "name" "proton_9" } } } } } })").empty(),
          "only the default for other games: X4 native");

    if (failures) return 1;
    std::printf("report_info: all passed\n");
}
