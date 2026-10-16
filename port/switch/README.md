# Nintendo Switch (in progress)

A native homebrew build for a Switch running Atmosphère: an NRO that runs
the Android port's game image (the game compiled for ILP32 AArch64,
`build/android/halo_guest.elf`) on Horizon. `port/switch/host` is the
Switch's host, as `port/android/host` is Android's; the plan and the
evidence for it are in `halo_ce_switch_port_guide.md`.

## State

On a Switch (system 23.0.0), the game runs: the menus at 60 frames a
second, and the campaign is playable with sound, controllers and rumble,
though below 60 frames a second and with stutters. Not done yet:

| Part | State |
| --- | --- |
| Memory below 4 GB, the image, threads, files, time | Done |
| Sound | Done (audout); no microphone |
| Picture | Done: OpenGL ES 3.2 on switch-mesa (nouveau), on the display's 1280x720 in the hand and, docked, the TV's setting up to 1920x1080 (following docking and undocking). The game's 852x480 is drawn at 480 lines in the hand and 720 docked (two thirds of the display's), scaled up; Video Settings > Resolution (`display.render_height`) chooses 480 to 1080. Drawing the display's own lines makes the GPU the limit: docked at 1080, about 40 frames a second in the campaign against 58 at 480 |
| Controllers | Done: up to 4 (Joy-Con pairs, the handheld's, Pro Controllers, single Joy-Cons), by Xbox position (B is the Xbox's A), with HD Rumble; no touch screen, keyboard or clipboard |
| Split screen | Co-op campaign (Multiplayer > CO-OP CAMPAIGN) works with two controllers, paired before OpenCE starts (HOME > Controllers): checked in 343 Guilty Spark at about 35 to 60 frames a second (45 most of the time). Split-screen multiplayer goes through a network game's lobby, which needs the network |
| Speed | The campaign is playable but below 60 frames a second in heavy scenes, where the game's own code on one core is the limit; Mesa's GL thread, persistently mapped vertex buffers and Mesa's shader cache (`port/switch/mesa`) took the driver's work and repeated shader compiles off the main thread |
| Network, sockets | None yet: the game's network functions fail as without a network (`host_network.c`) |

An import the host does not implement yet logs its name in `host.txt` the
first time the game calls it.

## Build

You need devkitPro with `switch-dev`, and what `ninja android` needs for the
guest image (refer to `port/android/README.md`), and for Mesa what
`port/switch/mesa/README.md` lists. `--release` leaves out the game's
assertions, which its tag and datum lookups check on every call; clang 22
reads the PGO profile in `pgo/` (an older clang builds without it).

```sh
python configure.py --release --android-guest-cc clang-22
ninja build/android/halo_guest.elf
bash port/switch/mesa/build_mesa.sh    # Mesa with its GL thread and shader cache
make -C port/switch/host        # build/switch/opence.nro
```

## Run

1. Copy `build/switch/opence.nro` to `sdmc:/switch/opence/`.
2. Copy the game's `maps` folder (from the Xbox disc image) to
   `sdmc:/switch/opence/maps/`.
3. Start it in title takeover: hold R while starting a game, then choose it
   in the Homebrew Menu. From the Album (applet mode) there is too little
   memory, and it says so.
4. Quit it from the game's menus: the host stops the game's threads and
   asks the system to end the application, which returns to the HOME
   menu after a moment. (Closing it from the HOME menu works too.)

The screen shows the host's log until the game opens its window.
`sdmc:/switch/opence` then holds `host.txt` (the host's log, with a
heartbeat every 5 seconds: what the game did, where its time went, and a
profile of its main thread), `profile.txt` (every sample of the main
thread since the start, written every minute: its count, where the
thread was, and up to eleven callers from its frame records; `guest` addresses are for
`llvm-symbolizer --obj=build/android/halo_guest.elf`, `host` offsets for
`aarch64-none-elf-addr2line -f -e build/switch/opence.elf`),
the game's `debug.txt` and `config.toml`, and `save/` (the Xbox's `z:` and
`u:`). The first start writes the game's cache files to `save/z` (about
760 MB), which takes a few minutes on an SD card.

## Controllers

Controllers connected before OpenCE starts are players 1 to 4. Holding -
and + together for a second opens the system's controller screen, where
controllers are connected, a Joy-Con pair is parted into two players (each
presses SL and SR), or two Joy-Cons are joined again (L and R on both).
With two Joy-Cons of their own, both players hold their - or + at once; one
alone holds its - or + and clicks its stick.

A Joy-Con of its own is held upright, as half of a pair:

| Joy-Con | In the game |
| --- | --- |
| Stick | Move |
| Turning the Joy-Con | Aim (its gyroscope; faster turns turn further) |
| ZR or ZL | Fire |
| R or L | Throw a grenade |
| Face buttons (the left one's arrows) | A, B, X and Y by position: jump, melee, action and reload, change weapon |
| Stick click | Zoom |
| SL | Crouch |
| SR | The flashlight when tapped; held, the gyroscope lets go, to bring the Joy-Con back without turning |
| + or - | Pause |

The grenade's kind and the scoreboard have no button on one.

## How it operates

- **Memory**: Horizon puts a homebrew process's heap, alias and stack
  regions above 4 GB. The host maps heap memory again below 4 GB with
  `svcMapProcessCodeMemory`, as the homebrew loader maps an NRO, for the
  Xbox window (`0x80000000`), the image (`0x88000000`), fixed ranges the
  game asks for, and 64 MB pools for the rest (`host_memory.c`). A page of
  such a mapping can be made executable only if it was never writable, so
  the image is built whole in heap memory, mapped, and only then given
  each page's permission (`host_loader.c`).
- **Threads**: each runs the guest on a stack below 4 GB
  (`host_stack.s`, `host_thread.c`), at priority `0x3B`, the one Horizon
  time-slices on cores 0 to 2.
- **System calls**: the guest's musl makes Linux system calls, which
  `host_syscall.c` performs with newlib and libnx (files, futexes on
  `svcWaitForAddress`, time).
- **Texture tracking**: by page hashes (`host_watch_hash.c` of the Android
  host), as page faults cannot be caught.
- The process ends with the game (`svcExitProcess`), since the game's
  mappings use the homebrew loader's heap.

`port/switch/feasibility/memory_probe` is the test that showed the memory
scheme works.
