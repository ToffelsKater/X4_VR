# Notes for the next release

Entries for the next GitHub release notes, in release-note wording. Move them into the release
body when publishing, then clear this file.

<!-- Headset check owed: the Steam Frame fix below is built by the Linux CI only and waits for a
     Steam Frame player to confirm it. -->

- Linux, Steam Frame: with "Shared pose per eye pair" on, the right eye still jittered when X4 ran
  below the headset's refresh rate. Both eyes of a pair now keep one head pose in that case too.
- Moving X4's *Head Motion Smoothing* slider during a session turned X4's head smoothing back
  on until the game restarted, which made head movement feel floaty. Even the slider's lowest
  setting smooths a lot. The mod now turns smoothing off again whenever the slider changes it.
- *Report a bug*: Windows Explorer showed the bug-report zip as empty. The files were there, but
  stored under a `./` prefix that Explorer doesn't list. They now sit in a named folder.
- Launcher: new fields for the flat screen's distance and width (menus, map, cutscenes,
  Ctrl+F11), 0.5 to 10 m. They are saved with the profile and apply while you play (#25).
- The launcher now also turns off Overwolf's Vulkan game capture layer for X4, which was
  loaded into the game in #28. Closing Overwolf is still best, as it has other hooks.
- *Report a bug*: when the VR layer never loaded, the report said `GPU: unknown`. It now lists
  the graphics cards Windows reports and notes that the layer did not report (#21).
