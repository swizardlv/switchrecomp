renderer=vulkan_native (SwitchRecomp v190 test)

The Vulkan renderer in libsnxvk.a is ScarletNX's GPU front end and Vulkan renderer
(https://github.com/scarletnx/ScarletNX, main branch commit b5f4f14f2bfc), as vendored by
Metroid Dread Recomp (runtime/snxvk, with its "SWR" hooks). Every global symbol is renamed
snxv_* so it sits next to SwitchRecomp's own GPU front end. runtime/snxvk_glue.c connects it.

Parts of it are ported from Eden and yuzu and keep those projects' copyright lines (Eden Emulator
Project, GPL-3.0-or-later; yuzu Emulator Project, GPL-2.0-or-later): the Maxwell 3D, compute,
DMA and 2D engines, the macro interpreter and its HLE, the GPU command processor (gpu.c), the
shader decoder table, the ASTC decoder and the VIC shaders. The Vulkan backend in src/nx/gpu/vk
and the SPIR-V translator carry the ScarletNX contributors' line; their comments compare with
Eden's behavior in several places.

License: the code in src/ is GPL-3.0-or-later (LICENSE). MZ Labs' original parts in it are
under their original license (LICENSE.apache, NOTICE). A runtime built with libsnxvk.a is a
GPL-3.0 work: anyone given that runtime must also be offered this source.

Third-party libraries in the build: Vulkan-Headers 1.4.365 (Apache-2.0/MIT), volk 1.4.363
(MIT), VulkanMemoryAllocator 3.4.0 (MIT), renderdoc_app.h (MIT).

Rebuild (Linux, mingw-w64 and glslang installed): put the three libraries in deps/ as named
in build_lib.py, then  SWR_NO_DLSS=1 python3 build_lib.py
The runtime links libsnxvk.a automatically for --target win64 --backend win32.
SWR_NO_SNXVK=1 builds the runtime without it.
