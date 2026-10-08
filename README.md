# X4 VR

> [!IMPORTANT]
> [Join the X4 VR Discord](https://discord.gg/yDmj5bnG7n) for setup help, bug reports and
> development updates.

Native stereoscopic VR with 6DOF head tracking for X4: Foundations, on any SteamVR (OpenVR) headset,
and experimentally through a headset's own OpenXR runtime.

This is an unofficial fan project. It is not affiliated with or endorsed by Egosoft.

> [!WARNING]
> Some antivirus programs, including Windows Defender, have flagged the download as
> "Trojan:Win32/Sabsik.FL.A!ml". That is a false positive. The "!ml" means Defender's cloud
> machine learning judged the file by its behavior, which happens a lot with new programs that
> aren't signed. All of the code is in this repository, and you can
> [build it yourself](#building-from-source) instead.

> [!TIP]
> Quest players: stream with Virtual Desktop, not Steam Link. Over Steam Link the right eye
> jitters when you turn your head, with both the OpenVR and the OpenXR option. See
> [Limitations](#limitations).

> [!NOTE]
> Linux: a native port for the Linux version of X4 is available (experimental), see
> [Linux (native X4, experimental)](#linux-native-x4-experimental).

## What works

- Flying from the cockpit in stereo with full head tracking. The world, the cockpit and the HUD
  all follow your head, including leaning.
- The HUD can be moved further away. Out of the box X4 places it about 15 cm in front of your
  eyes, which is uncomfortable in VR. The launcher builds a small X4 extension from your own
  game files that pushes it back while keeping its apparent size.
- The main menu and fullscreen menus (map, inventory, trading and so on) appear on a flat
  virtual screen in front of you, with the mouse cursor on it.
- Walking on foot, in stereo with head tracking. Turning with the mouse is compensated so both
  eyes stay aligned during the turn.
- Ctrl+F11 switches to the flat screen at any time. Use it for anything that doesn't work in VR
  yet.
- A launcher with profiles, a live status panel, a check of X4's graphics settings and a
  *Report a bug* button.

## What you need

- Windows 10 or 11 (64-bit), Steam, and X4: Foundations 9.00 or 8.00 (see [Limitations](#limitations)).
- SteamVR plus your headset's own software, for example Varjo Base with SteamVR support
  enabled. With the experimental OpenXR option, the headset's OpenXR runtime replaces SteamVR.
- A GPU that can hold your headset's refresh rate (for example 90 fps). Every displayed frame
  renders one eye.
- An NVIDIA GPU is recommended, because the setup below uses DSR for a high render resolution.
  AMD users can try Virtual Super Resolution (untested).
- Only to build from source instead of using the download: Visual Studio 2022 with the
  *Desktop development with C++* workload (it includes CMake), and Git. Python 3 is optional
  and only runs extra self-tests.

It was developed and tested with X4 9.00 from Steam, a Varjo Aero (Varjo Base + SteamVR), an
NVIDIA RTX 3090 holding a steady 90 fps at 3840×2160, and Windows 11. Other SteamVR headsets
(Index, Vive, Quest through Virtual Desktop, and so on) should work but haven't been tested by
the developer. Players report Quest 3 working through Virtual Desktop; Steam Link makes the right eye jitter
(see [Limitations](#limitations)). X4 8.00 is supported too; on versions the mod doesn't know, some parts
fall back to safer behavior (see [Limitations](#limitations)).

OpenXR is available as an experimental option in the launcher. It lets the mod use a headset's
own OpenXR runtime, for example Varjo Base's, without going through SteamVR. On a Varjo Aero it
behaves the same as the SteamVR path. SteamVR (OpenVR) stays the default; see
[VR runtime](#vr-runtime-openvr-or-openxr-experimental). Updates are posted in the Discord's
#openxr-dev-updates channel.

## Setup

### 1. Install

Download the newest `X4_VR-<version>.zip` from the
[Releases page](https://github.com/ToffelsKater/X4_VR/releases). Extract it into your X4
installation folder, the one that contains `X4.exe`, so that you end up with
`X4 Foundations\X4_VR\X4VRLauncher.exe`. Nothing else needs installing.

Windows may show "Windows protected your PC" the first time you start the launcher, because the
program isn't signed. Click *More info* and then *Run anyway*.

On its first start the launcher points X4's head-tracker setting, the per-user registry value
`HKCU\Software\FreeTrack\FreeTrackClient\Path`, at `X4_VR\build\Release`. That is where X4 looks
for a FreeTrack head tracker. If you already use opentrack or TrackIR, the launcher leaves that
value alone, and you switch it with the launcher's *Fix head-tracking path* button. The old value
is backed up, and the uninstaller restores it. While X4 loads the mod's head tracker, the mod turns
off X4's TrackIR and Tobii head tracking, which would otherwise replace the headset pose.

#### Building from source

Clone the repository into your X4 installation folder, the one that contains `X4.exe`:

```bat
cd "C:\Program Files (x86)\Steam\steamapps\common\X4 Foundations"
git clone https://github.com/ToffelsKater/X4_VR.git
```

Then run the installer from PowerShell:

```bat
powershell -ExecutionPolicy Bypass -File X4_VR\scripts\install.ps1
```

The installer downloads the pinned dependencies (OpenVR SDK, OpenXR SDK, Vulkan headers, MinHook), builds
everything and runs the self-tests; add `-SkipTests` to skip the tests. It then points the
per-user registry value `HKCU\Software\FreeTrack\FreeTrackClient\Path` at
`X4_VR\build\Release`, which is where X4 looks for a FreeTrack head tracker. If a different
FreeTrack or opentrack path was set, it is backed up, and the uninstaller restores it. Finally
it creates the settings file `reports\captures\stereo.txt`. When it's done, the launcher
`X4VRLauncher.exe` sits directly in the `X4_VR` folder.

The project used to be called X4_Rebirth. An existing `X4_Rebirth` folder keeps working, and
`git pull` in it still reaches the renamed repository.

### 2. Raise the render resolution with NVIDIA DSR

The headset gets the image X4 renders. At monitor resolution (1920×1080) that looks blurry in
VR, so let the game render at a higher resolution:

1. Open NVIDIA Control Panel > 3D Settings > Manage 3D settings > Global Settings.
2. Under DSR - Factors, tick 4.00x (native resolution), which turns 1920×1080 into 3840×2160.
   You can also tick DL 2.25x. With a 1440p monitor, 2.25x (3840×2160) is the sensible choice;
   4x (5120×2880) is very heavy.
3. Leave DSR - Smoothness at its default. It only affects the monitor view; the headset gets
   the full-resolution image.
4. Apply.

### 3. Set X4's options

Set these once in X4's Options menu; X4 saves them.

Options > Controls > Head Tracking Support:

| Setting | Value | Note |
| --- | --- | --- |
| OpenTrack Support | On | Enables X4's FreeTrack support, which the mod uses. "Waiting for OpenTrack connection" is normal. |
| FreeTrack > Head Rotation Factor | 100 % | Required: the mod expects 1:1. |
| FreeTrack > Head Position Factor | 100 % | Required. |
| FreeTrack > Head Motion Smoothing | any | The mod turns smoothing off internally, which the slider cannot do. |

Options > Display Settings:

| Setting | Value | Note |
| --- | --- | --- |
| Display Mode | Fullscreen | DSR resolutions only exist in fullscreen. |
| Resolution | 3840×2160 | Appears after enabling DSR. You can change it while playing. |
| Anti-Aliasing | None | A non-temporal mode (FXAA/MSAA/SSAA) is fine. Avoid *Temporal*. |
| AMD FSR | Off | Temporal upscalers mix left- and right-eye frames, which shows as ghosting. |
| NVIDIA DLSS | Your choice | Works, tested up to Ultra Performance. Keep DLSS frame generation off: it blends frames of different eyes. |
| VSync | Off | SteamVR paces the frames. |
| Frame Rate Limit | Off | The mod paces the game to the headset, so a limit adds nothing, and one under 180 fps defeats pair mode. The same goes for NVIDIA Control Panel's Max Frame Rate. |
| FOV | maximum (120°) | Required. The eye mapping is calibrated for it (see `game_tan_y` below). |

Options > Graphics Settings:

| Setting | Value |
| --- | --- |
| Chromatic Aberration | Off (recommended in VR) |
| Distortion | Off (recommended in VR) |
| Everything else | Whatever still holds a steady 90 fps |

You don't have to check the display and graphics settings by hand. The launcher reads X4's
`config.xml` and lists anything that doesn't match. Its *Fix X4 settings* button corrects them
after backing up `config.xml`, and only works while X4 is closed. The head-tracking factors
aren't stored in that file, so set those in the game.

### 4. Move the HUD back

Start `X4VRLauncher.exe` in the `X4_VR` folder while X4 is closed. In the *HUD distance* box,
enter a factor and press *Apply*. At 2.5 the HUD sits 2.5 times further away and looks the same
size; any value from 1 to 6 works. *Remove* takes the HUD back to X4's default.

This writes the extension `extensions\x4vr_hud` in your X4 folder. It is generated from your
own game files, so nothing from Egosoft is part of this repository, and savegames don't depend
on it. When a game update changes those files, the launcher rebuilds the extension the next time
you press *Play X4 in VR*. If the new files no longer look as expected, it removes the extension
and tells you.

### 5. First launch

1. Start your headset software and SteamVR. Steam must be running too.
2. Start `X4VRLauncher.exe`. Don't start X4 from the Steam library; launched that way the game
   runs flat.
3. Check the launcher's Status box: SteamVR should be running, and the head-tracking DLL path
   and the build should both read "ok". The X4 settings box should say that all settings
   match.
4. Press *Play X4 in VR*.
5. The main menu appears on the virtual screen. Load your game, sit comfortably, look straight
   ahead and press Ctrl+F12 to recenter. The view also recenters on its own when head tracking
   starts.

Quit X4 normally when you're done.

## Playing

| Key | Action |
| --- | --- |
| Ctrl+F12 | Recenter the view. Also moves the virtual screen in front of you. |
| Ctrl+F11 | Switch to the flat virtual screen and back. |

Always recenter with Ctrl+F12. X4's own *Reset Head Tracking* key breaks the calibration. After
a SteamVR *Reset seated position*, press Ctrl+F12 again.

The game switches between the stereo view and the virtual screen by itself. Fullscreen menus
and cutscenes go to the screen, and flying and walking stay in stereo. The screen stands 2 m in
front of you and is 2.2 m wide. In the stereo view the mouse cursor floats 5 m ahead, in the
direction the game points it.

Ctrl+F11 is the way out of anything that doesn't work in VR yet, such as an unusual camera or a
menu that is hard to read in 3D. Press it again to return to VR.

The launcher stays open while you play. Changes you make in it apply within half a second, and
it shows the frame rate the headset is getting. It keeps profiles with your VR mode, world
scale, prediction, stutter protection and the X4 resolution you want: type a name and press
*Save*. It comes with two, *Default* and *Pair 90 Hz (experimental)*.

You can also start the game without the launcher:
`powershell -ExecutionPolicy Bypass -File X4_VR\scripts\play.ps1`

Keep X4 focused on the desktop. Clicking into another window can throttle the game.

### VR runtime: OpenVR or OpenXR (experimental)

The launcher's *VR runtime* box picks how frames reach the headset. It applies the next time X4
starts.

- *OpenVR (SteamVR)* is the default and the tested path.
- *OpenXR (experimental)* uses Windows' active OpenXR runtime, for example Varjo Base's own
  runtime, which skips SteamVR. SteamVR doesn't need to run then. Overlays that live in SteamVR
  (dashboard, fpsVR, OVR Toolkit) don't show in a native OpenXR runtime. If something looks
  wrong, switch back to OpenVR.

## Updating

Close X4 and the launcher first. If you installed the download, extract the new zip into the X4
folder and let it replace the files in `X4_VR`. If you built from source, run this in the X4
installation folder:

```bat
cd X4_VR
git pull
powershell -ExecutionPolicy Bypass -File scripts\install.ps1
```

Your settings, saved profiles and HUD extension stay as they are.

## Fine-tuning (optional)

`X4_VR\reports\captures\stereo.txt` is re-read every half second while you play. The launcher
writes `stereo`, `pair`, `ipd_scale`, `predict` and `async_submit` for you. The other values
are calibrated for X4 9.00; keys you leave out use their defaults.

| Key | Default | Meaning |
| --- | --- | --- |
| `ipd_scale` | 1 | Eye-separation multiplier. Larger makes the world feel smaller. |
| `pos_scale` | 3.6 | Converts head movement into X4's units (1:1 in metres). |
| `yaw_gain`, `pitch_gain`, `roll_gain` | 2.1177, 2.1177, 3.1416 | Undo X4's internal angle scaling. |
| `delay` | 2 | Frames between reading a pose and showing that frame. |
| `delay_walk` | 1 | The same on foot, where X4 uses the head pose sooner. |
| `predict` | 0.035 | Pose prediction in seconds. |
| `game_tan_y` | 0.8675 | Tangent of half the game's vertical FOV; 0.8675 matches FOV = 120°. |
| `stereo` | 1 | 0 = mono (the same image in both eyes). |
| `recenter` | 0 | Changing this number also recenters. |
| `async_submit` | 1 | Frames go to SteamVR from a separate thread, so a game stutter repeats the last image instead of flashing. 0 = old behaviour. |
| `pair` | 0 | Experimental: render both eyes back to back for 90 Hz per eye. Needs the game at 180 fps. |
| `theater` | 1 | Virtual screen: 1 = for fullscreen menus and cutscenes, 0 = never, 2 = always. |
| `theater_distance`, `theater_width` | 2, 2.2 | Distance and width of the virtual screen in metres. |
| `cursor`, `cursor_distance` | 1, 5 | Mouse cursor on (1) or off (0), and how far it floats in the stereo view, in metres. |
| `turn_comp` | 1 | Mouse-turn compensation: 1 = on foot, 0 = off, 2 = also in the cockpit (there it keeps the world aligned during ship turns but shifts the cockpit interior instead). |

## Smoother frames (optional)

None of these are required. They target short hitches rather than the average frame rate, so
keep what helps on your system.

| Where | Setting | Why |
| --- | --- | --- |
| NVIDIA Control Panel > Manage 3D settings > Program Settings > X4 | Power management mode: Prefer maximum performance | Keeps the GPU clock from dropping between frames. |
| Same place | Shader Cache Size: 10 GB or Unlimited | X4 keeps compiling shaders while you play. A bigger cache keeps them for the next session. |
| Control Panel > Power Options | High performance | Avoids CPU core parking, a known cause of hitches at 90 Hz. |
| Your mouse software | Polling rate 250–500 Hz | Players report less camera stutter in X4 than at 1000 Hz. |
| X4's options | A longer autosave interval | Only if you notice a hitch at every autosave. |

Also close tools that hook the game's graphics: RTSS (MSI Afterburner's on-screen display), OBS
game capture, Overwolf, and on some systems the Steam overlay.

## Limitations

Each eye gets 45 Hz, because frames alternate between the eyes. SteamVR reprojection keeps
head rotation smooth, but fast head movement shows some parallax judder on nearby objects. On
foot, fast mouse turns can still look slightly soft. The experimental `pair=1` mode gives 90 Hz
per eye but needs about twice the GPU power, and if the game can't hold 180 fps it shows dark
flashes.

The HUD is drawn into the rendered image, so it has no VR layer of its own. The HUD distance
extension moves the cockpit HUD, but small popup menus in the cockpit, such as the interaction
menu, stay at X4's original close distance.

On wide-FOV headsets there is a small black band at the very bottom of the view, because X4's
maximum FOV is a bit smaller than the Varjo Aero's. Quest 3 players see it too.

Quest over Steam Link: the right eye jitters on head movement while the left eye stays smooth.
Each eye is rendered at a different moment and sent with its own head pose, but Steam Link
corrects both eyes with the left eye's pose. The OpenXR option doesn't help, because SteamVR's
OpenXR runtime goes through Steam Link too. Use Virtual Desktop instead: it corrects each eye
with its own pose, with both the OpenVR and the OpenXR option.

VR only works when X4 is started through the launcher or `play.ps1`. A normal Steam launch runs
the game flat.

X4 9.00 and 8.00 are supported. To play an older version, pick it in Steam under X4's
Properties > Game Versions & Betas, then start it through the launcher, which rebuilds the HUD
distance extension for that version's files. The mod finds the X4 code it relies on by its
bytes, not by address, and knows the code of both versions. It changes three spots in memory at
runtime (one allows leaning backwards, two allow head tracking on foot) and hooks X4's head
tracker to pick each frame's eye. On a version whose code it doesn't know, it leaves that code
alone: leaning backwards stays blocked, walking is shown on the virtual screen, or eyes are
picked at the tracker read (occasional stutter). The log says which (`patched` or `signature
mismatch`). Developers can check any `X4.exe` without starting it:
`build\Release\code_scan_tests.exe <path to X4.exe>`. The calibration values were measured on
9.00.

## Troubleshooting

Logs are written to `X4_VR\reports\captures\debug-*\debug-events.log`; search for `X4VR`.

| Problem | Check |
| --- | --- |
| No head tracking, or nothing in the headset | OpenTrack Support is On and X4 was started with the launcher or `play.ps1`. If the launcher's Status box says the head-tracking DLL path is not set, press *Fix head-tracking path* and start X4 again. The log should show `X4VR freetrack: first headset pose delivered`. |
| Headset shows only SteamVR's grey room | SteamVR must be running before the launch. The log should show `X4VR presenter: first stereo pair submitted`. |
| Blurry | Display Mode Fullscreen at 3840×2160 (DSR enabled). |
| Low frame rate or judder | Use a smaller DSR factor (e.g. 2560×1440) or lower graphics settings; 90 fps is needed. For short hitches, see [Smoother frames](#smoother-frames-optional). |
| Quest: only the right eye jitters when turning the head | Stream with Virtual Desktop instead of Steam Link (see [Limitations](#limitations)). |
| World too big or too small | Adjust World scale in the launcher (`ipd_scale` in `stereo.txt`). |
| View off-center | Look straight ahead and press Ctrl+F12. |
| HUD too close or too far | Change the factor in the launcher's HUD distance box (X4 closed) and press *Apply*. |
| A view or menu doesn't work in VR | Press Ctrl+F11 for the virtual screen, and Ctrl+F11 again to return. |
| Walking shows the virtual screen instead of stereo | The log should show `on-foot head-pose zeroing patched` and `on-foot camera offset patched`. A `signature mismatch` means X4 changed that code in your version, so walking isn't supported there yet. |
| Game froze at startup (rare) | Close it and launch again. Disabling overlay hooks (Overwolf, OBS game capture) can help. |

## Reporting a bug

Press *Report a bug* in the launcher. It asks first, then packs your VR settings, the recent
logs, the newest crash dump, X4's `config.xml` and a short system summary (Windows version, GPU,
X4 version, SteamVR, installed extensions) into `reports\captures\bug-report-<date>.zip`. It
shows the zip in Explorer and opens a new GitHub issue with the system summary already filled
in. Describe what happened and drag the zip into the issue.

The files contain paths from your PC, including your Windows user name, so look through the zip
before you attach it. Creating the issue needs a GitHub account.

## Uninstall

Close X4 and the launcher, then run this in the X4 installation folder:

```bat
powershell -ExecutionPolicy Bypass -File X4_VR\scripts\uninstall.ps1
```

This restores or removes the FreeTrack registry value and removes the HUD distance extension.
Then delete the `X4_VR` folder. If you like, set OpenTrack Support back to Off and restore your
display settings.

## Linux (native X4, experimental)

> **Experimental.** Tested on two setups so far (Steam Frame with an AMD GPU, and with an NVIDIA
> RTX 4090; X4 9.00). Expect rough edges.

A port for the native Linux version of X4 9.00, through SteamVR. It covers stereo in the cockpit
and on foot, head tracking, the virtual screen for menus, the mouse cursor and the HUD distance.
OpenXR isn't supported on Linux yet. It was tested with a Steam Frame on an AMD GPU and on an
NVIDIA RTX 4090; Intel GPUs should work but are untested. Got it running? Please post in
[Working on Linux](https://github.com/ToffelsKater/X4_VR/issues/8) with the file from *Make a bug
report* attached: it lists your GPU, driver, headset and system, so that issue tracks which setups
work. A problem gets its own new issue. The Windows build is unchanged.

### What you need

- X4: Foundations from Steam (the native Linux version), started once.
- SteamVR with your headset working. Steam from your distribution or Valve, not the Flatpak.
- To build: Git, plus either Nix (NixOS) or GCC 13+ / Clang 16+, CMake 3.24+ and the Vulkan
  headers from your distribution (e.g. `vulkan-headers`, or `libvulkan-dev` on Debian and Ubuntu).

### Install

**With CMake (any distribution):**

```bash
git clone https://github.com/ToffelsKater/X4_VR.git ~/x4vr-src
cd ~/x4vr-src
cmake -S . -B build -DX4VR_LINUX=ON -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$HOME/.local
cmake --build build -j && cmake --install build
```

The first `cmake` downloads Valve's OpenVR sources (about 1 GB), which are built into the mod.
The menu is then `~/.local/bin/x4vr`.

**With Nix (NixOS), from your channel's nixpkgs:**

```bash
git clone https://github.com/ToffelsKater/X4_VR.git ~/x4vr-src
cd ~/x4vr-src
nix-build linux/nix
```

The menu is then `~/x4vr-src/result/bin/x4vr`. To have it installed system-wide instead, add
`(pkgs.callPackage /path/to/x4vr-src/linux/nix { })` to `environment.systemPackages`.

To update either way: `git pull` in `~/x4vr-src`, then the same build commands again.

### Setup and playing

Run `x4vr` (see above for where it is). This terminal menu replaces the Windows launcher, and
explains each item at the bottom of the screen.

1. Choose *Copy the Steam launch option* and paste it into X4 > Properties > General > Launch
   options. Steam's Play button still starts the normal game; only the menu starts VR.
2. On a tiling window manager (Hyprland, Sway, i3), add the rule from *Tiling window manager
   rules*.
3. Check the setting checklist
4. Choose *Launch X4 in VR*. It starts SteamVR if needed.
5. Look straight ahead and press Ctrl+F12 to recenter. Ctrl+F11 switches to the flat screen.

On a laptop with two GPUs, X4 must run on the same GPU as SteamVR (the dedicated one), else it
runs flat. Put `__NV_PRIME_RENDER_OFFLOAD=1 __GLX_VENDOR_LIBRARY_NAME=nvidia` (NVIDIA) or
`DRI_PRIME=1` (AMD or Intel Arc) in front of the launch option. The menu's *GPU* status line shows
which GPU the last VR session used.

The mod keeps your 2D X4 settings apart from the VR ones and puts them back when X4 closes. If
something goes wrong, the log is `~/.local/state/x4vr/x4vr.log`, and *Make a bug report* packs it
up for a GitHub issue, with SteamVR's logs and your setup (GPU and driver, headset, system, Steam
and SteamVR versions). If X4
hangs at its start (running in Steam, but no window), make the report while it hangs: it then also
records where X4 waits (with a full backtrace when `gdb` or `eu-stack` is installed). The menu's first line (and `x4vr version`)
shows the version.

### Uninstall

Choose *Uninstall* in the menu, clear X4's launch option in Steam, then delete the `~/x4vr-src`
directory.

## License

MIT (see [LICENSE](LICENSE)). The dependencies fetched at build time keep their own licenses:
OpenVR SDK (BSD-3-Clause), OpenXR SDK loader (Apache-2.0, with jsoncpp under MIT),
Vulkan-Headers (Apache-2.0), MinHook (BSD-2-Clause).
X4: Foundations is © Egosoft; this project contains no game files.
