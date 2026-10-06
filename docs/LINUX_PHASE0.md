# Linux port: Phase 0, session 2 (measurements with the probe kit)

Session 1 (commands only) is written up in `docs/LINUX_FINDINGS.md`. This session uses the
probe kit built from this repository. Nothing in it changes the game: the probes only write logs.
Every result goes back into `docs/LINUX_FINDINGS.md`.

Time: about 45 minutes, most of it step 4 in the cockpit.

---

## 1. Get and build the kit (once)

```bash
mkdir -p ~/src && cd ~/src
git clone https://github.com/cully-curwen/x4_vr_linux.git
cd x4_vr_linux
nix-build
```

`nix-build` builds with your system's nixpkgs channel and runs the tests. It ends by printing a
`/nix/store/...-x4vr-0.1.0-phase0` path and leaves a `result` link in the folder:

| File | What it is |
|---|---|
| `result/bin/x4vr` | Command-line tool: `vr-check`, `udp-send`, `elf-classes` |
| `result/bin/x4vr-probe-run` | Steam launch wrapper that loads the two probes into X4 |
| `result/lib/libx4vr_probe.so` | Socket probe (logs how X4 reads its OpenTrack socket) |
| `result/lib/libVkLayer_x4vr_probe.so` | Vulkan probe layer (logs X4's GPU, queues, window system, swapchain) |

Later updates: `git pull && nix-build`.

All logs go to `~/.local/state/x4vr/probe/`. They contain paths with your user name.

Keep one note file for anything you observe:

```bash
mkdir -p ~/x4vr-phase0 && cd ~/x4vr-phase0
```

---

## 2. Static class list (0.3 follow-up, no game running, 1 minute)

Lists X4's head-tracking classes and their virtual function tables, from the type names that are
still in the binary. This is the starting point for the eye-at-use hook (plan stage B).

```bash
X4DIR="$HOME/.local/share/Steam/steamapps/common/X4 Foundations"
~/src/x4_vr_linux/result/bin/x4vr elf-classes "$X4DIR/X4" Track  > classes-track.txt
~/src/x4_vr_linux/result/bin/x4vr elf-classes "$X4DIR/X4" Camera > classes-camera.txt
tail -1 classes-track.txt classes-camera.txt
```

Send both files. They hold only class names and addresses, no game code.

---

## 3. SteamVR from a Linux program (0.1, no game running, 2 minutes)

Start SteamVR with the Frame connected, as you normally do. Then run the check through Steam's
runtime (`steam-run`): SteamVR's client library needs it, and run directly the check reports no
headset (docs/LINUX_FINDINGS.md, 0.1):

```bash
nix-shell -p steam-run --run "steam-run ~/src/x4_vr_linux/result/bin/x4vr vr-check --seconds 15" | tee vr-check.txt
```

While it runs, turn your head left, right, up and down, and lean left and right. It prints the
headset model, the render size per eye, both eye positions, and then the head pose four times a
second.

Send `vr-check.txt`.

---

## 4. X4 with the probes (0.5, 0.6, 0.7)

### 4.1 Set the launch option

In Steam, X4 → Properties → General → Launch Options, enter (with your user name):

```
/home/cully/src/x4_vr_linux/result/bin/x4vr-probe-run %command%
```

Use this full path, not a `/nix/store` one: the `result` link follows rebuilds.

### 4.2 Main menu (0.7)

Start X4 and wait 30 seconds at the main menu. Check that a `layer-<number>.log` and a
`socket-<number>.log` have appeared:

```bash
ls -l ~/.local/state/x4vr/probe/
```

If they haven't, quit X4 and send `wrapper.log` and `stderr.log` from that folder; nothing else
in step 4 will work until this does.

### 4.3 Turn on OpenTrack

Options → Controls → Head Tracking Support → **OpenTrack Support: On**. Note (or screenshot) any
new options that appear, such as filter strength, deadzone or position factor, and their values.
Leave them as they are.

Also note *Game Settings → Camera → Head Movement Intensity* (keep it at 100 for now) and
*VE Goggles Auto Reset*.

### 4.4 Into the cockpit

Load a game and sit in your ship's cockpit, ship stopped, in a quiet spot. Don't pause the game.
The views change only through head tracking from here on, so don't touch the mouse.

### 4.5 Send head poses (0.5)

In a terminal next to the game (windowed or a second monitor helps):

```bash
~/src/x4_vr_linux/result/bin/x4vr udp-send
```

It sends a head pose 90 times a second and takes commands, one per line. Units are OpenTrack's:
x, y, z in centimetres; yaw, pitch, roll in degrees. After each command, look at the game and
note what happened, then type `zero` before the next one.

Note for each line: direction (left/right, up/down, forward/back, tilt), roughly how far, and
whether it moved at once or eased in.

| # | Command | What to note |
|---|---|---|
| 1 | `yaw 30` | Turns which way? About 30°, or more or less? |
| 2 | `yaw -30` | Opposite of 1? |
| 3 | `yaw 90` | About a quarter turn? Stops short (limit)? |
| 4 | `pitch 20` | Up or down? About 20°? |
| 5 | `roll 20` | Tilts which way? |
| 6 | `x 5` | Head moves left or right? Roughly how far? |
| 7 | `y 5` | Up or down? |
| 8 | `z 5` | Forward or back? |
| 9 | `z -5` | Opposite of 8? **Does one of 8 or 9 not move at all?** (backward clamp) |
| 10 | `x 50` | Moves further than 6, or stops at a limit? |
| 11 | `yaw 30` then, after 5 s, `yaw 31` | Does the small change still move the view? (still-pose check) |
| 12 | `yaw 30`, then wait 30 s | Does the view drift back to centre on its own? |
| 13 | `alt yaw 10` | Shaking ±10° quickly, or calm near the centre? (smoothing) |
| 14 | `yaw 30`, then `pause` | View freezes, snaps back to centre, or slowly returns? Then `resume`. |
| 15 | Set *Head Movement Intensity* to 50 in the options, then `yaw 30` | Half as far as in 1? Set it back to 100. |

`quit` stops the sender. If anything else stands out, write it down too.

### 4.6 How X4 reads the socket (0.6)

The socket probe has been logging the whole time. For a cross-check from the kernel's side, while
`udp-send` is still running, run this in another terminal (needs `sudo`; it slows the game for
20 seconds):

```bash
cd ~/x4vr-phase0
pid=$(pgrep -x 'Main\(\)')
nix-shell -p strace --run "sudo \$(command -v timeout) -s INT 20 \$(command -v strace) -f -tt -e trace=%network,poll,ppoll,select,pselect6,epoll_wait,epoll_pwait,epoll_ctl -o strace.txt -p $pid"
grep -c . strace.txt
```

### 4.7 Finish

Quit X4 normally, then collect everything:

```bash
cd ~/x4vr-phase0
cp ~/.local/state/x4vr/probe/*.log .
ls -l
```

Send: `classes-track.txt`, `classes-camera.txt`, `vr-check.txt`, your notes from 4.3 and 4.5,
`strace.txt` (if large, the first 300 lines), and the `socket-*.log`, `layer-*.log`, `wrapper.log`
and `stderr.log` of the X4 process. `stderr.log` can be long; the lines starting with
`[Vulkan Loader]` and any `ERROR` lines are the ones that matter.

You can leave the launch option in place for later runs, or clear it to go back to a plain start.
