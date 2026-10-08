# Notes for the next release

Entries for the next GitHub release notes, in release-note wording. Move them into the release
body when publishing, then clear this file.

<!-- Below is the full body for v0.5.2, a normal release that becomes Latest and replaces 0.4.0.
Headset checks done on 2026-10-08 (Varjo Aero): X4 9.00 cockpit, on foot, external screen, shared
pose, the 60-degree turn limit and the seat position mod in the Kukri; X4 8.00 cockpit, on foot
and Ctrl+F11, with debug-events.log showing "TrackIR and Tobii trackers off, X4 8.00 code". Not
run: Ctrl+F11 on 9.00 (same code as 8.00), the new options on 8.00 (the Kukri file has the same
values there), shared pose over Steam Link. v0.5.0 and v0.5.1 stay pre-releases. -->

X4 VR 0.5.2

This is the new stable release and replaces 0.4.0. It includes everything from the pre-releases [0.5.0](https://github.com/ToffelsKater/X4_VR/releases/tag/v0.5.0) and [0.5.1](https://github.com/ToffelsKater/X4_VR/releases/tag/v0.5.1): other head trackers can no longer take over from the headset, CPU-heavy saves run at a higher frame rate, and the native Linux version of X4 is supported (experimental). Checked on X4 9.00 and 8.00 in the cockpit and on foot.

### Fixes

- No more black flashes in one eye when you turn quickly with the mouse on foot (#10).
- No image in the headset while head tracking worked: the launcher now turns off the Vulkan layers of RTSS and OBS game capture for X4. To record with OBS, use window or display capture (#21).
- The launcher warns when it runs as administrator, because X4 then starts without the VR layer (#21).

### New launcher options, all off by default

- *Steam Link / Steam Frame: fix the jittering right eye*. Both eyes of a pair are drawn from one head pose. Leave it off on other headsets, because it adds a little delay to head movement. Not yet tested over Steam Link on Windows, so please report how it looks (#4).
- *External views (F2/F3): screen fills the view*. The theater screen becomes as large as X4's field of view and stays in place when you look around. The picture has no depth, because X4 does not move the external camera with your head (#17).
- These two are part of your profile. Press *Save* in the launcher to keep them.
- *Camera further back in ships where it sits too far forward*. In the Kukri X4 places the pilot's camera too far forward for VR. The launcher writes a small X4 extension that moves the camera and the HUD back by 0.75 m. Tick it while X4 is closed. If the view in another ship looks wrong, name the ship in #22 (#23).

### Thanks

- @Cully-Curwen for the Steam Link fix from the Linux port and for reporting the Kukri camera (#22).
- @dshaughnessy5 and @coleblooded1 for the reports in #4.
- Joenyan for tracking down #21 over about 60 relaunches.

### Install or update on Windows

1. Download `X4_VR-v0.5.2.zip` below.
2. Close X4 and the launcher. Extract the zip into the folder that contains `X4.exe`, so that you get `X4 Foundations\X4_VR\X4VRLauncher.exe`. Settings and profiles stay.
3. Start `X4_VR\X4VRLauncher.exe`. First install: follow the [README](https://github.com/ToffelsKater/X4_VR#setup).

Linux: nothing changed since 0.5.1. Build from the repository as the [README's Linux section](https://github.com/ToffelsKater/X4_VR#linux-native-x4-experimental) describes.

Requirements: Windows 10 or 11, X4 9.00 or 8.00, SteamVR or your headset's OpenXR runtime (experimental).

Questions and feedback: [Discord](https://discord.gg/yDmj5bnG7n) or a [GitHub issue](https://github.com/ToffelsKater/X4_VR/issues). The launcher's *Report a bug* button collects the logs.
