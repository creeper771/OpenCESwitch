# Licences of the Switch build's libraries

The NRO links libraries the other builds do not have, whose licences ask
each copy to carry their notices. `tools/ci_build.py switch` puts these
beside it, with the notices every build carries and those of Mesa
(`docs/license.html` of its source), musl (its `COPYRIGHT`, the guest's C
library) and zlib (devkitPro's `switch-zlib` package installs its
`LICENSE`), which the build has at hand.

| File | Library | Licence | Taken from |
| --- | --- | --- | --- |
| `libnx-LICENSE.md` | libnx 4.12.0, the Switch's system library | ISC | `LICENSE.md` of [switchbrew/libnx](https://github.com/switchbrew/libnx) at `v4.12.0` (SHA-256 `5b6675f85016009624d29c9222201d3c0fc6a2d5c59253367f27d3e15011cdb2`) |
| `libdrm_nouveau-LICENSE.txt` | libdrm_nouveau, Mesa's nouveau driver's link to the GPU | MIT | the notice at the top of `source/nouveau.c` of [devkitPro/libdrm_nouveau](https://github.com/devkitPro/libdrm_nouveau) at `137265185c95cf58ce94c7afecba14f895dac2a7`, which has no licence file of its own |
