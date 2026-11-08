# Building and running on Linux

A native Linux build: the game becomes an ELF executable and shared libraries, with SDL3 for the window, sound and
controllers, Vulkan for graphics and FFmpeg for the movies. No Wine and no emulator. As on Windows, the repository holds
no game code: everything is generated on your PC from **your own dump** of Super Smash Bros. Ultimate **13.0.4 or 13.0.5**
(only 13.0.4 has been tried on Linux so far).

```
git clone <this repository> ssbu-recomp
cd ssbu-recomp
scripts/linux/install_deps.sh                                   # system packages (sudo), Python venv, small source deps
deps/venv/bin/python scripts/setup.py --target linux --dump <your dump folder>     # about 1.5 hours, resumable
./play.sh                                                       # play
```

## Status

Tested on one PC: CachyOS (Arch-based, kernel 7.2), 24 threads, 31 GB RAM, NVIDIA RTX 3080 (proprietary driver), Wayland.

| | |
|---|---|
| Build from the dump | works: game library, 98 script modules, executable; `check` step passes (SDK self-test, pad self-test 56/56, C++ runtime self-test 145/145, differential test 32,043 / 32,043; it runs only with `--full-check`) |
| Opening movie, title, menus, character and stage select | work |
| F1 settings menu | built and drawn as on Windows (Dear ImGui over the Vulkan picture; keyboard, mouse and controller), see [Settings menu (F1)](#settings-menu-f1). **Tested on the porter's Linux desktop; not yet tried on a Steam Deck** |
| On-screen keyboard | not on Linux (it draws with Windows APIs). Text input, for example naming a Mii, opens a zenity or kdialog dialog instead |
| A whole Smash match (scripted input, 1v1 vs CPU) | works: steady 60 fps, sound, no audio underruns |
| Keyboard | works (tested); controllers through SDL3 are the same code as on Windows but **not tested with a real pad on Linux** |
| Album movie recording (the game's video capture to the album) | **not implemented** on Linux; the 7 self-test cases for it are accepted as known failures |
| OpenGL renderer | not on Linux: Vulkan only |
| Online play (experimental, `SSBU_ONLINE`) | Works: tested 2026-11-04 against a Windows player, over the internet and on the same LAN, in Local Wireless Create Room and the game's Online > Battle Arenas, with the normal delay-based netcode and with rollback; several matches, no desync or crash (this was Linux against Windows; Linux against Linux is not yet tested); rollback remains experimental and keeps the limits below. UPnP is a stub on Linux (forward the UDP port by hand); rollback fights are silent by default (the audio threads are held). See [Rollback and online play](#rollback-and-online-play-linux) |
| Other distributions, AMD / Intel GPUs, X11, Steam Deck | not tested. The package lists for Debian/Ubuntu and Fedora in `install_deps.sh` are best guesses; `setup.py` checks every library and says which one is missing |

## What you need

- **A dump of the game, 13.0.4 or 13.0.5** (base game with the update applied), from a console you own. The layout is the
  same as on Windows, see [BUILDING.md, "What you dump from your own console"](BUILDING.md#what-you-dump-from-your-own-console):
  ```
  <dump>/exefs/          main, sdk, rtld, subsdk0, main.npdm
  <dump>/romfs/          data.arc and the rest of the RomFS
  <dump>/sysdata/fonts/  optional, the console's shared fonts (see "Fonts" below)
  ```
  From `.nsp` files and your own keys: `deps/venv/bin/python tools/extract/romfs_extract.py <base.nsp> <update.nsp> <out>`
  and `tools/extract/nca_exefs.py`; they read the keys from `~/.switch/prod.keys`.
- **RAM:** 16 GB or more. `setup.py` runs one compiler per CPU thread by default (at most 24; `--jobs N` overrides it),
  and the compilers share RAM - 4 GB: each one reserves what its file is expected to need before it starts (at least
  1.5 GB; most files need well under 1 GB, a few need many GB) and is held to it. A file that needs more is compiled again
  with twice as much, up to the whole share; a few generated files need more than that at full optimisation (one passed
  20 GB) and are compiled with less optimisation instead, and the build log lists those files. Running out of memory
  would not slow the build down, it would get processes killed, possibly your desktop's: that is what the sharing
  prevents. (Before 2026-10-20 the rule was one compiler per 4 GB of RAM: 3 on a 16 GB Steam Deck with 8 threads.)
- **Disk:** about 12 GB for `build-linux/`, plus about 4 GB of shader cache after the first play session, plus the dump
  itself (about 16 GB).
- **GPU:** a Vulkan 1.3 driver. With `VK_EXT_graphics_pipeline_library` (current NVIDIA, Mesa RADV and ANV drivers have
  it) shaders are prepared ahead of time and nothing is compiled during a fight.
- **A distribution with SDL3 and clang 19 or newer:** current Arch and its derivatives, Fedora 42+, Debian 13,
  Ubuntu 25.04+.
- **For the rollback ring: userfaultfd.** The snapshot ring's write watch uses `userfaultfd` write protection with
  asynchronous faults (kernel 6.7 or newer; only user-mode-only faults, never kernel ones). `run.py` tries the syscall
  at start when rollback is on and warns with the fix when the kernel refuses it; without it the ring never starts and
  the game only says so in its log (`the rollback ring is OFF`). Note `sysctl vm.unprivileged_userfaultfd` is 0 on
  Fedora by default and that is fine: the knob gates only faults the kernel itself would service. If the warning
  appears anyway: `echo vm.unprivileged_userfaultfd=1 | sudo tee /etc/sysctl.d/90-ssbu-uffd.conf`, or
  `sudo sysctl vm.unprivileged_userfaultfd=1` until the next boot, or `sudo setcap cap_sys_ptrace=ep
  <build>/work/ssbu_native`.
  **These workarounds weaken the machine.** `vm.unprivileged_userfaultfd=1` is system-wide: it lets every local
  process use `userfaultfd`, a kernel attack surface. `setcap cap_sys_ptrace=ep` gives this executable the ability to
  inspect and control other processes, far more than the game needs. Use them only on a machine you trust (not a
  shared one), and undo them when done: `sudo rm /etc/sysctl.d/90-ssbu-uffd.conf` and
  `sudo sysctl vm.unprivileged_userfaultfd=0`, plus `sudo setcap -r <build>/work/ssbu_native`.

## 1. Dependencies

```
scripts/linux/install_deps.sh
```

It installs, using your package manager (pacman, apt or dnf; asks for sudo):
clang, pkg-config, SDL3, FreeType, OpenSSL, FFmpeg, glslang, the Vulkan loader, libGL, Opus, Python, git.
Then, without root, into `deps/` (ignored by git):

- `deps/venv`: a Python virtual environment with numpy, lz4, zstandard, Pillow, unicorn, capstone, pycryptodome
- `deps/Vulkan-Headers`: the Khronos headers, tag v1.4.351 (the version the port was built with)
- `deps/suyu`: a 2 MB sparse checkout of the Eden emulator's Mii service sources; only reference data for a self-test is
  read from it

`--no-system` skips the package manager if you install the packages yourself. On another distribution the script lists
what to install.

**Steam Deck (SteamOS):** SteamOS is read-only and every update removes what pacman installed, so `install_deps.sh` stops
there and shows how to build inside a [distrobox](https://distrobox.it) container instead (SteamOS 3.5 and newer
include distrobox and podman), in Desktop Mode, from Konsole, with this folder in your home folder:

```
distrobox create --name ssbu --image docker.io/library/archlinux:latest
distrobox enter ssbu
scripts/linux/install_deps.sh            # inside the container; on a Deck it also installs vulkan-radeon and libpulse
deps/venv/bin/python scripts/setup.py --target linux --dump <your dump folder>
./play.sh                                # also from inside the container (distrobox enter ssbu)
```

`install_deps.sh --steamos-host` installs on SteamOS itself instead (it needs a sudo password,
`sudo steamos-readonly disable` and the pacman keys, and the next SteamOS update removes the packages again). A Deck now runs 8 compilers instead of 3: the build should take
roughly 2 to 2.5 hours instead of 4.5 (an estimate from the job counts, not measured on a Deck yet).

The FFmpeg must be able to decode VP9 and Vorbis (the game's movies). Every distribution's normal FFmpeg can;
`setup.py` checks it.

Optional: `zenity` or `kdialog`. The game's on-screen keyboard (naming a Mii, a ruleset, ...) and the settings menu's
"Type in a dialog" buttons (your name, a room code) open a dialog with one of them.

`install_deps.sh` also fetches Dear ImGui v1.91.9b (MIT) into `deps/imgui`: the in-game settings menu (F1). Without it the
game builds without the menu (`setup.py` says so; `--imgui <folder>` or `IMGUI_ROOT` point to another copy).

## 2. Build

```
deps/venv/bin/python scripts/setup.py --target linux --dump <your dump folder>
```

Use the venv's Python, as shown. The build goes into `build-linux/`, in eleven steps. The same table as in
[BUILDING.md](BUILDING.md#the-steps) applies, with `.so` libraries instead of DLLs. Measured on the PC above:

| Step | Time |
|---|---|
| deps, stage, convert, modules, tables, lift, sdk | under a minute together |
| compile (1,161 files, 567 MB game library) | about 1.5 hours with 4 jobs (measured: 77 min for 967 of the files) |
| nro (98 script modules) | 14 min |
| link, check | about 1 min |

The build can be stopped at any time (Ctrl+C, or a crash). Run the same command again and it continues where it
stopped; finished steps are skipped. Every invocation needs `--dump`. Other options: `--status`, `--redo <step>`,
`--from <step>`, `--jobs N`, `--full-check` (the check step also runs the translator's long differential test), `--help`.
Files that fail to compile (often for lack of memory) are compiled again with fewer jobs before the build stops.
A file that runs into the 180 s time limit is compiled again with more of its longest functions unoptimised; a retry
that would leave exactly the same functions unoptimised as the attempt that just failed is skipped (it would fail the
same way), so the few hopeless files reach their unoptimised fallback after one or two 180 s attempts, not three.

When something is missing, step `deps` stops at once with what to install. It compiles and runs a small test program
against every library, so a broken package shows there and not an hour later.

## 3. Play

```
./play.sh
```

It works from any folder (a symlink to it in `~/bin` is fine) and takes the options of `scripts/run.py` (`./play.sh --help`).
The game's output shows in the terminal and is kept in `~/.local/share/ssbu-recomp/last_run.log`.

**Updates (2026-11-08):** `play.sh` keeps the game up to date by itself. Before the game starts it asks GitHub whether the
branch of this copy has newer commits; when it does, it runs `git pull --ff-only` and builds the game program again
(`setup.py --target linux --only link`, with the dump folders this build was made from: a few minutes). Nothing is updated
when files of the project have changes that are not committed, without internet, or with `auto_update = off` in
`build-linux/config/graphics.ini`. If the build fails, the game starts with the program built before and the command to
finish it is printed. (A change to the lifter or the build tools still needs `setup.py --target linux --from lift` by hand.)

- Saves go to `~/.local/share/ssbu-recomp/userdata` (`--userdata <folder>` for another one).
- `scripts/linux/install_desktop.sh` adds **Super Smash Bros. Ultimate (recomp)** to your application menu; the game's
  output of the last start is then in `~/.local/share/ssbu-recomp/last_run.log`. `--remove` takes it out again.
- **First start:** the game waits at a progress screen while it reads its shaders out of `data.arc` and prepares every
  one for your GPU. On the PC above, with the dump on a hard disk, reading took 101 s and the preparation was 61 % done
  200 s after the start (the screen then estimated 2 more minutes). After that the cache in `build-linux/cache/` is
  used, and the next start goes straight to the game. A driver update starts the preparation once more.
- **On a new save**, after the title screen a "Milestones" banner waits for a few presses of A.
- Close the window to quit.

### Controls

Controllers: any pad SDL3 knows (Xbox, PlayStation, Switch Pro / Joy-Con, 8BitDo, GameCube adapters, ...), up to 8
players, hot-plug. The keyboard is one more player:

| Keys | Button |
|---|---|
| Arrow keys | left stick |
| Z  X  C  V | A  B  X  Y (in the positions of a Switch controller) |
| Q  E | L  R |
| A  S | ZL  ZR |
| I  J  K  L | d-pad |
| Enter, Backspace | +  − |

Remapping, player slots, dead zones and rumble: `build-linux/config/controllers.ini`. It is written on the first
start, explains itself, and is re-read within a second of being saved. `build-linux/work/ssbu_native --pad-test` shows
what a pad sends.

Window: `build-linux/config/graphics.ini` (`window = windowed | borderless | fullscreen`, size, vsync), or F1 > Display.
Only `windowed` was tested on Linux.

### Settings menu (F1)

The same menu as on Windows ([BUILDING.md, "In-game menu (F1)"](BUILDING.md#in-game-menu-f1)), drawn over the game.

| To open or close it | |
|---|---|
| Keyboard | **F1** (F2: the stats overlay) |
| Controller | the **Home / Guide** button or the **Capture / Share** button (when the system lets SDL see it) |
| Any controller, Steam Deck | hold **− and + together for 1 second** (Xbox-style pads: View + Menu; Steam Deck: the two buttons left and right of the screen, View and Menu) |
| Close | also **+** or **Esc**, or the Close button |

Inside: d-pad / left stick move, A selects, B goes back, L / R switch pages; mouse and keyboard work too.

**Steam Deck:** the Steam button and the "..." button belong to Steam, so use **View + Menu held for 1 s**. While you hold
them the game sees − and + (a match may pause) until the menu opens; after that the game gets no input until you
release every button. Another way: in the game's Steam Input layout, bind a back button (L4 / R4 / L5 / R5) to the
keyboard key F1. Text boxes: in Desktop Mode use the keyboard or "Type in a dialog" (zenity / kdialog); in Game Mode,
Steam's on-screen keyboard (Steam + X) types into the active box (SDL may also open it by itself when a box is
selected; not verified).

**Online play** from the F1 page works on Linux; its settings are saved and used from the next start, as on Windows. The
Log page shows the log only when the game's output goes to a file (`play.sh` does that).

### Rollback and online play (Linux)

The native rollback state save/restore is built on Linux too: the real `game_state.c` branch, the snapshot ring, the
POSIX page tracker and the cooperative thread gate (`runtime/rollback/rb_park.c`). Threads stop themselves at SDK import
boundaries; no signals, ptrace or `SIGSTOP` are used. Window 1 (the game's big region) is read with `GetWriteWatch` on
userfaultfd, window 2 (image and modules) is compared page by page, the extras are compared when their write watch says
they changed.

Fault recovery is ported as well (`runtime/rollback/controller/fault_recovery_posix.c` and `runtime/nnunwind.c`):

- A pass that faults is marked tainted and the ring reloads the newest healthy state at the next safe point, walking
  further back if the same pass faults again.
- A throw whose guest frames cannot be walked is delivered through the host stack to the innermost `nro_catch_call`
  frame on the faulting thread. `eh_terminate` uses the same path before it ends the game.
- A `SIGSEGV` / `SIGBUS` in the lifted game code is answered by a `ucontext` handler that steps over a plain memory move
  or returns from a leaf memory helper. Once the pass is tainted it resumes at the innermost catch frame of the same
  host stack (`nnunwind_bail_context_posix`), using the registers from its `jmp_buf`; this is not a `longjmp` out of a
  signal handler.
- The catch table keeps a record on a host fiber that the ring's live list no longer names when the frame it points at
  is still the same catch frame (`nnunwind_catch_valid`). This is what fixed the `L2CExceptionShutdown` failure of the
  first port in the 1,698-frame run below.
- Pia / NetSystem threads are held at their waits but not rewound: their guest stacks are marked not-simulation
  (`r1_nosim`) at every thread-table build (`park` and the run-ahead hold path `ra_hold_save`, which the Linux ring's
  saves and captures use), and `restore_thread_regs` skips their registers. The thread table reuses a finished slot,
  so a network thread started after the 256th start (Pia is started again for every
  room) is still held and marked. The network is an external side channel like audio; a rollback must not replay it.
  At a load, a thread parked at a different wait (a network thread, a TaskWorker, or any service/presentation thread
  such as the VSync count, render dispatch or a file loader) is taken after 5 ms without refusing the load: it is
  held for the copy, `same` stays false and its registers are not restored. `SSBU_RB_NET_THREAD_WAIT=0`,
  `SSBU_RB_WORKER_WAIT=0` and `SSBU_RB_THREAD_WAIT=0` disable the network, worker and generic exceptions. The ring
  refuses a load only when a thread is still RUNNING guest code after the hold; a thread waiting while owning a mutex
  still refuses the hold. Absorbable faults on network threads taint the pass for the usual safe-point reload;
  they are not rejected just because of the thread name. This mitigates the observed NetSystem crash, but the heap
  objects these threads use can still be rolled back. Full Linux-against-Linux stability is not established. The
  network-thread exclusions, wait exceptions and thread-slot reuse are guarded by `__linux__`; they do not change the Windows paths.
  `SSBU_RB_NET_THREAD_EXCLUDE=0` turns the exclusion off and rewinds Pia/NetSystem guest stacks like the Windows path;
  their registers are restored only when their wait is the same. This is the A/B experiment for the NetSystem heap
  fault; replaying the network thread may duplicate Pia traffic. A TaskWorker parked at a different wait is covered by
  the same 5 ms exception; a running worker or one owning a mutex still refuses the hold. Holding the POSIX task workers
  through the cooperative gate from H1 to H2 was tried and reverted: the gate parks workers inside jobs and stalled the
  frame rate (avg 286 ms/frame in the Linux two-instance test).

What is not ported: the presentation pool, the debug-only rollback files, and run-ahead (`SSBU_RUNAHEAD`). The 13.0.5
address table is audited: every code address used by the rollback files is in `g_code_13_0_5`; data addresses follow the
documented +0x1000 segment shift (including the run-ahead-only `0x71053365a8`).

**Online test (2026-11-04).** The port was played against a Windows player, over the internet and on the same LAN:
Local Wireless Create Room and the game's Online > Battle Arenas, with the normal delay-based netcode and with
rollback. Several matches ran with no desync or crash; F1 was used for the settings. This is the first reported real
online test of the Linux rollback port. The automated tests below were not rerun in this pass.

**Remaining risk.** The offline simulation is exact for 1,698 real frames (see below), but a long match is not proven
on every path. Only Pia/NetSystem stacks and registers are excluded, not all their heap objects; the earlier
Linux two-instance test crashed on NetSystem after a load. The load admission change, wait exception and restored fault
absorption above were not seen to fail in the online test, but the automated Linux-against-Linux two-instance stress
run has not been repeated.
Another risk is a load that restores a state whose guest object names a job fiber/object that the load did not bring
back (the fiber moved to another host fiber between the capture and the load). The re-run then faults in a cascade.
The signal handler absorbs the first plain moves, but the cascade has no `nro_catch_call` frame on the current host
stack to cut it back to, so after 4,096 faults the net lets go and the process ends. A catch record on another
fiber's stack cannot be used: a cross-stack `longjmp` is exactly what the port must not do. A throw can also still
fail with `f_720052e2f0 handled an exception but was not entered through nro_catch_call` when its record is on another
fiber. These are the port's known limits, not something to work around with signals or a rewritten safe point.
Offline play is unaffected. If a long match does fail, fall back to delay-based online play
(`SSBU_RB_NET_NOROLLBACK=1`), or turn Rollback off in F1; the ring is then never made.
(2026-11-03: after the load-admission fix, the offline ring simulation completed 84 loads with k 1..7, no refused
load, no `FAULT RECOVERY` and no `HOST SIGNAL`. The base state also loads now: a stress run that rolled back every 7
passes loaded frame 0 in 23.9 ms with 2 audio threads taken at another wait, then hit the known not-saved-page
cascade at pass 374. 2026-11-04: the online test above passed; the automated Linux-against-Linux two-instance retest
is still pending.)

**Offline rollback test** (one process, no network). `--seed 1` (`SSBU_FIXED_SEED`) makes both runs pick the same CPU
fighter and stage, so the reference and the rollback run play the same match:

```
deps/venv/bin/python scripts/run.py --linux --build build-linux-1305 \
  --userdata <copy of a save that reached the fight menus> \
  --seq tests/seq_linux_battle.txt --seconds 150 --seed 1 \
  --env SSBU_RB_SIM=30,2 --env SSBU_DETERMINISTIC=1 --env SSBU_RB_AUDIO_OFF=1 \
  --env SSBU_R1_LOG=/tmp/r1_sim.log --env SSBU_FIGHT_HASH=/tmp/sim.hash

deps/venv/bin/python scripts/run.py --linux --build build-linux-1305 \
  --userdata <another copy of the same save> \
  --seq tests/seq_linux_battle.txt --seconds 150 --seed 1 \
  --env SSBU_DETERMINISTIC=1 --env SSBU_RB_AUDIO_OFF=1 \
  --env SSBU_FIGHT_HASH=/tmp/ref.hash

deps/venv/bin/python tests/rb_sim_cmp.py /tmp/sim.hash /tmp/r1_sim.log /tmp/ref.hash
```

Result on this PC: 1,698 real frames identical to the reference; 51 re-run runs (76 frames) all equal; no terminate.
`tests/seq_linux_battle.txt` goes from boot to a fight without waiting for the movie's `@movie` mark (the Linux movie
path does not raise it); use a save that has already seen the first-boot "Milestones" and "Unlocked!" banners (press A
through them once), because they sit between the title and the menu. `SSBU_RB_HOLD_PROF=1` prints where a hold's time
went.

**Fault test** (a test switch, not a full test of the net): add `--env SSBU_RB_FAULT_TEST=100` to the simulation. It
makes a deliberate leaf read, a plain read and a plain write every 100 real passes; the signal handler implements all
three shapes. It was not taken to completion here: the test run ended in the Lua catch-record case above before its
first trigger, so the shapes are covered by the code and the trace, not by a passing run of this switch.

**Online** (loopback bot, one process, room-shaped). Create a Local Wireless room in the game; the waiting-stage fight
starts alone and the loop bot is the other player (`SSBU_ONLINE=127.0.0.1:39500` without a server only says
"disconnected"):

```
deps/venv/bin/python scripts/run.py --linux --build build-linux-1305 \
  --userdata /tmp/rb-a --seconds 300 --seed 1 \
  --log /tmp/loop_room.log \
  --env SSBU_ONLINE=host:39520 --env SSBU_ONLINE_LOOPBACK=1 \
  --env SSBU_RB_NET=loop,8 --env SSBU_RB_NET_BOT=7 --env SSBU_RB_AUDIO_OFF=1 \
  --env SSBU_RB_NET_LOG=/tmp/loop_room.netlog
```

Checks: `grep -c "rollback [0-9]" /tmp/loop_room.log` (dozens+), `grep -c "FAULT RECOVERY"` (want 0),
`grep "HOST SIGNAL"` (want nothing), `grep "RING load .* REFUSED"` (want nothing), and the `load mean` in the
`RB NET report` should be a few milliseconds, not 500.

The long offline test was not rerun in this pass. The launcher maps
F1 > Experimental > Rollback netcode (`graphics.ini rollback = on`) to `SSBU_RB_NET=net` on the next start (`play.sh`
passes `--play`, `scripts/run.py` sets it). The launcher's group settings (`SSBU_RB_AUDIO_OFF=1`, `SSBU_LS_X_WW=1`, the
input delay) are the same as on Windows; `group_lobby = off` in `graphics.ini` keeps F1's online mode instead of the
same-network lobby.

**Linux against Linux:** two instances on one PC: one `SSBU_ONLINE=host:39520` and one
`SSBU_ONLINE=join:127.0.0.1:39520`, each with its
own `--userdata`, `SSBU_ONLINE_LOOPBACK=1`, `SSBU_RB_NET=net` and `SSBU_RB_AUDIO_OFF=1`, started about 25 s apart. Add
`SSBU_RB_NET_DELAY=0` and `SSBU_RB_NET_AHEAD=1` on both to force real loads on loopback (with the default delay the
~0 ms round trip absorbs every guess). The load admission change and fault recovery above were not seen to fail in the
2026-11-04 online matches; an automated Linux-against-Linux two-instance stress run is still worth repeating.
Use a windowed 1280x720 build for the retest (two fullscreen instances are over the frame budget), and set
`SSBU_FIGHT_HASH=<file>` on both so the checksums are exercised. The guest log should show
`r1: NETWORK: Pia ReceiveThreadStream guest stack ... not rolled back`; there should be no `FAULT RECOVERY` cascade on
the Pia thread and no `HOST SIGNAL`. `tests/linux_pair.py` drives the Linux sequences
(`tests/seq_lwr_host_linux.txt` / `seq_lwr_join_linux.txt`); play the room manually if they do not fit the save's
menus.

**Freeze at match start:** `mark_network_threads` must call `r1_nosim(lo, hi - lo)`: its second argument is a byte count,
not an end address. Passing `hi` scanned billions of pages at the base and every later capture, and excluded unrelated
heap pages from rollback. A saved Linux two-instance trace showed base saves of 136,165 / 135,385 ms, despite the initial
memory copies taking about 2 s. `tests/rb_network_stack_test.c` checks the argument using the real marking function
without loading the game. After the correction, the user's Linux two-instance logs showed base saves of 3,464 / 3,349.7 ms,
but the join instance later crashed on NetSystem in that automated run. That crash was not seen in the 2026-11-04 online
matches; the automated Linux-against-Linux two-instance retest is still pending.

UPnP is not implemented on Linux: in host mode the log says which UDP port to forward on the router.
`SSBU_RB_NET_NOROLLBACK=1` plays the match delay-based when the ring cannot be made or is turned off; verified with the
loop bot (the log says `rollback OFF for this match (SSBU_RB_NET_NOROLLBACK): delay-based`, no ring is made, the match
runs to its end). Crash recovery is the POSIX port described above; it is not a substitute for a correct state restore.

## Fonts

The game asks the system for the console's shared fonts, which are not part of the game dump. Without them, the
open-source replacements the runtime ships in `runtime/fonts_open` are used, as on Windows (see its README: Latin
text in Open Sans and Source Sans Pro, no Chinese, Japanese or Korean characters). With a firmware dump from your
console: `deps/venv/bin/python tools/extract/fw_fonts.py <firmware folder> <dump>/sysdata/fonts`, then run setup with
the same `--dump` (add `--redo convert` if you built before).

## Troubleshooting

- **Something gets killed during `compile` or `nro`** (the desktop freezes, then a program is gone; `journalctl` shows
  `oom`): too many compiler jobs for your RAM. Run again with a lower `--jobs`; finished files are kept. With
  systemd you can also cap the build:
  `systemd-run --user --scope -p MemoryMax=20G deps/venv/bin/python scripts/setup.py --target linux --dump <dump>`.
- **The game exits at once with code 7:** a font file is missing (see "Fonts"). The log line starts with `nn::pl:`.
- **A black window that never changes:** look at the output for `nvn_vk:` lines. `vulkaninfo --summary` must list your GPU.
- **No sound:** the game plays through SDL3, so PipeWire / PulseAudio must be running. The log says which audio device was opened.
- **To report a problem**, run `scripts/linux/collect_logs_linux.sh`: it makes `~/ssbu_linux_report_<date>.zip` with the
  build logs, the last game logs and facts about your PC (distribution, GPU and driver, Vulkan, library versions), never
  your dump or a game file. Send that file, or at least `~/.local/share/ssbu-recomp/last_run.log` (written by every `./play.sh`). It lists the
  libraries, the GPU and driver, the controllers found, and the error. If no frame was shown for 5 s,
  `build-linux/work/ssbu_native_freeze.txt` has a report of every thread.

## How the port works (for developers)

The runtime is written against the Win32 API. Rather than rewriting it, the Linux build keeps those calls and supplies
them: `runtime/plat/windows.h` and the other headers in `runtime/plat/` declare the subset of Win32 the runtime uses, and
`runtime/plat_posix.c` implements it on POSIX. That covers threads, events, condition variables on futexes,
`VirtualAlloc` with a page map, `GetWriteWatch` on userfaultfd write protection (as Wine does it; kernel 6.7+), files,
`LoadLibrary` (a `.dll` name maps to the `.so`), fibers, waveOut on SDL3 audio, and CryptoAPI on OpenSSL. Where its behaviour differs from Windows in a way a caller
could notice, a note says so at the function. `runtime/plat_mf.c` provides the part of Media Foundation the movie
player uses, on FFmpeg. Code that really is
Windows-only is under `#ifdef _WIN32`, with Linux branches: the SDL3 window, the Vulkan surface, FreeType glyph
outlines, the time zone from `/etc/localtime`, `/proc` thread reports, and the signal crash handler.

`RECOMP_TARGET=linux` (set by `setup.py --target linux`) switches the build scripts to `-fPIC`, `-fvisibility=hidden`, `.so` outputs, and an
executable that finds its libraries next to itself (`rpath $ORIGIN`). `tests/plat_selftest.c` tests the compatibility
layer by itself:
`clang -I runtime/plat -include runtime/plat/plat_pre.h tests/plat_selftest.c runtime/plat_posix.c $(pkg-config --cflags --libs sdl3 openssl) -lm -o plat_selftest`
(55 checks, 0 failures on the PC above).

## What to report (testers)

Please answer these, even if the answer is "no", and attach the file of `scripts/linux/collect_logs_linux.sh`:

1. Your distribution and version, GPU and driver, X11 or Wayland.
2. Did `install_deps.sh` and `setup.py` finish? If not, which step failed (the line starting with `FAILED at step`)?
3. Did the `check` step pass (the line `SSBU_SDK_SELFTEST:` and the differential test line)?
4. Did the game window open? Did the opening movie play, with sound?
5. Did the title screen appear? Did a fight start, and how did the speed feel?
6. Which controller did you use (keyboard, which pad)?
