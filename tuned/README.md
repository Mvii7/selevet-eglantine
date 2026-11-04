# Selevet Eglantine

An experimental, unofficial Windows build of the Eglantine Nintendo Switch 2 emulator, tuned for Mario Kart World on a Ryzen 5 5500 with an RTX 3050 8 GB and 16 GB of RAM.

It is based on [zortos293/Eglantine build 31](https://github.com/zortos293/Eglantine/releases/tag/nx2-build31-20261101) (commit `badc91ebce`). That project builds on [Djihads80/Eglantine](https://github.com/Djihads80/Eglantine), Eden and yuzu. All credit for the emulator goes to those projects. This is not an official release of any of them, so please don't report problems with it to them.

## Download

Get the zip from the Releases page, extract it into its own folder, and run `eglantine.exe`. Settings are already built in, so you only need to add your keys and firmware and map your controller.

**You need:**

- A CPU with AVX2. This build is compiled with `-march=x86-64-v3`, so it won't run on CPUs without AVX2, such as older Intel CPUs and some Pentium and Celeron chips. If that happens, use the official build 31 or build from source with your own flags.
- The latest [Microsoft Visual C++ x64 runtime](https://aka.ms/vs/17/release/vc_redist.x64.exe).
- Your own legally dumped keys, firmware and games. None are included.

## What's different from build 31

| Change | Files | Notes |
|---|---|---|
| GPU mapping table updates only compare the changed range | `nx2_gpu_mapping_table.h` | Matched the original over 2 million randomised operations, and ran about 160-240x faster per operation in a benchmark. That is not an fps figure. |
| Async draw check uses the current draw's real count | `draw_manager.cpp`, `maxwell_3d.h`, `vk_pipeline_cache.cpp` | The old check could read a stale count from the buffer the draw doesn't use. `EDEN_NX2_STALE_DRAW_COUNTS=1` restores it for comparison. |
| Logs draws waiting on a pipeline build | `vk_graphics_pipeline.cpp` | Diagnostics only. |
| Saves the PGO profile every 30 s in training builds | `src/yuzu/main.cpp` | Only compiled with `EGLANTINE_PGO_GENERATE`. |
| Command entries only wait when their page was GPU-written before | `dma_pusher.cpp` | No measurable fps change in repeated no-input tests. A page's first write can show stale commands for one frame. `EDEN_NX2_COMMAND_DRAIN=1` restores the old wait. |
| Tested settings are the defaults | `src/common/settings.h`, `src/core/game_settings.*` | Controller mappings aren't included. Booted to rendered frames with a default config and no input; a full race has not been run on this exact build. |

The release is also built with Clang 23.1.4, ThinLTO, an `x86-64-v3` target and a profile trained on about 7 minutes of Mario Kart World.

## Launch flags

`nx2_switches.txt` beside the program sets `EDEN_NX2_UNSAFE_PREEMPTIVE_READS=1`, an unsafe shortcut that build 31's own README does not recommend by default. Delete that line if you get glitches. Unlike build 31's launcher, this build leaves the split GPU thread on, because the project's notes measured it at 1.46x throughput.

## Built-in settings

These are the program's defaults, in `src/common/settings.h`. They apply to all games, and Unsafe accuracy can break some Switch 1 titles, so change them in the emulator's settings or per game if something misbehaves. If you already have an Eden or Eglantine config, any settings you changed yourself stay as they are.

**This build shares its config with other Eden and Eglantine builds.** When it saves your config, settings that match these defaults get marked as "default", so another build you open afterwards will use its own defaults for them instead, which can be much slower. Back up `%APPDATA%\eden\config` first if you switch between builds. Creating an empty `user` folder beside `eglantine.exe` gives this build its own separate data, but you then need to put your keys and firmware there.

| Setting | Value |
|---|---|
| GPU mode | Fast |
| DMA accuracy | Unsafe |
| GPU fence behaviour | Balanced (Immediate for Mario Kart World) |
| Scaling filter | ScaleForce |
| Memory layout | Stock default (Switch 2 games always use 12 GB) |
| CPU accuracy | Unsafe |
| Asynchronous shaders | On |
| Asynchronous presentation | On |
| Disk shader cache | On |
| Driver pipeline cache | Off |
| Sync memory operations | Off |
| Reactive flushing | Off |
| VRAM usage mode | Aggressive |
| Resolution | 0.5x, handheld mode |

Turning sync memory operations off gave a black screen entering races on an older Eglantine build. It didn't on this one in testing, but if you get a black screen, turn it back on first.

## Test results

Tested on one PC only: Ryzen 5 5500, RTX 3050 8 GB, 16 GB RAM at 3600 MHz, Mario Kart World.

| Run | Median fps | Lowest 600-frame average |
|---|---|---|
| This build, played at 1x resolution | 55.5 | 25.9 |
| v1, sync memory on, 0.5x | 36.7 | 23.5 |
| v1, sync memory off, 0.5x | 42 | 24.2 |

The sync memory off run was short, so treat it as promising, not final. Heavy tracks can still drop while textures load, and this is not a locked 60 fps.

## Known problems

- The game can log a panic (for example 2162-0001 or 2010-0212) and may keep running in a broken state. The cause isn't fully known.
- Stopping a game can leave `eglantine.exe` running with no window, holding a lot of RAM. Check Task Manager before relaunching.
- Don't share your shader cache: `vulkan.bin` contains game code.

## Building from source

You need Visual Studio with the C++ tools, Clang 23, `glslangValidator` and `patch` on `PATH`. Edit the paths in the cache files in this folder to match your machine.

```bash
cmake -C tuned/pgo-use-cache.cmake -S . -B build/pgo-use -G Ninja
cmake --build build/pgo-use --target yuzu
```

To train your own profile, build with `tuned/pgo-gen-cache.cmake`, run with `LLVM_PROFILE_FILE` set, play for a while, then merge the output with `llvm-profdata merge -o tuned/eglantine.profdata`. A profile trained on your own games and CPU should suit you better than the one included here.

## 30 FPS limit

Graphics settings has **Limit speed to 30 FPS**. It holds presents to 30 per second and applies without restarting. The old separate 30 FPS download set `EDEN_NX2_PRESENT_HZ=30` in `nx2_switches.txt`. That line is no longer read.

## Speed when a frame misses 60

Donkey Kong Bananza version 458752 clamps the measured rate to 20-60 Hz. At 39 Hz and below a frame steps two or three vsyncs, and that choice is applied on the same frame. The frame fill stays 1/60, so a slow frame does not hold the next one open. At 100% the host lets the game time its own frames. The 30 FPS limit, and a speed limit other than 100%, still pace presents. `EDEN_NX2_FRAME_RATE=0` leaves the game's rate code unchanged.

## Build 104 merge

The source now also includes the changes from [Djihads80/Eglantine build-104](https://github.com/Djihads80/Eglantine/releases/tag/build-104). Sparse texture tile mapping (`nx2:gpu 216`) is included but off by default, because it ran about 15-20% slower on an RTX 3050. `EDEN_NX2_SPARSE_REMAP=1` in `nx2_switches.txt` turns it on.

## Performance build

This source sends work to the host GPU every 128 draws instead of every 4096 (`EDEN_NX2_FLUSH_DRAWS`), skips waits on semaphores already met in memory (`EDEN_NX2_ACQUIRE_PEEK=0` restores them), runs at high process priority (`EDEN_NX2_HIGH_PRIORITY=0` turns that off) and uses a profile retrained on Mario Kart World. The semaphore changes and the benchmark logging (`EDEN_BENCH_LOG`, `EDEN_TAS_AUTOSTART`) come from [shintasku/selevet-eglantine-linux](https://github.com/shintasku/selevet-eglantine-linux).

## Performance build 2

This source fetches GPU-written command lists in one batch per GPU wait instead of one wait each (`EDEN_NX2_CMD_BATCH=0` restores that), and skips the wait for a semaphore when a release that meets it is already queued for the split GPU thread (`EDEN_NX2_ACQUIRE_QUEUED=0` restores that). The release turns on `EDEN_NX2_QMD_ACQUIRE_SKIP=1` in `nx2_switches.txt`. Changing the memory layout now restarts the emulated system. `EDEN_NX2_CMD_STATS=1` logs the GPU waits every 10 seconds, and `EDEN_NX2_SHADOW_SKIP=N` (off) redraws shadow maps every N frames.

## Performance build 3

The CPU translator no longer uses AVX-512, even on CPUs that have it (`EDEN_NX2_AVX512=1` turns it back on). The upscaler fallback also looks for its images at constant buffer 0 +0x60 and +0x70, where Donkey Kong Bananza version 458752 keeps them. The video decoder only marks a picture from the final input as the end of a video. The release no longer turns on `EDEN_NX2_QMD_ACQUIRE_SKIP`.

## Performance build 4

The loader makes the Switch 2 SDK's `nn::vi::detail::DefaultDisplay::QueryVrrSupported` report VRR support, found by its exported name (`EDEN_NX2_REPORT_VRR=0` turns it off). Switch 2 draws skip a pipeline that is still being built instead of waiting for it when asynchronous shaders are on (`EDEN_NX2_ASYNC_SKIP=0`). `EDEN_NX2_VBLANK_ALIGN=1` holds late frames to the next vblank while the host is pacing (the 30 FPS limit, or a speed limit other than 100%). A debug readback that froze the game once is off by default.

## NUP

NUP containers load, and a loose dump can be packed from the game list (`Convert to NUP...`) or with `eden-cli --pack-nup <dump> [<out.nup>]`. A compressed member larger than 256 MiB is decompressed to a temporary file instead of being held in memory. A Switch 2 NUP is detected from `main.npdm` inside the container and boots with the 12 GiB layout.

## License and AI disclosure

GPL-3.0-or-later, the same as the projects it's based on. See `LICENSE.txt`.

The changes in this repo were written with Claude (Anthropic), then tested in-game by the repo owner. They have not been reviewed by the Eglantine developers.

The NUP loader and the 30 FPS checkbox were written with Grok 4.7.
