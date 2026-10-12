# Memory probe

Phase 1.1 of `halo_ce_switch_port_guide.md`: finds out whether a homebrew
process on the Switch can have the memory the OpenCE guest needs, the fixed
range `0x80000000`–`0x8c000000` read/write and more address space below
4 GB (`port/android/include/halo_android_abi.h`).

## Build

You need devkitPro with `switch-dev`.

```sh
make        # opence_memory_probe.nro
```

## Run

1. Copy `opence_memory_probe.nro` to `sdmc:/switch/` on the SD card.
2. Start it from the Homebrew Menu twice:
   - from the Album (applet mode);
   - by holding R while starting a game (title takeover).
3. Each run writes `sdmc:/switch/opence_memory_probe.txt` (the second run
   overwrites the first, so copy it in between). `+` exits.

## The report

| Section | What it shows |
| --- | --- |
| process | The system version, the launch mode, the memory, and which of the system calls used below the process may make |
| regions | The kernel's regions and whether each covers the fixed range. svcMapMemory maps only into the stack region, svcMapPhysicalMemory only into the alias region |
| mappings below 4 GB | Every mapping below 4 GB |
| free space below 4 GB | The free space below 4 GB in each region |
| the fixed addresses | svcMapMemory and svcMapPhysicalMemory at the fixed range, each checked by writing and reading back every page, then unmapped |
| a pool below 4 GB | A 64 MB (or smaller) mapping elsewhere below 4 GB in the stack region, as the guest's heap and stacks need |
| code mapping in the address space region | svcMapProcessCodeMemory (as the homebrew loader maps each NRO) at the fixed range, made read/write and checked page by page and by reading a file straight into it; one page of the image made read/execute and a function copied there called; then a pool elsewhere below 4 GB |
| verdict | `FIXED ADDRESSES: yes` or `NO`, the room for pools, and `CODE MAPPING` |

The first runs (system 23.0.0) found the stack, alias and heap regions all
above 4 GB in both launch modes, so svcMapMemory and svcMapPhysicalMemory
cannot reach the fixed range; only the address space region covers it.
Applet mode had 393 MB in all, too little for the guest.

The next run (title takeover) mapped the fixed range and a 64 MB pool with
svcMapProcessCodeMemory, read/write, and a file read straight into it
matched. Making a page read/execute after it had been read/write failed
(`0xd401`): a page made writable stays data. The execution test therefore
copies its function in before the mapping is made and makes that page
read/execute directly, as the homebrew loader does with an NRO; the guest
image would be loaded the same way.

With that change (title takeover, system 23.0.0) every check passed: the
code page at `0x88000000` became module code and its function ran, the
other pages of the fixed range were read/write, a file read into them
matched, and the pool mapped. The host can therefore load the guest image
as the homebrew loader loads an NRO: copy it into heap memory, map it at
`0x88000000` with svcMapProcessCodeMemory, make its code pages read/execute
and its data pages read/write; and map the window and the guest's pools
below 4 GB the same way, read/write. Applet mode is not enough memory.

`FIXED ADDRESSES: NO` in both launch modes means the guest cannot run at its
current addresses on Horizon.
