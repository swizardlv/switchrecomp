# SwitchRecomp

Static recompiler for Nintendo Switch games, by MZ Labs. The game code (ExeFS) is translated from ARM64 to C once
and compiled into a native module. A C runtime emulates the Horizon OS services and the Maxwell GPU, with an OpenGL
renderer and an optional Vulkan renderer on Windows. The game data (RomFS) is read as it is.

This repository has the source only. No games, keys or firmware are included, and none are downloaded.

Player manual (recompiling a game, controls, `.ini` options): `README.txt`.
Code layout, logs, F5 traces and code style: `DEVELOPER_GUIDE.md`.
Changes: `CHANGELOG_v190.txt`.

## Layout

| Path | Contents |
| --- | --- |
| `switchrecomp.py`, `a64.py`, `codegen.py`, `nxloader.py`, `brt.py`, `tools_sym.py` | The recompiler (Python 3) |
| `runtime/` | The runtime (C): kernel, IPC, services, GPU, audio, video, window backends |
| `runtime/snxvk/` | The optional Vulkan renderer (GPL-3.0, see `runtime/snxvk/README.txt`) |
| `tool/` | The GUI (`SwitchRecomp.exe`), `Install.bat` / `Install.sh` helpers, `get_deps.py` |
| `tests/` | Unit and regression tests (`REGRESSION.md`) |

## Building

### Requirements (Linux, Debian/Ubuntu names)

```sh
sudo apt install build-essential python3 libsdl2-dev libx11-dev libgl-dev libegl-dev \
    mingw-w64 gcc-mingw-w64-x86-64-win32
```

The Windows builds are cross-compiled with the win32 thread model variant of `x86_64-w64-mingw32-gcc`.

### Runtimes

From the repository root:

```sh
python3 switchrecomp.py --build-runtime --target win64 --backend win32    --cc x86_64-w64-mingw32-gcc
python3 switchrecomp.py --build-runtime --target win64 --backend headless --cc x86_64-w64-mingw32-gcc
python3 switchrecomp.py --build-runtime --target linux --backend x11      --cc gcc
python3 switchrecomp.py --build-runtime --target linux --backend headless --cc gcc
```

The results go to `bin/`.

### GUI

```sh
tool/build_tool.sh SwitchRecomp.exe        # Windows, cross-built
tool/build_tool_linux.sh SwitchRecomp.bin  # Linux
```

The Windows GUI links SDL2 2.30.9, SDL2_ttf 2.22.0 and SDL2_image 2.8.2 statically. It expects their mingw devel
packages unpacked in `tool/deps/` (`SDL2-devel-2.30.9-mingw`, `SDL2_ttf-devel-2.22.0-mingw`,
`SDL2_image-devel-2.8.2-mingw`, from the libsdl-org GitHub releases), or in the folder named by `SDLW`.

### Optional libraries

The runtime builds without any of these. Each one adds a feature when its static library is in place:

| File | Adds | Source |
| --- | --- | --- |
| `runtime/oh264/win/libdecoder.a`, `libcommon.a` (or `<os>/libopenh264.a`) | H.264 game videos | OpenH264 (BSD-2), headers in `runtime/oh264/include` |
| `runtime/opus/<os>/libopus.a` | Opus audio | libopus (BSD-3), headers in `runtime/opus/include` |
| `runtime/vp9/<os>/libvpx.a` | VP9 WebM movies | libvpx 1.14.0, build steps in `runtime/vp9/win/README.md` and `runtime/vp9/lin/README.md` |
| `runtime/spvc/<os>/libspvc.a` | LSFG frame generation | SPIRV-Cross (Apache-2.0), headers in `runtime/spvc` |
| `runtime/snxvk/libsnxvk.a` | Vulkan renderer (Windows) | `runtime/snxvk/src`, built by `runtime/snxvk/build_lib.py` |

`<os>` is `win` or `lin`. `SWR_NO_SNXVK=1` builds the Windows runtime without the Vulkan renderer even when the
library is there.

## Recompiling a game

```sh
python3 switchrecomp.py /path/to/game -o MyGame
```

`/path/to/game` holds the `exefs/` and `romfs/` folders extracted from a legally owned copy. `README.txt` has the
full steps and the options.

## License

SwitchRecomp is under the GNU AGPL v3 (`LICENSE.txt`) with the additional terms in `NOTICE.txt`: copies, forks and
derived works keep the credit to "SwitchRecomp by MZ Labs" and the startup screen. The Vulkan renderer in
`runtime/snxvk` is GPL-3.0-or-later, and a runtime built with it is a GPL-3.0 work. Third-party components keep
their own licenses, listed at the end of `README.txt`.
