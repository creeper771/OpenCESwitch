# Mesa for the Switch, with its GL thread

The Switch host draws through Mesa (OpenGL ES on the nouveau driver).
devkitPro's `switch-mesa` package has Mesa's GL thread, but on the Switch it
is never started, and the campaign's profile spent about 40% of the
game's main thread in the driver. `build_mesa.sh` builds that package
again, as devkitPro does, with this folder's patches:

| Patch | What it does |
| --- | --- |
| `0001-newlib-timespec_get.patch` | Builds with today's newlib, which has its own `timespec_get` |
| `0002-glthread.patch` | Gives contexts their GL thread hooks again (devkitPro's patch leaves `start_thread` and `thread_finish` out on the Switch), gives the GL thread the `set_background_context` it needs, starts it when `mesa_glthread=true` is in the environment (as DRI does for its option), drains it before a context is changed, and moves it to its own core (`mesa_glthread_core`, 1 by default) |
| `0003-thread-stack.patch` | Gives Mesa's threads a 4 MB stack: one made with `thrd_create` gets libnx's small default stack, taken from the heap, and the GL thread, which runs the GLSL compiler, ran off its end and broke the heap (the game crashed in `malloc` as it set up its renderer) |

Nothing of Mesa is kept in the repository: the script downloads Mesa
20.1.0-rc3 and devkitPro's recipe (`switch/mesa` of
[devkitPro/pacman-packages](https://github.com/devkitPro/pacman-packages),
commit `c7876f72c1f3353725ab8468f0ef9180280a8c31`), checks the SHA-256 sums
the recipe gives, applies the recipe's patches and then these, and builds
into `build/switch/mesa`. Mesa is under the MIT licence; these patches are
CC0, as the repository is.

## Build

You need devkitPro (`switch-dev`, `switch-libdrm_nouveau`,
`dkp-meson-scripts`, `switch-pkg-config`, `dkp-toolchain-vars`) and meson,
ninja, bison, flex and Python's mako:

```sh
sudo apt install meson bison flex python3-mako
sudo dkp-pacman -S dkp-meson-scripts switch-pkg-config dkp-toolchain-vars
bash port/switch/mesa/build_mesa.sh     # about 15 minutes
make -C port/switch/host                # links it when it is there
```

`bash port/switch/mesa/build_mesa.sh --source` only downloads and patches
(into `build/switch/mesa/src`), for writing a new patch.

## Using it

The host (`port/switch/host/host_gl.c`) sets `mesa_glthread=true` before it
makes the game's context, and then writes the renderer's vertices with
`glBufferSubData` in pieces the GL thread queues without waiting, instead
of mapping the buffer, which would stop the GL thread each time.

The GL thread is on unless an empty file `sdmc:/switch/opence/no_glthread`
exists. It pays only with the renderer's persistently mapped streaming
buffers (`host_gl_persistent_buffer`, used by `port/linux/src/d3d8_gl.c`):
without them each frame's vertices (about 5 MB) went through its queue and
were copied again into the GPU's buffer, the queue filled, and the game's
main thread waited 45% of its time for it. With them (checked on a
Switch), the driver's draw work leaves the main thread, which waits for the
GL thread or the GPU about 2% of its time; shader compiles still run on the
main thread, where glthread waits for them.

With the system's `switch-mesa` instead of this one, the GL thread is never
started, whatever the setting.
