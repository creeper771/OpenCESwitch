# Nintendo Switch (in progress)

A native homebrew build for a Switch running Atmosphère: an NRO that runs
the Android port's game image (the game compiled for ILP32 AArch64,
`build/android/halo_guest.elf`) on Horizon. `port/switch/host` is the
Switch's host, as `port/android/host` is Android's; the plan and the
evidence for it are in `halo_ce_switch_port_guide.md`.

## State

On a Switch (system 23.0.0), the game starts, reads its maps, reaches the
main menu and plays its music. Not done yet:

| Part | State |
| --- | --- |
| Memory below 4 GB, the image, threads, files, time | Done |
| Sound | Done (audout); no microphone |
| Picture | None yet: the game runs without a renderer (`HALO_NULL_RENDERER`), and the screen shows the host's log |
| Controllers | None yet |
| Network, sockets | None yet |

An import the host does not implement yet logs its name in `host.txt` the
first time the game calls it.

## Build

You need devkitPro with `switch-dev`, and what `ninja android` needs for the
guest image (refer to `port/android/README.md`).

```sh
python configure.py
ninja build/android/halo_guest.elf
make -C port/switch/host        # build/switch/opence.nro
```

## Run

1. Copy `build/switch/opence.nro` to `sdmc:/switch/opence/`.
2. Copy the game's `maps` folder (from the Xbox disc image) to
   `sdmc:/switch/opence/maps/`.
3. Start it in title takeover: hold R while starting a game, then choose it
   in the Homebrew Menu. From the Album (applet mode) there is too little
   memory, and it says so.
4. Close it from the HOME menu.

`sdmc:/switch/opence` then holds `host.txt` (the host's log, with a
heartbeat every 5 seconds: what the game did and where its time went),
the game's `debug.txt` and `config.toml`, and `save/` (the Xbox's `z:` and
`u:`). The first start writes the game's cache files to `save/z` (about
760 MB), which takes a few minutes on an SD card.

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
