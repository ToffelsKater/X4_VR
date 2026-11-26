# Notes for the next release

Entries for the next GitHub release notes, in release-note wording. Move them into the release
body when publishing, then clear this file.

<!-- Checks still owed from 0.6.0:
     - Linux Steam Frame fix (shared pose): built by the Linux CI only, waits for a Steam Frame player.
     - Flat screen distance and width fields: not checked in the headset yet.
     - Side panels on foot default 0.6 (0.4 overlapped the conversation menu once the HUD scaled):
       not checked in the headset yet.
     - Linux x4vr menu has no Side panels setting yet; it uses the defaults (ship 1, on foot 0.6).
     - 0.6.1: the HUD label check changed for 9.00 too (prefix field); not rechecked on 9.00.
     - GOG: config.xml path taken from PCGamingWiki (Documents\Egosoft\X4\config.xml), not seen on a
       GOG install yet. The VR patches were never checked against GOG's X4.exe.
     - Launcher fit to small screens: tested with a simulated 1080p at 125 % work area only.
     v0.6.2 is a pre-release with the two entries below, for a GOG player. 0.6.1 stays Latest; keep
     the entries for the next normal release. -->


- The launcher now finds X4's settings on the GOG version, which keeps `config.xml` directly in `Documents\Egosoft\X4`.
- The launcher window fits on smaller screens. At 125 % or 150 % scaling on a 1080p screen, the Play button was below the edge of the screen.
- You can pass your own arguments to X4 through the launcher. Set the user environment variable `X4VR_GAME_ARGS`, for example to `-skipintro -nocputhrottle -logfile debuglog.txt`. It replaces the default arguments, so keep `-nocputhrottle` in it: without it, X4 drops to about 12 fps while its window is not focused. (#34)
