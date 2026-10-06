// X4's Steam launch option in localconfig.vdf (tools/linux/steam_config.hpp).
#include "steam_config.hpp"
#include <cstdio>
#include <cstdlib>

namespace {
int failures = 0;
void check(bool ok, const char* what) { if (!ok) { std::printf("FAIL: %s\n", what); ++failures; } }
const std::string with_option = R"("UserLocalConfigStore"
{
	"Software"
	{
		"Valve"
		{
			"Steam"
			{
				"apps"
				{
					"1234"
					{
						"LaunchOptions"		"other game"
					}
					"392160"
					{
						"LastPlayed"		"1700000000"
						"LaunchOptions"		"\"/home/a b/x4vr-run\" %command%"
						"cloud"
						{
							"LaunchOptions"		"nested, not ours"
						}
					}
				}
			}
		}
	}
	"apps"
	{
		"392160"
		{
			"LaunchOptions"		"wrong path"
		}
	}
}
)";
const std::string without_option = R"("UserLocalConfigStore"
{
	"Software"
	{
		"valve"
		{
			"Steam"
			{
				"Apps"
				{
					"392160"
					{
						"LastPlayed"		"1700000000"
					}
				}
			}
		}
	}
}
)";
const std::string library_vdf = R"("libraryfolders"
{
	"0"
	{
		"path"		"/home/a/.local/share/Steam"
		"apps"
		{
			"228980"		"123"
		}
	}
	"1"
	{
		"path"		"/mnt/games/Steam Library"
		"label"		""
		"apps"
		{
			"392160"		"456"
		}
	}
}
)";
}

int main() {
    using namespace x4vr::steam;
    const auto current = launch_options(with_option);
    check(current && *current == "\"/home/a b/x4vr-run\" %command%", "reads X4's option (escapes, not other apps, not nested)");
    check(launch_options(without_option) == std::optional<std::string>(""), "no option yet reads as empty (keys case-insensitive)");
    check(!launch_options("\"UserLocalConfigStore\"\n{\n}\n"), "no app block");
    const auto paths = library_paths(library_vdf);
    check(paths.size() == 2 && paths[0] == "/home/a/.local/share/Steam" && paths[1] == "/mnt/games/Steam Library",
          "library folders from libraryfolders.vdf (spaces kept, nested keys ignored)");
    std::printf(failures ? "%d failures\n" : "all passed\n", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
