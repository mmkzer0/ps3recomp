# RSX Graphics Translation

How ps3recomp translates PS3 RSX GPU commands to host graphics APIs.

---

## Architecture Overview

The RSX graphics pipeline has three layers, each in its own source file:

```
Game code (recompiled C)
    │
    ▼
cellGcmSys HLE                    ← libs/video/cellGcmSys.c (33+ functions)
  │ State management: display buffers, IO mapping, flip/VBlank,
  │ tile/zcull config, command buffer control (put/get/ref),
  │ local memory allocator, offset tables, timestamps
  │
  ▼
RSX Command Processor             ← libs/video/rsx_commands.c
  │ Parses NV47xx FIFO command buffer
  │ Tracks all GPU state with dirty flags
  │ Dispatches to registered backend via callbacks
  │
  ├──▶ Null Backend               ← libs/video/rsx_null_backend.c
  │    Win32 window + GDI clear color (debugging)
  │
  ├──▶ D3D12 Backend              ← libs/video/rsx_d3d12_backend.c
  │    Real GPU rendering (Windows) with compiled shaders
  │
  └──▶ Vulkan Backend             ← opt-in (Linux; PS3RECOMP_RSX_VULKAN)
       Guest shaders on the shared draw engine

Supporting files:
  libs/video/rsx_primitives.h      ← RSX → D3D12 topology mapping + index conversion
  libs/video/rsx_vertex_formats.h  ← RSX → DXGI format mapping
  libs/video/rsx_d3d12_shaders.h   ← Built-in HLSL shaders
  libs/video/cellResc.c            ← Resolution scaling (16 functions)
  libs/video/cellVideoOut.c        ← Video output configuration
```

---

## Layer 1: cellGcmSys (HLE Module)

**File:** `libs/video/cellGcmSys.c` (~800 lines, 33+ functions)

The game interacts with the GPU through `cellGcmSys` — the PS3 SDK's RSX interface. Our HLE implementation provides:

### Initialization
- `cellGcmInit(cmdSize, ioSize, ioAddress)` — Sets up the RSX configuration struct (local memory address/size, IO region, clock frequencies), initializes the offset translation tables, and creates the command buffer control registers.
- `cellGcmGetConfiguration(config)` — Returns the `CellGcmConfig` struct with local/IO memory addresses and sizes. Games read this to know where GPU memory starts.
- `cellGcmGetControlRegister()` — Returns a pointer to the `CellGcmControl` struct (`put`, `get`, `ref` registers). The game writes the `put` pointer to advance the command buffer. **Important:** This must return a guest-accessible address (allocated in guest VM), not a host pointer.

### Display Buffers
- `cellGcmSetDisplayBuffer(id, offset, pitch, width, height)` — Registers a framebuffer region. Games typically register 2 buffers for double-buffering.
- `cellGcmSetFlipMode(mode)` — VSYNC or HSYNC.
- `cellGcmSetFlipHandler(handler)` / `cellGcmSetVBlankHandler(handler)` — Callback registration. In recomp, these store guest function pointers but don't fire automatically (no real VBlank interrupt).
- `cellGcmResetFlipStatus()` / `cellGcmGetFlipStatus()` — Flip state tracking.
- `cellGcmGetLastFlipTime()` — Returns host timestamp via `QueryPerformanceCounter` (Windows) or `clock_gettime` (POSIX).

### RESC output path (`cellResc`, `resc_convert.c`)
A title that composes into an off-screen surface hands it to `cellRescSetSrc` and ends the frame with `cellRescSetConvertAndFlip`. The HLE appends the firmware's stream (`resc_emit_convert_stream`): state, a 4-vertex quad, the display buffer as the colour target, the source as texture unit 0, a vertex program, the fragment program binding and one draw. The draw engine runs it like any other pass: the source is a registered colour surface, so unit 0 samples that surface (render-to-texture), not guest memory.
- **The fragment program is guest memory.** `SHADER_PROGRAM` names the buffer the title passed to `cellRescSetBufferAddress`, and the library's own microcode lives in that buffer only because `cellRescSetBufferAddress` copies it there (libresc `0x02006C70`). Without the copy the buffer holds whatever the title left (zeros): the program samples nothing and every display buffer comes out black. The DC program (one TEX instruction) is held; E0/E4/E8 belong to interlaced and PAL-interpolating modes, whose `cellRescSetConvertAndFlip` is a named stop.
- **How to see it.** `RSX_ENGINE_PASS_DUMP` writes each pass and an index line per pass. The convert pass lists its texture unit (`t0 0:0x<src offset> fA5 1280x720 ... S<n>@<serial>`: `S` names the registered surface that is sampled). A convert pass with no `t0` entry is the signature of an empty fragment program.
- **Test.** `resc_present_metal` (`libs/video/tests/test_resc_present_metal.m`, CMake on Apple with the MSL translation, label `gpu`; skips with exit 77 without a Metal device) renders a gradient into a 1280x720 surface, runs the library's SetSrc/SetBufferAddress/SetConvertAndFlip through the real draw engine and Metal backend, presents buffer 0 and compares every pixel of the display buffer with the source (worst channel error 1).

### Memory Management
- `cellGcmMapMainMemory(ea, size, offset)` — Maps main memory for GPU access. Uses a bump allocator with 1MB alignment and tracks mappings in an offset table.
- `cellGcmMapEaIoAddress(ea, io, size)` — Explicit EA→IO mapping with overlap detection.
- `cellGcmAddressToOffset(address, offset)` — Translates effective address to RSX-relative offset using the populated offset tables.
- `cellGcmGetOffsetTable(table)` — Returns pointers to the ioAddress/eaAddress translation arrays.

### Tile and Zcull
- `cellGcmSetTile(index, ...)` — Configures one of 15 tile regions with compression parameters.
- `cellGcmSetZcull(index, ...)` — Configures one of 8 zcull (hierarchical depth) regions.
- `cellGcmBindTile(index)` / `cellGcmUnbindTile(index)` — Activate/deactivate tile regions.
- `cellGcmSetTileInfo(...)` — Alternative tile configuration (same as SetTile, needed by Tokyo Jungle).

### Labels and Reports
- `cellGcmGetLabelAddress(index)` — Returns a guest pointer to one of 256 label slots (4 bytes each). **Must be in guest VM memory** for the game to read/write via vm_read/vm_write.
- `cellGcmGetReportDataAddress(index)` — Returns pointer to report data (16 bytes: timestamp + value).
- `cellGcmGetNotifyDataAddress(index)` — Notify data area (256 slots, used by Tokyo Jungle).

### Reference
- Based on RPCS3's `rpcs3/Emu/RSX/rsx_methods.cpp` and `cellGcmSys.cpp`
- PS3 SDK documentation: `cell/target/ppu/include/cell/gcm/` headers

---

## Layer 2: RSX Command Processor

**File:** `libs/video/rsx_commands.c` (~400 lines) + `rsx_commands.h` (~300 lines)

### How It Works

On the real PS3, the game writes NV47xx GPU method commands to a FIFO ring buffer. The RSX hardware reads these commands and executes them. In ps3recomp, we intercept the command buffer after the game writes to it and process the methods ourselves.

### Command Buffer Format (NV47xx FIFO)

Each command is a 32-bit header followed by N data words:

```
Header: [31:29] type | [28:18] count | [17:13] subchannel | [12:2] method | [1:0] flags
```

| Type | Name | Behavior |
|------|------|----------|
| 0 | Increasing | Writes data to method, method+4, method+8... |
| 2 | Non-increasing | Writes all data to the same method address |
| 1 | Jump | Changes command buffer read position |
| 3 | Call/Return | Subroutine call/return |

### State Tracking

The `rsx_state` struct tracks ALL GPU state that methods modify:

