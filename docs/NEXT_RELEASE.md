# Notes for the next release

Entries for the next GitHub release notes, in release-note wording. Move them into the release
body when publishing, then clear this file.

<!-- v0.5.1 is a pre-release, as v0.5.0 was. Its Windows files are those of v0.5.0 (only the
README differs); it adds the Linux fixes of #9. Before marking v0.5.1 Latest: headset check on
9.00 and 8.00. debug-events.log shows "TrackIR and Tobii trackers off, X4 <version> code", head
tracking works in the cockpit and on foot, and Ctrl+F11 toggles the theater screen. Then remove
the pre-release paragraph from its notes. -->

### No image in the headset with RTSS, OBS or administrator rights

On some systems X4 ran flat on the monitor with working head tracking, while the headset stayed in
the empty SteamVR room. Two things caused it. The Vulkan layers of RTSS and OBS game capture load
into X4 even when those programs are closed, and they could break the VR layer. The launcher now
turns both off for X4. Starting the launcher as administrator has the same effect, because Windows
then ignores the VR layer. The launcher now warns before it starts the game that way and shows it
in the Status box. Thanks to Joenyan (#21).
