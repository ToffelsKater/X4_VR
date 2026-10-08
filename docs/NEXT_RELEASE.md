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

### Steam Link and Steam Frame: option against the jittering right eye

Over Steam Link and on the Steam Frame the right eye jittered on head turns, because SteamVR's
streaming corrects both eyes with the left eye's head pose. The launcher has a new option,
*Steam Link / Steam Frame: fix the jittering right eye*. With it both eyes of a pair are drawn
from one head pose. It is off by default, and other headsets should leave it off. The method
comes from Cully-Curwen's Linux port, where it removed the jitter on a Steam Frame. On Windows
it is untested with these headsets so far, so please report how it works (#4).

### External views can stay in VR

The external camera (F2/F3) went to the flat theater screen since theater mode was added. The
new launcher option *External views (F2/F3) stay in VR* keeps them in the headset. Fullscreen
menus and cutscenes still use the theater screen. Experimental and off by default (#17).
