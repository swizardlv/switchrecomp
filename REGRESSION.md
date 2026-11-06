# SwitchRecomp regression checks

Every runtime change goes through these checks before it reaches users. They run in order. A level that fails stops
the release until it is fixed or explained. Report the results with the template at the end.

| Level | What | Who runs it | Time |
| --- | --- | --- | --- |
| 0 | The four runtimes and the tool build, packages are complete | developer | 5 min |
| 1 | Unit tests (audio, recording clock) | developer | 1 min |
| 2 | Homebrew GPU test programs, screenshots compared with a baseline | developer, Linux | 5 min |
| 3 | Games, scene by scene | user | 30 min |
| 4 | Intel on Windows | user with an Intel GPU | 15 min |

---

## Level 0: build

From the `SwitchRecomp` folder:

```sh
for t in "linux x11 gcc" "win64 win32 x86_64-w64-mingw32-gcc" "linux headless gcc" "win64 headless x86_64-w64-mingw32-gcc"; do
  set -- $t; python3 switchrecomp.py --build-runtime --target $1 --backend $2 --cc $3 2>&1 | grep -E "error|Done"
done
tool/build_tool.sh SwitchRecomp.exe && tool/build_tool_linux.sh SwitchRecomp.bin
```

Pass when:
- all four print `Done:` and nothing prints `error`;
- the runtime inside each package is the one just built (`md5sum` the package's `bin/` files against `bin/`);
- the Windows zip and the Linux tar.gz are under 20 MB and contain no `gcc/`, `python/`, `games/*/*` or
  `__pycache__`;
- every new source file starts with the MZ Labs header and `CHANGELOG_v190.txt` has a line for the change.

The runtime prints the version `v190` in the first line of every game log. A build that says anything else
(`v190-something`) is not a release build.

## Level 1: unit tests

```sh
sh tests/run_audio.sh
```

This builds `tests/test_audio.c` and `tests/test_record_time.c` with AddressSanitizer and UndefinedBehaviorSanitizer
and runs them with SDL's dummy audio device. Pass when every line says `PASS` and the sanitizers report nothing.
`tests/VALIDATION.txt` has the expected output. Run it after any change to `runtime/audio.c`, `audio_*.h`,
`rec_*.c/h` or `rec_audio_time.h`. Otherwise it's optional.

## Level 2: homebrew GPU test programs

Small Switch homebrew programs, each drawing one thing the runtime has broken before, recompiled with SwitchRecomp
and run under Xvfb for 3 seconds. The last frame is saved and compared pixel by pixel with the same program under
the last good runtime.

**The test programs are not in the package.** They were lost and have to be rebuilt (devkitPro with deko3d,
then recompiled with SwitchRecomp). Each one goes in its own folder under `tests/regress/programs/<name>/`, like a
game folder (`<name>_code.so`, `<name>_data/`). The set used for v190:

| Folder | Covers |
| --- | --- |
| `deko_examples` | deko3d's example programs (triangles, textures, uniforms, depth) |
| `deko_basic` | the simplest clear + triangle |
| `cubeout` | rendering into cubemap faces and sampling the cubemap |
| `mipout`, `mip2out` | mip chains rendered level by level, sampling at a given LOD |
| `vol3out` | 3D textures rendered slice by slice |
| `aliasout` | a render target sampled through a texture at the same address |
| `swzout` | texture swizzles (component reorders, constant 0 and 1) |
| `srgbout` | sRGB render targets and textures |
| `lutout` | small lookup textures (LUTs) |
| `imgout` | image stores from fragment shaders |
| `csout` | compute shaders writing storage buffers and images |
| `cslatout` | compute results read back by the CPU (readback latency, held fences) |
| `ssboout` | global memory (storage buffers) read and written by shaders |
| `zeroout` | constant buffer reads past the bound size (must read 0) |
| `dmaout` | DMA engine copies |
| `chout` | several GPU channels at once |

Rebuilding a set like this is the first thing to do before more GPU changes. Add a program for every new bug class
(for example cubemap mips written by compute, or depth array layers).

### Running it

```sh
# once, with the last runtime known to be good: the baseline
tests/regress/regrun.sh path/to/good/runtime_linux_x11 base
# with the new runtime
tests/regress/regrun.sh bin/runtime_linux_x11 new
python3 tests/regress/cmpreg.py new base
```

Needs `Xvfb` and Mesa (the software renderer is enough). `regrun.sh` prints the frame count and image hash of each
program. `cmpreg.py` prints `same`, `DIFFERENT n pixels` or `NO IMAGE` (crashed or never presented: read
`new.out` in that folder).

Known noise: `dmaout` always differs (it draws timing-dependent data), and `deko_examples` differs on some runs. Run
any other difference twice. If it persists, it's a regression: open both `.ppm` images and the program's log, and
find the change that caused it (section "When something breaks").

Repeat level 2 with `SWR_DRV_EXCL=1` set (the Intel driver-exclusive path) when a change touches `glq.c`, the
compile thread or fences.

## Level 3: games

The user runs these with the new runtime copied over each game's executable. Every scene starts from the game's
own menu, not a save state. For each game: does it reach the scene, does it look like the reference, FPS from
`performance.txt`, and the log.

| Game | GPU used for testing | Scene | Should look like / check |
| --- | --- | --- | --- |
| Super Mario Odyssey | AMD (Linux), Intel Iris Xe and Intel HD (Windows) | title, Cap Kingdom, first 3D minute | no black or blue screen, bloom correct, table artifacts gone, no freeze when 3D starts |
| Sonic Frontiers | AMD (Linux) | Sky Sanctuary start (Cyber Space) | bright green level with sun shadows (reference screenshots from the user), not black or washed out blue |
| Tomodachi Life | Intel HD Gen11 (Windows) | boot, loading screen, 3D island map | no freeze on the loading screen, map loads in a few seconds, Miis render |
| GBA (Nintendo Switch Online) | any | boot | the Game Boy Advance menu shows, not black, not 2 s per frame |
| Celeste, Brawlhalla | any | boot and first level | sound plays (audout) |
| A Xenoblade title | any | boot | no out-of-memory crash (automatic extra memory) |
| Any game | any | F5, F7, F1 | F5 writes a trace, F7 records with sound, F1 opens the settings |

Compare against the previous runtime when something looks different: same scene, both screenshots side by side.

## Level 4: Intel on Windows

Intel's Windows driver is where most freezes come from (driver exclusive mode, the shader helper process).

- Tomodachi Life and Super Mario Odyssey to their 3D scenes, 2 minutes each.
- After a freeze, wait 10 s and send the log and `threads.txt`. The **GPU progress** lines show the pass and the
  shaders the GPU didn't finish.
- Once with `loop_limit=2048` in the game's `.ini`. If that avoids a freeze, a shader is looping.
- Check that the log says `driver exclusive mode` and that the helper process is off unless the driver crashed
  before (`shader_process`).

---

## When something breaks

1. **Find the change.** Most fixes have a switch that turns them off (`grep -n 'getenv("SWR_' runtime/*.c`). Run
   the failing test or game with the switch for each suspect change. The one that makes it pass is the cause.
   Common ones: `SWR_NO_IMG_LEVEL`, `SWR_IMG_LEVEL_2D_ONLY`, `SWR_NO_CUBE_MIPGEN`, `SWR_NO_UBO_RING`, `SWR_NO_GLQ`,
   `SWR_NO_RT_MIPS`, `SWR_NO_RTVOL`, `SWR_NO_TEXVIEW`, `SWR_NO_GPU_WATCH`, `SWR_NO_GPU_PROF`, `SWR_CS_SYNC`,
   `SWR_SYNC_SHADERS`.
2. **Compare F5 traces** of the good and the bad run: the first draw whose `dNNNN.ppm` differs, its inputs, and the
   mip/layer statistics at the end of the trace (`DEVELOPER_GUIDE.md`, section 5).
3. **Fix the cause**, don't hide it behind a game check. Then rerun levels 0 to 2 and the game that broke.
4. A fix that can't be proven yet ships behind a switch that's off by default, with a line in the changelog saying
   how to turn it on.

## Report template

```
Runtime: v190, built <date>, from <what changed>
Level 0: builds OK / packages OK (zip X MB, tar.gz Y MB)
Level 1: N PASS / failures: ...
Level 2: N same, M different: <test>: <pixels>, <cause or "known noise">
Level 3: <game>: <scene> OK / <problem> (log, screenshot)
Level 4: <game>: OK / freeze (GPU progress: pass ..., vs=... fs=...)
Not checked: <anything skipped and why>
```
