# D3D12 on the register-file draw engine

Status: planned, not started (noted 2026-11-09, during the Windows bring-up of *Odin Sphere Leifthrasir*).

## Why

Windows has two RSX backends, and they draw by different models:

- `rsx_d3d12_backend.c` is the upstream reference path, the one the in-tree ports were built against. It decodes draws itself through the vtable callbacks and has none of the register-file engine's state handling.
- `rsx_vulkan_backend.c` (Windows since the bring-up, `-DPS3RECOMP_RSX_VULKAN=ON`) runs on `rsx_draw_engine`. Metal does too. The engine owns surfaces, render-to-texture, MRT, depth textures, cube maps, vertex compaction and pipeline keys. The fork's title fixes live there: the framebuffer sRGB rule, colour feedback and texture gamma (`docs/rsx-*.md`).

*Odin* on the legacy D3D12 path: the title runs at 60 flips a second and the backend executes its draws, but the window stays black. On Vulkan the same build draws the title screen. Windows therefore depends on the Vulkan SDK and SDL2 for now, and Microsoft's own graphics stack goes unused (PIX, the D3D12 debug layer, no extra runtime DLL).

## Shape

A third `rsx_draw_backend` (`libs/video/rsx_draw_engine.h`) next to Vulkan and Metal, in a new file beside the legacy backend, which stays as it is for the ports that use it. The engine calls are the contract; nothing in the engine changes for D3D12.

| Engine stage | D3D12 |
|---|---|
| E1: targets, clears, present, readback | committed resources per surface; a flip-model swap chain on the backend's own Win32 window (the legacy backend's window and message pump can be lifted as they are); readback through a `READBACK` heap |
| E2: pipelines and draws | one root signature (constants as root CBVs, a descriptor table per stage); PSOs keyed on the engine's pipeline key, plus a PSO cache with the same key as Vulkan's pipeline cache |
| E3: textures, surface views, samplers | shader-visible CBV/SRV/UAV and sampler heaps, ring-allocated per frame; surface views as SRVs on the render targets |
| E4: depth snapshots | typeless depth formats (`R32G8X24_TYPELESS` / `R24G8_TYPELESS`) so depth can be read as a texture |

Shaders: the decompilers already emit HLSL. D3D12 compiles it with DXC (`dxcompiler.dll`, loaded at run time like `vulkan-1.dll`) to DXIL, with no SPIR-V detour. The translation cache (`rsx_shader_spirv.cpp`) becomes keyed by backend.

Submission: start where Vulkan's draw batching stands (`vk.draw_batch`), so the first cut does not repeat the per-draw fence that limits the Vulkan backend in later scenes (title `docs/linux-performance.md`).

## Acceptance

- `ps3recomp_host` scenes pass on the D3D12 engine path. That needs the host harness to build on Windows first.
- *Odin*: the title screen and the first scenes match the Vulkan backend's frame dumps (`PS3RECOMP_VK_DUMP_LIST` against the same present numbers), and it holds 60 flips a second where Vulkan does.
- Selected with `PS3RECOMP_RSX_ENGINE=dispatch` like the other engine backends; the legacy path stays the default until the engine path covers the in-tree ports.