| Category | Fields | Method Range |
|----------|--------|--------------|
| **Surfaces** | color/depth offsets, pitches, format, clip rect, color target | 0x200-0x23C |
| **Viewport** | x, y, width, height, depth range (clip_min/max) | 0x300-0x398 |
| **Scissor** | x, y, width, height | 0x8C0-0x8C4 |
| **Clear** | color value (ARGB), zstencil value | 0x1D0, 0x1D8, 0x1D94 |
| **Blend** | enable, src/dst factors, equation, color | 0x310-0x350 |
| **Depth** | test enable, function, write mask | 0x304-0x30C |
| **Stencil** | enable, func, ref, mask, fail/zfail/zpass ops | 0x360-0x378 |
| **Cull** | enable, face (front/back), winding (CW/CCW) | 0x2BC-0x2C4 |
| **Color mask** | ARGB channel write enables | 0x028 |
| **Alpha test** | enable, function, reference value | 0x104-0x10C |
| **Textures** | 16 units × 8 registers (offset, format, address, control, filter, rect, border) | 0x1A00-0x1BFF |
| **Vertices** | 16 attribs × format + offset (type, size, stride, enabled) | 0x1680-0x177F |
| **Shaders** | fragment program addr, vertex load slot, constants, output mask | 0x8E4, 0x1E9C, 0x1EFC |
| **Draw** | primitive type, draw_arrays (first+count), draw_indexed | 0x1808, 0x1814, 0x1820 |

Each category has a **dirty flag**. When a method modifies state, the dirty flag is set. Before a draw call (`SET_BEGIN_END`), all dirty state is flushed to the backend via callbacks. This ensures the backend only processes actual changes, not redundant updates.

### Processing Flow

```c
// Game writes methods to command buffer via put pointer
// We process them when the game calls flip or when we detect put > get

int rsx_process_command_buffer(rsx_state* state, const u32* buf, u32 size);
// Parses headers, extracts method+data, calls rsx_process_method for each

int rsx_process_method(rsx_state* state, u32 method, u32 data);
// Updates rsx_state and sets dirty flags
// For draw commands, dispatches to backend
```

### Reference
- NV47xx method register documentation: [envytools rnndb](https://envytools.readthedocs.io/)
- RPCS3's method handling: `rpcs3/Emu/RSX/rsx_methods.cpp`

---

## Layer 3: Graphics Backends

### Backend Interface

**File:** `rsx_commands.h` (the `rsx_backend` struct)

Every backend implements these callbacks:

```c
typedef struct rsx_backend {
    void* userdata;              // backend-specific state pointer

    // Lifecycle
    int  (*init)(void*, u32 w, u32 h);
    void (*shutdown)(void*);
    void (*begin_frame)(void*);
    void (*end_frame)(void*);
    void (*present)(void*, u32 buffer_id);

    // State changes (called when dirty flags set, before draw)
    void (*set_render_target)(void*, const rsx_state*);
    void (*set_viewport)(void*, const rsx_state*);
    void (*set_blend)(void*, const rsx_state*);
    void (*set_depth_stencil)(void*, const rsx_state*);
    void (*set_color_mask)(void*, const rsx_state*);
    void (*set_alpha_test)(void*, const rsx_state*);
    void (*set_shader)(void*, const rsx_state*);
    void (*set_vertex_attribs)(void*, const rsx_state*);

    // Rendering
    void (*clear)(void*, u32 flags, u32 color, float depth, u8 stencil);
    void (*draw_arrays)(void*, u32 primitive, u32 first, u32 count);
    void (*draw_indexed)(void*, u32 primitive, u32 offset, u32 count);
    void (*bind_texture)(void*, u32 unit, const rsx_texture_state*);
} rsx_backend;
```

Register a backend with `rsx_set_backend(&my_backend)`. Only one backend can be active at a time.

### Null Backend

**File:** `libs/video/rsx_null_backend.c`

Debug backend that creates a Win32 window and displays the RSX clear color via GDI. Shows an FPS counter and debug overlay text. Use this to verify the command pipeline works before switching to D3D12.

```c
rsx_null_backend_init(1280, 720, "My Game");  // creates window
// ... game runs ...
rsx_null_backend_pump_messages();              // call each frame
```

### D3D12 Backend

**File:** `libs/video/rsx_d3d12_backend.c` (~990 lines)

Real GPU rendering via Direct3D 12. Current capabilities:

| Feature | Status | Details |
|---------|--------|---------|
| Device creation | **Done** | Feature level 11.0, any D3D12-capable GPU |
| Swap chain | **Done** | Double-buffered, flip-discard, VSync present |
| Clear | **Done** | Clears to RSX clear color (ARGB → float4 RGBA) |
| Root signature | **Done** | Input assembler enabled |
| Pipeline state | **Done** | Vertex-colored shader (position + color) compiled at init |
| Vertex buffer | **Done** | 4MB upload heap, persistently mapped, DrawInstanced |
| Vertex upload | **Done** | Reads position (float3) + color (ubyte4/float4) from guest memory with BE byte-swap |
| Shader compilation | **Done** | Runtime D3DCompile (vs_5_0 / ps_5_0) |
| Frame sync | **Done** | Fence-based, per-frame command allocators |
| Draw calls | **Done** | DrawInstanced with accumulated per-frame vertices |
| Vertex attribs | **Done** | Logs all enabled attribs (type, size, stride, offset) |
| Shader logging | **Done** | Logs fragment/vertex program addresses and output mask |
| Blend state | Logging | Logs blend enable/factors (PSO recreation needed) |
| Depth/stencil | Logging | Logs depth test/stencil state |
| Texture binding | Logging | Logs format, dimensions, guest address. Format decode ready. |
| Texture upload | Scaffold | 25 RSX→DXGI format mappings, upload process documented |
| RSX shaders | Not started | Need NV40 ISA → HLSL translation |
| Depth buffer | Not started | Need depth/stencil texture creation |

**How the D3D12 backend renders a frame:**

```
1. Reset command allocator + command list for current frame
2. Transition render target: PRESENT → RENDER_TARGET
3. Set render target, viewport, scissor
4. ClearRenderTargetView with RSX clear color
5. Bind root signature + PSO + vertex buffer + topology
6. DrawInstanced with accumulated vertex data
7. Transition render target: RENDER_TARGET → PRESENT
8. Present with VSync
9. Signal fence, advance to next frame
```

**Shaders** are compiled at init time from inline HLSL strings using `D3DCompile`. The basic vertex-colored shader is:

```hlsl
// Vertex shader
struct VSInput { float3 pos : POSITION; float4 col : COLOR; };
struct VSOutput { float4 pos : SV_POSITION; float4 col : COLOR; };
VSOutput main(VSInput i) { VSOutput o; o.pos = float4(i.pos, 1.0); o.col = i.col; return o; }

// Pixel shader
float4 main(PSInput i) : SV_TARGET { return i.col; }
```

Additional shader definitions in `rsx_d3d12_shaders.h`: solid color fill, textured quad.

---

## Utility Headers

### Primitive Type Mapping (`rsx_primitives.h`)

Maps RSX primitive types to D3D12 topologies. D3D12 doesn't support triangle fans, quads, or line loops — these need index buffer conversion:

| RSX Primitive | Value | D3D12 | Conversion |
|--------------|-------|-------|------------|
| Points | 1 | POINTLIST | Direct |
| Lines | 2 | LINELIST | Direct |
| Line Loop | 3 | LINESTRIP | Add closing edge index |
| Line Strip | 4 | LINESTRIP | Direct |
| Triangles | 5 | TRIANGLELIST | Direct |
| Triangle Strip | 6 | TRIANGLESTRIP | Direct |
| Triangle Fan | 7 | TRIANGLELIST | Expand: center + edge pairs |
| Quads | 8 | TRIANGLELIST | Split: 2 triangles per quad |
| Quad Strip | 9 | TRIANGLELIST | Expand |

Conversion functions generate index buffers:
- `rsx_convert_triangle_fan(first, count, indices, max)` — (N-2)×3 indices
- `rsx_convert_quads(first, count, indices, max)` — (N/4)×6 indices
- `rsx_convert_line_loop(first, count, indices, max)` — N+1 indices

### Vertex Format Mapping (`rsx_vertex_formats.h`)

Maps RSX vertex attribute types (from `NV4097_SET_VERTEX_DATA_ARRAY_FORMAT`) to DXGI formats for D3D12 input layout descriptors:

| RSX Type | ID | Component Size | DXGI Formats (1/2/3/4 components) |
|----------|-----|---------------|----------------------------------|
| None | 0 | - | Disabled |
| S1 (snorm16) | 1 | 2 bytes | R16_SNORM / R16G16 / - / R16G16B16A16 |
| F (float32) | 2 | 4 bytes | R32_FLOAT / R32G32 / R32G32B32 / R32G32B32A32 |
| SF (float16) | 3 | 2 bytes | R16_FLOAT / R16G16 / - / R16G16B16A16 |
| UB (unorm8) | 4 | 1 byte | R8_UNORM / R8G8 / - / R8G8B8A8 |
| S32K (s16) | 5 | 2 bytes | (same as S1) |
| CMP (packed) | 6 | 4 bytes | Custom (11-11-10 bit) |
| UB256 (uint8) | 7 | 1 byte | R8_UINT / R8G8 / - / R8G8B8A8 |

---

## Register-File Draw Engine: Frame Identity, Accounting and Dumps

**File:** `libs/video/rsx_draw_engine.{c,h}`

The engine is the default path on macOS (`PS3RECOMP_RSX_ENGINE=dispatch`, the backend's default;
`vtable` is the way back). Three contracts matter to anything that compares or captures its output.

**Frame identity.** The engine's present counter and the GCM's flip counter are separate: the flip sink is
reached about three times per guest flip (the queued word `0xE944`, then the head `0xE924`, and `0xE920`).
`rsx_draw_engine_frame_identity()` returns `{flip_id, present_seq, display_buffer}` for the last present:

| field | meaning |
|---|---|
| `flip_id` | 0-based ordinal of the last GCM flip head retired *before* this present — the same ordinal `cellGcmSys` counts for `GCM_DRV_EVENT_FLIP_HEAD`, because the engine sees the same `0xE924` word with bit 31 in the same FIFO order. `RSX_FRAME_NO_FLIP` before the first head. The head's own present carries its own ordinal. |
| `present_seq` | 0-based engine present ordinal: the number the dump files are named by. |
| `display_buffer` | the display buffer the guest queued for this flip (0..7); the head's word is a command, not a buffer id. |

No counter is derived from the other, and no flip id is invented for a present that precedes a head.

**Draw accounting.** Every `BEGIN_END` group is one *submitted* draw. It is either *executed* or dropped
for exactly one reason from `rsx_drop_reason` (no packets, fetch failed, mixed sources, needs expansion,
unsupported topology, no vertices, rebuild failed, no indices, no targets, pipeline failed, debug skip,
texture feedback, index/constant allocation, not ready). The identity

```
submitted == executed + sum(by_reason)
```

therefore holds for every run. `rsx_draw_engine_draw_stats()` snapshots the counters (relaxed-atomic,
process-wide); `rsx_draw_engine_reset_draw_stats()` zeroes them. A drop logs its 1st, 2nd, 4th ... hit as
`[rsx-drop] reason=<name> count=<n>` — the `fw_divergence.h` pattern — so a drop path is never silent even
when the log is cut short.

**Pipeline failures** are counted per fragment program (`rsx_draw_engine_pipeline_failures()`), logged at
powers of two as `[rsx-pipeline-fail] fp=<hash> count=<n>`. The older `[rsx engine] pipeline ...` detail
line still stops at 32; the count does not.

**Dumps.** Both dump families name their files by the frame identity, so no image overwrites another:

- `RSX_ENGINE_PASS_DUMP`: `present_s<present_seq>_f<flip_id>.ppm`, and per pass
  `s<present_seq>_f<flip_id>_pass<NNN>_t<K>.ppm` / `_z.pgm`. `fnone` before the first flip head. The
  index (`passes.txt`) repeats `s=` and `flip=` on every line.
- `PS3RECOMP_METAL_FRAME_DUMP`: `<base>_s<present_seq>_f<flip_id>.ppm`. Presents are selected by
  `PS3RECOMP_METAL_FRAME_DUMP_RANGE=<first>-<last>` or `PS3RECOMP_METAL_FRAME_DUMP_LIST=<a,b,c>` (at most 32
  entries; a longer list is a `config-invalid` stop naming the count), else
  every 120th. A surface format the PPM writer cannot express is an error line plus a counter
  (`rsx_metal_backend_frame_dump_errors()`), never a silent skip; the pass dump counts its own through
  `rsx_draw_engine_pass_dump_errors()`.

---

## Replay: `rsx_replay` (macOS)

**Files:** `tools/rsx_replay/rsx_replay.cpp`, `cellGcm_rsx_replay_step` in `libs/video/cellGcmSys.c`,
`rsx_metal_backend_status` / `rsx_metal_backend_capture_environment` in `libs/video/rsx_metal_backend.m`.

A `ps3diff.rsx/1` capture (docs/PS3DIFF.md) is replayed through the code the title runs, not a copy of
it: `rsx_replay` maps a synthetic guest address space, replays `gcm-init` (`cellGcmSetupContext`) and the
state events (`cellGcmMapEaIoAddress`, `cellGcmUnmapEaIoAddress`, `cellGcmSetDisplayBuffer`), brings up
`rsx_metal_backend` headless, and then feeds the FIFO walker one recorded step at a time. The walker
entry `cellGcm_rsx_replay_step` does three things and nothing else: it checks the walker stands where the step
began (GET and the CALL return slot), it runs the walker with the PUT the live pass saw but stops where
the step left the walker, and it hands a GET_REPORT the recorded timer instead of the host clock. Everything
between (method decoding, the class table, the draw engine, the readback) is the live path.

Because the replay is captured by the same writer, a replay that reproduced the inputs has the live
capture's `capture_hash` at every frame: equal input events, read ordinals included. A cancelled step
(an ACQUIRE the walker sat on for several passes) leaves no read ordinals behind, otherwise the live
hash would depend on how long the title took to write the label.

What a replay cannot reproduce is named, not smoothed over: `present_seq` (the title's frame clock
presents between flips) is reported and never compared; the label window's address comes from the label
blobs; a GET_REPORT timer is the recorded one. Errors and exit statuses: docs/PS3DIFF.md, "Replay".

---

## RSX Shader Translation (Future Work)

The RSX uses a proprietary shader ISA derived from NVIDIA's NV40/NV47 architecture:

### Vertex Programs
- Stored in "transform program memory" on the RSX, loaded via `NV4097_SET_TRANSFORM_PROGRAM_LOAD` (method 0x1E9C).
- Up to 512 instructions per program.
- 4-component vector operations (float4).
- 16 input attributes, 16 output varyings.

### Fragment Programs
- Stored in VRAM or main memory at the address written to `NV4097_SET_SHADER_PROGRAM` (method 0x08E4). The low 2 bits indicate memory location (0=local, 1=main).
- Variable length (terminated by end instruction).
- Operates on interpolated fragment values + textures.

### Translation Strategy

1. **Parse** RSX shader bytecode (NV40 ISA encoding)
2. **Convert** to an intermediate representation (instruction list with typed operands)
3. **Emit** HLSL (for D3D12) or GLSL/SPIR-V (for Vulkan)
4. **Compile** at runtime using `D3DCompile` or `glslang`
5. **Cache** compiled shaders by content hash to avoid recompilation

**Practical approach for initial ports:** Most PS3 games use only 10-50 unique shader programs. Rather than implementing a full NV40 ISA translator, start by:
1. Dumping the raw shader bytecode when `SET_SHADER_PROGRAM` / `SET_TRANSFORM_PROGRAM_LOAD` are called
2. Manually writing equivalent HLSL for each unique shader
3. Building a per-game shader lookup table

**Reference implementations:**
- RPCS3: `rpcs3/Emu/RSX/Program/RSXVertexProgram.cpp` and `RSXFragmentProgram.cpp`
- Mesa NV40: `src/gallium/drivers/nouveau/nv30/` (open-source NV40 shader compiler)
- envytools: NV40 ISA documentation at [envytools.readthedocs.io](https://envytools.readthedocs.io/)

---

## Memory Architecture

The RSX has two memory regions:

| Region | Size | Guest Address | Purpose |
|--------|------|---------------|---------|
| **Local (VRAM)** | 256 MB | Configured at init | Framebuffers, textures, vertex data |
| **Main (mapped)** | Variable | Game-specified | GPU-accessible system memory |

`cellGcmMapMainMemory()` makes a region of main memory visible to the RSX by mapping it into the IO address space. The mapping creates entries in the offset table that `cellGcmAddressToOffset()` uses for translation.

**In ps3recomp**, both regions are in the same flat guest VM address space. The distinction matters for the RSX command processor (which needs to know whether a texture/buffer address is in local or main memory) but not for the host CPU (which can access both via `vm_base + addr`).

---

## Integration Guide (For Game Porters)

### Step 1: Null Backend (Prove Command Flow)

```c
// In your game's main.cpp, before entering the game loop:
rsx_null_backend_init(1280, 720, "My Game");
```

Verify that:
- `cellGcmInit` succeeds (log shows RSX configuration)
- Display buffers are registered
- The game loop calls flip functions

### Step 2: D3D12 Backend (Real Rendering)

```c
// Replace null backend with D3D12:
rsx_d3d12_backend_init(1280, 720, "My Game (D3D12)");
```

The window should show the RSX clear color. Draw calls are logged to stderr.

### Step 3: Identify Draw Patterns

Look at the `[D3D12] draw_arrays(prim=5, first=0, count=36)` log messages:
- `prim=5` = triangles, `prim=6` = triangle strip
- Check which vertex attributes are enabled and what format they use
- Note the draw count — small counts suggest UI elements, large counts suggest 3D geometry

### Step 4: Wire Vertex Upload

Read RSX vertex attribute state from `rsx_state`, translate vertex data from guest memory to the D3D12 vertex buffer, and issue `DrawInstanced`. Use `rsx_to_dxgi_format()` from `rsx_vertex_formats.h` for input layout creation.

### Step 5: Shader Translation

For each unique shader program the game uses:
1. Dump the bytecode (from the guest address in `SET_SHADER_PROGRAM`)
2. Write an equivalent HLSL shader
3. Create a new PSO with the game-specific shader
4. Map the shader address to the PSO in a lookup table

---

## File Reference

| File | Lines | Purpose |
|------|-------|---------|
| `cellGcmSys.c` | 830 | RSX state management HLE (33+ functions) |
| `cellGcmSys.h` | 310 | GCM struct definitions and constants |
| `cellVideoOut.c` | 100 | Video output resolution/mode config |
| `cellResc.c` | 254 | Resolution scaling (16 functions) |
| `rsx_commands.c` | 400 | NV47xx FIFO parser + state tracker |
| `rsx_commands.h` | 300 | Method defines, rsx_state, rsx_backend |
| `rsx_d3d12_backend.c` | 700 | D3D12 rendering (device, PSO, VB, shaders) |
| `rsx_d3d12_shaders.h` | 100 | Built-in HLSL shader strings |
| `rsx_null_backend.c` | 260 | Win32 window + GDI clear |
| `rsx_primitives.h` | 112 | Topology mapping + index conversion |
| `rsx_vertex_formats.h` | 115 | Vertex type → DXGI format mapping |

---

## Method classification (S1, issue #47)

The RSX FIFO carries `(class, method, value)` triples. A subchannel is a binding slot: `SET_OBJECT`
(method 0) names the object, and the object's class gives a method number its meaning. Every method the
walker consumes (`libs/video/cellGcmSys.c`) ends in exactly one verdict, decided by
`libs/video/rsx_method_class.h` — the table there is the source of truth; this section mirrors it, and
`tools/test_rsx_methods.py` fails if the two disagree.

### Verdicts

| Verdict | Meaning | At runtime |
|---|---|---|
| **handled** | a consumer reads it: a draw-time read of the register file, or a dispatch/command handler. The row names the consumer (`file` / `function`) | counted `handled`; the state takes effect |
| **inert** | nothing this renderer does depends on it, for the value the row pins. `data`/`mask` hold that value; `mask = 0` means every value | counted `inert` when the write matches the pin; **missing** when it does not |
| **missing** | a real state this renderer ignores. Either implemented (depth range) or an S2 row below | counted `missing`, first occurrence logged; does not stop and does not invalidate a capture frame |
| **unknown** | no row covers the pair | a capture frame is invalid, and the live walker stops with `rsx-unknown-method` |

**Why `missing` does not stop.** A gap that is known, documented and scheduled is not an unexplained
command: it is a measured divergence (the S2 rows). Stopping on it would make the boot unusable and hide
the census that reports it. An **unknown** pair is different — nothing says what the console did with it,
so the frame it belongs to cannot be trusted, and `ps3diff validate` refuses it.

**Why an unbound write is dropped, and why a live run has none.** A method on a slot with no object bound
reaches no engine: PFIFO has no class to decode it against (envytools `rnndb/fifo/nv_object.xml` — the
subchannel's object is what gives a method its meaning), so the console drops the write. The walker models
that instead of letting the state land in a register file. An *unregistered* handle is not dropped: this
runtime does not know that class's methods, so the pair stays unknown and stops.

The console's channel is bound before any title traffic: the driver's init emits and runs the default-state
subroutine whose first words are the seven `SET_OBJECT`s, and libgcm's own RESC conversion stream
(`resc_convert.c`) writes viewport and depth state without binding anything — which can only work on a
bound channel. `cellGcmInit` therefore installs those seven bindings when it writes the subroutine; the
census's 18 "unbound" observations are that window (the same 18 NV4097 methods, before the first observed
`SET_OBJECT`), not a different class. A slot that is never bound (a bug in the guest, or a synthetic ring)
still has its writes dropped, and `test_rsx_method_census.c` checks both.

**Why the census is exact.** The pin on each inert row is what the driver's default-state routine
(`libs/video/gcm_default_state.c`) writes every flip, and what RPCS3's reset image
(`rsx_state::init()`, `rsx_methods.cpp`) holds: the two agree. A title that writes another value turns
the pair into `missing` at that write, so an allow-list entry can never hide a changed state.

### Evidence tags

| Tag | Meaning |
|---|---|
| `consumer` | handled: the row names the file and function that reads it |
| `oracle-null` | RPCS3 at `d00d8023` binds no handler **and** has no register decoder/accessor for it (`rsx_methods.cpp`, `rsx_methods.h`, `rsx_decode.h`), and the row states why it can |
| `oracle-value` | the oracle reads the register; what it selects is the hardware reset value, which this renderer implements as its fixed behaviour. The row pins that value |
| `order` | a stall/invalidate/ordering hint; the walker is a synchronous in-order interpreter, so it already holds when the method is read |
| `redundant` | the information is carried by bits the renderer does read (a location bit, the one destination surface the 2D path owns) |
| `absent-stage` | the register drives a stage a shader-only NV40 pipeline does not have (fixed-function T&L, combiners, texture matrices); the oracle ignores it too |
| `no-transfer` | the 2D object performs no transfer in this stream, so the register is an input to work that never happens |
| `missing` | an S2 gap: no consumer, value not a reset default the renderer implements |

**Oracle pin.** RPCS3 `v0.0.43-20402-d00d8023` (M3 Amendment 3, item 6), read as a semantics oracle only.
The register names are the psdevwiki list in `docs/rsxfifo.txt` and envytools `rnndb`.

### The table

One row per register, or per array register (`+N` = the next N method numbers). Sorted by class then
method, like the header.

| Class | Method | Register | Verdict | Evidence | Consumer / why |
|---|---|---|---|---|---|
| NV406E | 0x0060 | `NV406E_SET_CONTEXT_DMA_SEMAPHORE` | inert | oracle-null | the DMA object the semaphore offset addresses; RPCS3 binds nullptr and addresses the label  window by offset alone, as gcm_rsx_process_fifo does ... |
| NV4097 | 0x0000 | `NV4097_SET_OBJECT` | handled | consumer | cellGcmSys.c gcm_bind_object: the binding table every later method on the slot decodes against |
| NV4097 | 0x0100 | `NV4097_NO_OPERATION` | inert | oracle-null | RPCS3 resets the register and binds no handler to it; rsx_dispatch.c carries it as an explicit  no-op (mark_class EXEC) |
| NV4097 | 0x0110 | `NV4097_WAIT_FOR_IDLE` | inert | order | libgcm's SetWaitForIdle / SetWriteBackEndLabelForConditional step (docs/rsxfifo.txt): the engine  must have completed everything issued before it. ... |
| NV4097 | 0x0180 | `NV4097_SET_CONTEXT_DMA_NOTIFIES` | handled | consumer | rsx_commands.c rsx_process_method: the NOTIFY (0x0104) path writes the notify slot of this context |
| NV4097 | 0x0184+1 | `NV4097_SET_CONTEXT_DMA_A/B` | inert | redundant | the texture DMA objects; the location of a texture is the format word's DMA bits and the 31-bit  offset (rsx_dsp_get_texture), so the handle adds ... |
| NV4097 | 0x018C | `NV4097_SET_CONTEXT_DMA_COLOR_B` | handled | consumer | rsx_dsp_get_surface: colour target B's location (dma_to_location) |
| NV4097 | 0x0190 | `NV4097_SET_CONTEXT_DMA_STATE` | inert | oracle-null | a state-object DMA handle for the fixed-function T&L state rung; RPCS3 has no decoder and the  engine's state arrives as method writes, not as a ... |
| NV4097 | 0x0194+1 | `NV4097_SET_CONTEXT_DMA_COLOR_A/ZETA` | handled | consumer | rsx_dsp_get_surface: colour target A and zeta locations |
| NV4097 | 0x019C+1 | `NV4097_SET_CONTEXT_DMA_VERTEX_A/B` | inert | redundant | vertex-array DMA objects; the location is the offset word's bit 31 (rsx_dsp_get_vertex_attr),  which is what both the SDK and RPCS3 use |
| NV4097 | 0x01A4 | `NV4097_SET_CONTEXT_DMA_SEMAPHORE` | handled | consumer | rsx_commands.c rsx_process_method: chooses the main-memory or label-window byte swap of 0x1D70/0x1D74 |
| NV4097 | 0x01A8 | `NV4097_SET_CONTEXT_DMA_REPORT` | handled | consumer | cellGcmSys.c gcm_rsx_process_fifo_unlocked: s_report_dma, the GET_REPORT destination |
| NV4097 | 0x01AC+1 | `NV4097_SET_CONTEXT_DMA_CLIP_ID/CULL_DATA` | inert | oracle-null | DMA handles for the clip-id and cull-data objects (conditional rendering). RPCS3 binds nullptr  with no decoder; this renderer models neither feature ... |
| NV4097 | 0x01B4+1 | `NV4097_SET_CONTEXT_DMA_COLOR_C/D` | handled | consumer | rsx_dsp_get_surface: MRT targets C and D locations |
| NV4097 | 0x0200+11 | `NV4097_SET_SURFACE_* (0x200..0x22C)` | handled | consumer | rsx_commands.c process_surface_method into rsx_state, and rsx_dsp_get_surface on the  register-file path: clip, format, pitches, offsets, colour ... |
| NV4097 | 0x0230 | `NV4097_HIERZ_OFFSET (rnndb)` | inert | absent-stage | the hierarchical-Z compression block. Compression is a bandwidth optimization: the depth values  the zeta surface holds are unchanged, and the engine ... |
| NV4097 | 0x0238+1 | `NV4097_SET_CYLINDRICAL_WRAP[0:1]` | inert | oracle-null | NV30-era cylindrical texture wrap; RPCS3 binds nullptr with no decoder; the sampler decodes wrap  from TEX_ADDRESS (rsx_dsp_get_texture.wrap), which ... |
| NV4097 | 0x0240+3 | `NV4097_SET_TEX_MATRIX_ENABLE[0:3] (rnndb)` | inert | absent-stage | fixed-function texture matrices, a T&L stage the VP replaces; RPCS3 has no decoder |
| NV4097 | 0x0280+3 | `NV4097_SET_SURFACE_PITCH_C/D, COLOR_C/DOFFSET` | handled | consumer | the same two consumers for the MRT targets C and D |
| NV4097 | 0x02B8 | `NV4097_SET_WINDOW_OFFSET` | missing | missing | S2: the shader-window offset every window coordinate is translated by. The engine's epilogue  hard-codes offset 0 (rsx_draw_engine.c xf[]), the value ... |
| NV4097 | 0x02BC | `NV4097_SET_WINDOW_CLIP_TYPE` | inert | oracle-null | selects whether the window clip rectangles below discard or only clip; RPCS3 has no decoder for  any of the window-clip registers |
| NV4097 | 0x02C0+15 | `NV4097_SET_WINDOW_CLIP_HORIZONTAL/VERTICAL[0:7]` | inert | oracle-null | eight window clip rectangles (0x2C0..0x2FC); RPCS3 decodes none of them; the engine clips with  the surface clip (rsx_dsp_get_surface) and the ... |
| NV4097 | 0x0300 | `NV4097_SET_DITHER_ENABLE` | inert | oracle-value | enabled at reset (rsx_state::init 0x300=1). A dither matrix maps a value the ROP already holds  at the target's precision (8-bit for the A8R8G8B8 ... |
| NV4097 | 0x031C | `NV4097_SET_BLEND_COLOR` | missing | missing | S2: the constant blend colour. The engine builds every blend from the factors and equation  and never binds a constant colour, so a draw whose factor ... |
| NV4097 | 0x032C | `NV4097_SET_STENCIL_MASK` | handled | consumer | rsx_draw_engine.c build_render_state: rs->s_write_mask (the front stencil write mask) |
| NV4097 | 0x0348 | `NV4097_SET_TWO_SIDED_STENCIL_TEST_ENABLE` | handled | consumer | rsx_draw_engine.c build_render_state: rs->stencil_two_sided |
| NV4097 | 0x034C | `NV4097_SET_BACK_STENCIL_MASK` | missing | missing | S2: the back-face stencil write mask. The engine reads the front mask only; Metal's stencil  descriptor has per-face masks, so the fix is to carry ... |
| NV4097 | 0x0350 | `NV4097_SET_BACK_STENCIL_FUNC` | handled | consumer | rsx_draw_engine.c build_render_state: rs->bs_func |
| NV4097 | 0x0354 | `NV4097_SET_BACK_STENCIL_FUNC_REF` | missing | missing | S2: with two-sided stencil the back faces compare against this reference; the engine passes one  reference (front) to the encoder. Metal shares the ... |
| NV4097 | 0x0358 | `NV4097_SET_BACK_STENCIL_FUNC_MASK` | missing | missing | S2: back-face compare mask, same row as 0x0354 (front mask is read, back is not) |
| NV4097 | 0x035C+2 | `NV4097_SET_BACK_STENCIL_OP_*` | handled | consumer | rsx_draw_engine.c build_render_state: rs->bs_fail/bs_zfail/bs_zpass |
| NV4097 | 0x0368 | `NV4097_SET_SHADE_MODE` | inert | oracle-value | GL_SMOOTH at reset (0x1D01), which is what the MSL vertex outputs interpolate as. A flat shade  mode would need an interpolation qualifier per ... |
| NV4097 | 0x036C | `NV4097_SET_BLEND_ENABLE_MRT` | handled | consumer | rsx_draw_engine.c build_render_state: rs->blend_mrt |
| NV4097 | 0x0370 | `NV4097_SET_COLOR_MASK_MRT` | handled | consumer | rsx_draw_engine.c build_render_state: rs->color_mask_mrt |
| NV4097 | 0x0374 | `NV4097_SET_LOGIC_OP_ENABLE` | inert | oracle-value | logic op disabled at reset (enable 0); the engine has no logic-op stage (Metal has none either) |
| NV4097 | 0x0378 | `NV4097_SET_LOGIC_OP` | inert | oracle-value | the logic operation itself (copy at reset); it cannot be applied while the enable above is 0,  and the row pins the reset value so a change is ... |
| NV4097 | 0x037C | `NV4097_SET_BLEND_COLOR2` | inert | oracle-value | the second blend constant, 0 at reset; only a blend factor naming the constant-2 source reads  it, and the engine implements the factors the title ... |
| NV4097 | 0x0380+2 | `NV4097_SET_DEPTH_BOUNDS_*` | handled | consumer | rsx_draw_engine.c build_render_state rs->depth_bounds_test and the per-draw set_depth_bounds  (0x0384/0x0388 as raw floats) |
| NV4097 | 0x0394 | `NV4097_SET_CLIP_MIN` | handled | consumer | rsx_process_method rsx_state.clip_min / rsx_dsp_depth_range: the near end of the depth range  passed to the Metal viewport (rsx_draw_engine.c draw, ... |
| NV4097 | 0x0398 | `NV4097_SET_CLIP_MAX` | handled | consumer | ...the far end, same consumers |
| NV4097 | 0x03B0 | `NV4097_SET_CONTROL0` | inert | oracle-value | the mip-rounding mode bit (rnndb MIPMAP_ROUNDING, bit 20) is set at reset; it chooses which mip  level a coordinate exactly between two levels ... |
| NV4097 | 0x03B8 | `NV4097_SET_LINE_WIDTH` | inert | oracle-value | line width 8 = 1/8-pixel units = 1.0 px at reset; the engine draws the host's 1-px lines,  which is what that value asks for |
| NV4097 | 0x03BC | `NV4097_SET_LINE_SMOOTH_ENABLE` | inert | oracle-value | line smoothing off at reset; the engine draws unsmoothed lines, as the reset value asks |
| NV4097 | 0x03C0 | `NV4097_SET_ANISO_SPREAD` | inert | oracle-null | the anisotropic LOD spread table; RPCS3 binds nullptr with no decoder and its samplers take the  anisotropy from TEX_FILTER alone ... |
| NV4097 | 0x03C4+14 | `NV4097 fixed-function TEX_MATRIX/TEX_GEN block` | inert | absent-stage | 0x3C4..0x3FC is the NV30 fixed-function texture-matrix/generator rung (rnndb gives it no NV40_3D  variant); the VP replaces it and RPCS3 ignores the ... |
| NV4097 | 0x0400+15 | `NV4097_UNK0400[0:15] (nv40.h NV40TCL_UNK0400)` | inert | oracle-null | 16 words of 0x7421 the default state writes; RPCS3 stores the same reset values under  registers[0x400] with no name and no decoder, so not even the ... |
| NV4097 | 0x0440+15 | `NV4097 dither matrix (rnndb DITHER_MATRIX)` | inert | oracle-null | the 4x4 ordered dither table the default state writes; see 0x0300: the ROP output and the target  are both 8-bit, where this table is the identity; ... |
| NV4097 | 0x08CC | `NV4097_SET_FOG_MODE` | inert | oracle-value | CELL_GCM_FOG_MODE_EXP at reset; the fog stage applies the FP's fog coordinate (o5.x) with this  mode, and this renderer has no fog stage: RPCS3 ... |
| NV4097 | 0x08D0+2 | `NV4097_SET_FOG_PARAMS` | inert | oracle-value | the fog equation constants, all 0 at reset (a zero exponential constant makes the fog factor  0); same stage as 0x08CC |
| NV4097 | 0x0A0C | `NV4097_SET_POINT_CENTER_MODE` | inert | oracle-null | whether a point sprite is centred on the pixel; RPCS3 binds nullptr with no decoder and the  engine draws POINTS with the host's convention, value 0 ... |
| NV4097 | 0x0A60+2 | `NV4097_SET_POLY_OFFSET_*_ENABLE` | inert | oracle-value | polygon-offset enables, all 0 at reset: no offset is applied, which is what the engine does  (it sets no depth bias). A title that enables one needs ... |
| NV4097 | 0x0A78+1 | `NV4097_SET_POLYGON_OFFSET_*` | inert | oracle-value | offset factor and bias, both 0 at reset; see 0x0A60 |
| NV4097 | 0x0B00+15 | `NV4097_SET_TEXTURE_CONTROL2[0:15]` | inert | oracle-null | the per-unit trilinear/aniso optimization hints (rnndb TEX_FILTER_OPTIMIZATION); RPCS3 binds  nullptr with no decoder: a quality/bandwidth hint, not ... |
| NV4097 | 0x0B40+9 | `NV4097_SET_TEX_COORD_CONTROL[0:9]` | inert | oracle-null | per-texcoord interpolation mode (2D/linear, centroid). RPCS3 binds nullptr with no decoder and  interpolates perspective-correct always, as this ... |
| NV4097 | 0x1428 | `NV4097_SET_SPECULAR_ENABLE` | inert | absent-stage | fixed-function lighting: whether the specular term is added after texturing; the VP computes  colours. RPCS3 has no decoder |
| NV4097 | 0x142C | `NV4097_SET_TWO_SIDE_LIGHT_EN` | inert | absent-stage | fixed-function two-sided lighting (front/back colour select). With a VP the back-face colours  are outputs o3/o4, which no consumer of this renderer ... |
| NV4097 | 0x1450 | `NV4097_SET_PERFORMANCE_PARAMS` | inert | oracle-null | PGRAPH performance counters/thresholds; RPCS3 has no decoder and nothing in the pipeline reads a  performance register |
| NV4097 | 0x1454 | `NV4097_SET_FLAT_SHADE_OP` | inert | absent-stage | fixed-function flat shading of the primary/secondary colour; RPCS3 has no decoder (it gets flat  shading from the shader's own interpolation ... |
| NV4097 | 0x145C | `NV4097_SET_EDGE_FLAG` | inert | absent-stage | fixed-function edge flag; RPCS3 has no decoder |
| NV4097 | 0x1478 | `NV4097_SET_USER_CLIP_PLANE_CONTROL` | inert | oracle-null | which of the VP's clip distances (o[9..14] on NV30 numbering) the fixed-function clipper tests;  this renderer clips with the host's clip volume ... |
| NV4097 | 0x147C | `NV4097_SET_POLYGON_STIPPLE` | inert | absent-stage | polygon stipple, a fragment-stage mask of the fixed-function ROP; RPCS3 decodes it but the engine  has no stipple stage; value 0 = disabled, the ... |
| NV4097 | 0x1710 | `NV4097_INVALIDATE_VERTEX_CACHE_FILE` | inert | order | drop the post-transform vertex cache; the engine fetches every vertex from guest memory for every  draw (rsx_vertex_fetch.c), so no cache exists to ... |
| NV4097 | 0x1714 | `NV4097_INVALIDATE_VERTEX_FILE` | handled | consumer | rsx_dispatch.c dsp_fill_classes marks it EXEC: the engine keeps no post-transform vertex  cache, so the invalidate has nothing to drop and every draw ... |
| NV4097 | 0x1738+1 | `NV4097_SET_VERTEX_DATA_BASE_OFFSET/INDEX` | handled | consumer | rsx_dsp_vertex_data_base_offset/index: added to every vertex fetch and indexed draw |
| NV4097 | 0x17CC | `NV4097_SET_ZPASS_PIXEL_COUNT_ENABLE` | inert | oracle-value | zpass counting enable, 0 at reset; RPCS3 binds set_zcull_pixel_count_enable, this renderer has  no occlusion query (GET_REPORT answers 0xFFFF for a ... |
| NV4097 | 0x1800 | `NV4097_GET_REPORT` | handled | consumer | cellGcmSys.c gcm_rsx_process_fifo_unlocked: writes the report slot (or main-memory report  area) named by SET_CONTEXT_DMA_REPORT, with the chunk's ... |
| NV4097 | 0x1828+1 | `NV4097_SET_FRONT/BACK_POLYGON_MODE` | inert | oracle-null | fill/line/point rasterization mode; RPCS3 binds nullptr with a decoder it never acts on, and the  engine rasterizes triangles: the title writes the ... |
| NV4097 | 0x1838 | `NV4097_SET_POLY_SMOOTH_ENABLE` | inert | oracle-null | polygon antialiasing via coverage; RPCS3 decodes it and no host pipeline models coverage AA; the  engine's MSAA comes from the surface's antialias ... |
| NV4097 | 0x1D64 | `NV4097_SET_INDEXED_CONSTANT_READ_LIMITS` | inert | oracle-null | bounds the constant indices a VP may read (0x02000000 at reset); RPCS3 has no decoder, and the  decompiler bounds every constant read to ... |
| NV4097 | 0x1D78 | `NV4097_SET_ZMIN_MAX_CONTROL` | inert | oracle-value | depth-test direction/rounding controls; the engine reads the engine-visible depth state from  DEPTH_FUNC/MASK/TEST (0x0A6C..0x0A74); RPCS3 binds ... |
| NV4097 | 0x1D7C | `NV4097_SET_ANTI_ALIASING_CONTROL` | inert | oracle-value | AA sample placement; the engine takes the sample grid from the surface format's antialias field  (rsx_dsp_get_surface), which is what libgcm sets ... |
| NV4097 | 0x1D80 | `NV4097_SET_SURFACE_COMPRESSION` | inert | oracle-null | zeta/colour compression enable; no compression in this renderer (the surface is read and  written uncompressed), RPCS3 has no decoder; value 3 = the ... |
| NV4097 | 0x1D88 | `NV4097_SET_SHADER_WINDOW` | missing | missing | S2: window height, origin (top/bottom) and pixel centre. The engine's epilogue hard-codes a  top-left origin (rsx_draw_engine.c xf[1]/xf[5]) while ... |
| NV4097 | 0x1D98+1 | `NV4097_SET_CLEAR_RECT_HORIZONTAL/VERTICAL` | inert | oracle-value | the rectangle CLEAR_SURFACE covers; 0x0FFF0000/0x0FFF0000 at reset, i.e. the full 4096x4096  window, so clearing the attachment is the same clear. ... |
| NV4097 | 0x1DA4 | `NV4097_SET_CLIP_ID_TEST_ENABLE` | inert | oracle-null | clip-id conditional rendering enable, 0 at reset; RPCS3 has no decoder and this renderer draws  every batch |
| NV4097 | 0x1DB4 | `NV4097_SET_LINE_STIPPLE` | inert | oracle-null | line stipple enable/pattern; RPCS3 has no decoder; the engine draws lines solid, value 0 |
| NV4097 | 0x1E94 | `NV4097_SET_TRANSFORM_EXECUTION_MODE` | inert | absent-stage | VPE execution mode (fixed-function vs program). The engine always runs the VP  (rsx_dsp_vp_start / TRANSFORM_PROGRAM_LOAD) because libgcm titles ... |
| NV4097 | 0x1E98 | `NV4097_SET_RENDER_ENABLE` | inert | oracle-value | the VPE render-enable word; the engine runs the vertex program unconditionally (libgcm titles  always bind one, and RPCS3's set_render_mode only ... |
| NV4097 | 0x1EE0 | `NV4097_SET_POINT_SIZE` | inert | oracle-value | point size 1.0 at reset; the engine draws POINTS as 1-px points, which is 1.0 |
| NV4097 | 0x1EE4+1 | `NV4097_SET_POINT_PARAMS_ENABLE` | inert | oracle-value | point parameters/sprite enable, both 0 at reset; point sprites need a sprite coordinate the  engine does not generate, and the reset value asks for ... |
| NV4097 | 0x1EF8 | `NV4097_SET_TRANSFORM_TIMEOUT` | inert | oracle-null | the VPE's timeout and watchdog threshold: a hang detector for the transform engine, not state a  draw reads. RPCS3 binds nullptr with no decoder |
| NV4097 | 0x1FC4+4 | `NV4097_SET_ATTRIB_COLOR/TEX_COORD/TEX_COORD_EX/UCLIP0/UCLIP1` | inert | absent-stage | the fixed-function attribute *routing* (which VP output carries COL0/TEX0/...). The VP decompiler  emits the hardware's reset routing directly ... |
| NV4097 | 0x1FE0 | `NV4097_SET_REDUCE_DST_COLOR` | inert | oracle-null | whether the reduce stage may read the destination; RPCS3 has no decoder; value 1 = reset |
| NV4097 | 0x1FE8 | `NV4097_SET_NO_PARANOID_TEXTURE_FETCHES` | inert | oracle-null | relaxes texture-fetch bounds checks; RPCS3 has no decoder. The engine validates every texture  span before sampling (rsx_texture_layout.c, ... |
| NV4097 | 0x1FEC | `NV4097_SET_SHADER_PACKER` | handled | consumer | rsx_draw_engine.c fragment-pipeline key (the register is read with the fragment program and  SHADER_CONTROL to decide the output packing) |
| NV0039 | 0x0000 | `NV0039_SET_OBJECT` | handled | consumer | cellGcmSys.c gcm_bind_object / gcm_nv0039_method case 0 |
| NV0039 | 0x0180 | `NV0039_SET_CONTEXT_DMA_NOTIFIES` | inert | oracle-null | the notifier object the hardware would write on BUFFER_NOTIFY; RPCS3's nv0039::buffer_notify  never reads it (the notify argument only reaches a ... |
| NV0039 | 0x0184 | `NV0039_SET_CONTEXT_DMA_BUFFER_IN` | handled | consumer | cellGcmSys.c gcm_nv0039_method: the copy source location |
| NV0039 | 0x0188 | `NV0039_SET_CONTEXT_DMA_BUFFER_OUT` | handled | consumer | cellGcmSys.c gcm_nv0039_method: the copy destination location |
| NV0039 | 0x030C | `NV0039_OFFSET_IN` | handled | consumer | gcm_nv0039_method / gcm_nv0039_copy |
| NV0039 | 0x0310 | `NV0039_OFFSET_OUT` | handled | consumer | gcm_nv0039_method / gcm_nv0039_copy |
| NV0039 | 0x0314+4 | `NV0039_PITCH_IN..FORMAT` | handled | consumer | gcm_nv0039_method / gcm_nv0039_copy |
| NV0039 | 0x0328 | `NV0039_BUFFER_NOTIFY` | handled | consumer | gcm_nv0039_method: the copy trigger |
| NV3062 | 0x0000 | `NV3062_SET_OBJECT` | handled | consumer | cellGcmSys.c gcm_bind_object: routes the slot's 2D methods |
| NV3062 | 0x0180 | `NV3062_SET_CONTEXT_DMA_NOTIFIES` | inert | oracle-null | the 2D object's notify context; gcm_2d_method never raises a notify (the copy runs inline,  cellGcmSys.c) and RPCS3 binds nullptr with no decoder |
| NV3062 | 0x0300 | `NV3062_SET_COLOR_FORMAT` | handled | consumer | gcm_2d_method: s_gcm2d.color_fmt |
| NV3062 | 0x0304 | `NV3062_SET_PITCH` | handled | consumer | gcm_2d_method: s_gcm2d.pitch |
| NV3062 | 0x030C | `NV3062_SET_OFFSET_DESTIN` | handled | consumer | gcm_2d_method: s_gcm2d.dst_offset |
| NV308A | 0x0000 | `NV308A_SET_OBJECT` | handled | consumer | cellGcmSys.c gcm_bind_object: routes the slot's 2D methods |
| NV308A | 0x0180 | `NV308A_SET_CONTEXT_DMA_NOTIFIES` | inert | oracle-null | see NV3062 0x0180: the 2D path raises no notify and RPCS3 has no decoder |
| NV308A | 0x0184+5 | `NV308A_SET_CONTEXT_COLOR_KEY..BETA4` | inert | no-transfer | the inline-transfer raster-op context (colour key, clip rectangle, pattern, ROP, beta). Odin  sends no NV308A_COLOR (0x400) and no SIZE_OUT/SIZE_IN ... |
| NV308A | 0x019C | `NV308A_SET_CONTEXT_SURFACE` | inert | redundant | names the destination surface object; the 2D path has exactly one destination state, the NV3062  object's (s_gcm2d), which is the object this handle ... |
| NV308A | 0x02FC | `NV308A_SET_OPERATION` | inert | no-transfer | the transfer's blend operation; see 0x0184: no transfer is issued by this object in the census |
| NV308A | 0x0300 | `NV308A_SET_COLOR_FORMAT` | inert | redundant | the colour format of the inline-transfer words (the default state writes 4 on this slot every flip, ahead of the NV309E bind); the inline write stores 32-bit words into the destination surface, whose format is the NV3062 object's (s_gcm2d.color_fmt), and RPCS3 reads no NV308A_SET_COLOR_FORMAT. Pinned to the default state's value. Until the class keyed the handler this word was read as NV309E's SET_FORMAT |
| NV309E | 0x0000 | `NV309E_SET_OBJECT` | handled | consumer | cellGcmSys.c gcm_bind_object: routes the slot's 2D methods |
| NV309E | 0x0180 | `NV309E_SET_CONTEXT_DMA_NOTIFIES` | inert | oracle-null | see NV3062 0x0180 |
| NV309E | 0x0184 | `NV309E_SET_CONTEXT_DMA_IMAGE` | inert | redundant | the destination image object; the swizzled path writes local memory unconditionally  (gcm_2d_method: cellGcmResolveLocated(1, s_nv309e.offset)), and ... |
| NV3089 | 0x0000 | `NV3089_SET_OBJECT` | handled | consumer | cellGcmSys.c gcm_bind_object: routes the slot's 2D methods |
| NV3089 | 0x0180 | `NV3089_SET_CONTEXT_DMA_NOTIFIES` | inert | oracle-null | see NV3062 0x0180 |
| DRIVER | 0x0000 | `SCE_DRIVER_SET_OBJECT` | handled | consumer | cellGcmSys.c gcm_bind_object: the driver object makes 0xE9xx/0xEBxx words on the slot driver commands |

### Census

The 120 s Odin boot (`scratch/h1b-census`, the last `GCM_METHOD_STATS` block) carried 214 observations:
NV4097 177, NV308A 10, NV309E 3, NV3062 2, NV3089 2, NV406E 1, driver 1, and 18 NV4097 methods seen
before the first observed bind. Those 18 are the same methods as some of the 177, so the stream has 196
distinct `(class, method)` pairs. Classified: **21 handled, 170 inert, 5 missing, 0 unknown**; the 18
pre-bind observations classify as the NV4097 rows they belong to. `libs/video/tests/test_rsx_method_census.c`
replays the pair table through the real walker (214 writes + 7 binds: 34 handled, 180 inert, 7 missing,
0 unknown).

### Handled states the walker itself consumes

`SET_OBJECT` (the binding), `GET_REPORT` (`0x1800`) and `SET_CONTEXT_DMA_REPORT` (`0x1A8`) are consumed by
`cellGcmSys.c`; `SET_CONTEXT_DMA_NOTIFIES` (`0x0180`) and `SET_CONTEXT_DMA_SEMAPHORE` (`0x01A4`) by
`rsx_commands.c` (the notify write and the back-end label write); the depth range by the engine (below).

### What consumed a word (the verdict) and where a word goes (the routing)

A method is interpreted in the class SET_OBJECT bound on its subchannel (`gcm_method_class`), never by the
subchannel's number: libgcm's default state binds 0 NV4097, 1 NV0039, 3 NV3062, 4 NV309E, 5 NV308A, 6 NV3089 and
7 the driver object (`cellGcmSetupContext` binds the same table), and a title may bind NV4097 on another slot.
Methods `0x04..0xFC` are NV406E channel control on every subchannel, and the driver's `0xE9xx`/`0xEBxx` words
are addressed by the method number itself; neither needs a binding. Everything else on a slot with no object is
dropped and counted (`unbound`); on a slot bound to a handle this table does not know it is unknown and stops.

Only words of a slot bound to NV4097 (and the driver's) reach the 3D register-file mirrors (the draw engine and
the live-draw engine). Both keep one NV4097 register file and mask the subchannel off the method, so a 2D word
used to be stored as the NV4097 register of the same offset: NV308A's `0x018C/0x0194/0x0198` are
`SET_CONTEXT_DMA_COLOR1/COLOR0/ZETA` there, and the title's every-flip default state moved the colour and depth
surfaces to RSX local memory. `libs/video/tests/test_gcm_class_routing.c` runs the real walker and engine.

The verdict records the consumer that took the word. With the draw engine on and no vtable backend the walker
no longer feeds `rsx_state` (#466), so the legacy state machine's answer cannot stand for a register the engine
reads: those registers carry a class-table row in `rsx_dispatch.c` (`dsp_fill_classes`), each naming the read.
Added with the gate's follow-up: the scissor (`0x08C0/0x08C4`), the vertex textures (`0x0900..0x097C`), the
frequency-divider mask (`0x1FC0`) and the VP branch bits (`0x1FF8`).

### Implemented in S1: the depth range

`SET_CLIP_MIN` (`0x0394`) / `SET_CLIP_MAX` (`0x0398`) are part of every viewport: libgcm's
`cellGcmSetViewport` writes them in the same packet (`docs/rsxfifo.txt`, "SetClipMinMax" / "SetViewport").
Window z is mapped to `[near, far]` before the depth test and the depth write. The path:
`rsx_commands.c` (`rsx_state.clip_min/clip_max`) and `rsx_dispatch.c` (`rsx_dsp_depth_range`) →
`rsx_draw_engine.c` (the optional `set_depth_range` backend hook, called per draw) →
`rsx_metal_backend.m` (`MTLViewport.znear/zfar`, both the engine and the vtable path).
Tests: `rsx_depth_range` (host) and `depth_range_metal` (0.25/0.75 → depth buffer 0.25/0.75).

### Known gaps (S2)

| State | What the renderer does today | S2 row |
|---|---|---|
| `NV4097_SET_WINDOW_OFFSET` (`0x02B8`) | the window transform has a fixed zero offset | add a per-draw window offset to the viewport epilogue; needs RPCS3's `window_offset_x/y` semantics pinned first |
| `NV4097_SET_SHADER_WINDOW` (`0x1D88`) | the NDC→window epilogue hard-codes a top-left origin and integer pixel centres | decode height/origin/pixel-centre and apply it in the epilogue; needs a reference frame to settle the convention (M3 non-goal) |
| `NV4097_SET_BACK_STENCIL_MASK` / `_FUNC_REF` / `_FUNC_MASK` (`0x034C`, `0x0354`, `0x0358`) | two-sided stencil compares and writes with the front mask and reference | carry the back masks in `rsx_be_render_state`; Metal has per-face masks but one shared reference, so a differing back reference needs emulation |
| `NV4097_SET_BLEND_COLOR` (`0x031C`) | constant blend factors use the host default instead of the guest's colour | add the four floats to the pipeline/encoder state |
