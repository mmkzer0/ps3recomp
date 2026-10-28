# HLE Modules — Complete Reference

Detailed documentation for every HLE (High-Level Emulation) module in ps3recomp. This goes beyond the status table to explain what each module does, how it's implemented, and what platform backends it uses.

---

## Table of Contents

1. [Module Architecture](#module-architecture)
2. [System / Core](#system--core)
3. [Filesystem](#filesystem)
4. [Input](#input)
5. [Audio](#audio)
6. [Video / Graphics](#video--graphics)
7. [Media / Codec](#media--codec)
8. [Font / Text](#font--text)
9. [Network](#network)
10. [PSN / NP](#psn--np)
11. [SPU / Multitasking](#spu--multitasking)
12. [Synchronization](#synchronization)
13. [Hardware / Peripherals](#hardware--peripherals)
14. [Miscellaneous](#miscellaneous)

---

## Module Architecture

Every HLE module follows the same structural pattern:

### File Organization

```
libs/<category>/
├── cellFoo.h     # Public API: constants, enums, error codes, struct definitions
└── cellFoo.c     # Implementation + module registration
```

### Implementation Pattern

```c
// cellFoo.c

#include "cellFoo.h"
#include "ps3emu/module.h"

/* ─── Internal State ──────────────────────────── */
static bool s_initialized = false;
static SomeState s_state;

/* ─── Internal Helpers ────────────────────────── */
static int32_t foo_validate_params(uint32_t param) { ... }

/* ─── Public HLE Functions ────────────────────── */
int32_t cellFooInit(uint32_t flags)
{
    if (s_initialized) return CELL_FOO_ERROR_ALREADY_INIT;
    // ... initialization ...
    s_initialized = true;
    return CELL_OK;
}

int32_t cellFooEnd(void)
{
    if (!s_initialized) return CELL_FOO_ERROR_NOT_INIT;
    // ... cleanup ...
    s_initialized = false;
    return CELL_OK;
}

/* ─── Lifecycle Hooks ─────────────────────────── */
static int32_t cellFoo_init(void) { return CELL_OK; }
static void cellFoo_shutdown(void) { s_initialized = false; }

/* ─── Module Registration ─────────────────────── */
DECLARE_PS3_MODULE(cellFoo, "cellFoo")
{
    REGISTER_FUNC(cellFooInit);
    REGISTER_FUNC(cellFooEnd);
    // ... more functions ...
    SET_MODULE_INIT(cellFoo_init);
    SET_MODULE_SHUTDOWN(cellFoo_shutdown);
}
```

### Design Principles

1. **Static allocation** — all internal tables have fixed sizes; no runtime malloc
2. **Platform abstraction** — `#ifdef _WIN32` blocks isolate Win32/POSIX differences
3. **Real backends preferred** — use actual OS services (threads, sockets, timers) rather than stubs where possible
4. **Error codes match PS3** — every function returns the same error codes the real PS3 would
5. **Init/End pattern** — most modules follow `Init()` / `End()` lifecycle with guard checks

---

## System / Core

### sysPrxForUser

**File:** `libs/system/sysPrxForUser.c`
**Purpose:** The "standard library" of PS3 — provides the foundational services that every game uses

This is the most critical module. It provides:

**Lightweight Mutexes (lwmutex) and Condition Variables (lwcond):**
- Implemented in `runtime/syscalls/sys_lwsync.cpp`, not in `sysPrxForUser.c`; see [RUNTIME.md](RUNTIME.md#lwmutex-and-lwcond)
- Per-object FIFO wait queues; ownership by guest thread id; recursive and non-recursive modes
- Signal one, signal all, signal to a thread, timed wait (`CELL_ETIMEDOUT`)

**Thread Functions:**
- `sys_ppu_thread_create` — creates real host threads
- `sys_ppu_thread_exit`, `sys_ppu_thread_yield`, `sys_ppu_thread_join`
- Thread-local storage via static pools

**Heap Management:**
- `_sys_malloc`, `_sys_free`, `_sys_realloc`, `_sys_memalign`
- Wraps host `malloc`/`free` with PS3 guest address translation

**I/O Functions:**
- `sys_tty_write` — debug output to stdout
- `_sys_printf`, `_sys_snprintf` — formatted output

**String/Memory Operations:**
- `_sys_memset`, `_sys_memcpy`, `_sys_memmove`
- `_sys_strlen`, `_sys_strcmp`, `_sys_strncmp`, `_sys_strcpy`, `_sys_strncpy`, `_sys_strcat`

**SPU printf server hooks** (`libs/system/sys_spu_printf.c`):
- `_sys_spu_printf_initialize` stores four guest callback OPDs; `_sys_spu_printf_attach_group`/`detach_group`/`attach_thread`/`detach_thread` call the matching one through `ppu_guest_call` and return its result, or `CELL_ESTAT` while it is not registered; `_sys_spu_printf_finalize` clears them
- State is process-global behind one lock; the guest callback runs with the lock released

### sys_crashdump

**File:** `libs/system/sys_crashdump.c`
**Purpose:** Crash dump user log area registration

- `sys_crash_dump_set_user_log_area(index, new_entry)`: registers a user log buffer (`label[16]`, `addr`, `size`) at slot 0..127. Returns `CELL_OK` on success, or `CELL_EINVAL` (`0x80010002`) if `index > 127` or `new_entry` is NULL (oracle: RPCS3 `sys_crashdump.cpp` @ `e53ac18f`). Out-of-bounds guest pointers stop with `hle-guest-range`.
- `sys_crash_dump_get_user_log_area(index, entry)`: retrieves the registered user log buffer for slot `index`. Returns `CELL_OK` on success (zeroed if slot unpopulated, matching oracle), or `CELL_EINVAL` if `index > 127` or `entry` is NULL.

### cellSysmodule

**File:** `libs/system/cellSysmodule.c`
**Purpose:** Module load/unload manager

Tracks which PS3 system modules are loaded. Games call `cellSysmoduleLoadModule(id)` before using any library.

**Implementation:**
- 55+ module ID constants (e.g., `CELL_SYSMODULE_NET = 0x0000`, `CELL_SYSMODULE_IO = 0x0001`)
- Boolean loaded/unloaded tracking per module ID
- Name lookup table mapping IDs to human-readable names
- `cellSysmoduleIsLoaded()` — check if a module is loaded

### cellSysutil

**File:** `libs/system/cellSysutil.c`
**Purpose:** System utility callbacks, parameters, and services

**Callback System:**
- Up to 4 callback slots
- `cellSysutilRegisterCallback(slot, handler, userdata)`
- `cellSysutilCheckCallback()` — polls and dispatches pending callbacks

**System Parameters:**
- `cellSysutilGetSystemParamInt(id, &value)` — query system settings
- Language (English default), enter button assignment (Circle/Cross), date format, timezone, parental level
- `cellSysutilGetSystemParamString(id, buf, size)` — query string parameters (username, etc.)

**Background Music:**
- `cellSysutilEnableBGMPlayback()` / `cellSysutilDisableBGMPlayback()`
- `cellSysutilGetBGMPlaybackStatus()` — returns disabled by default

**System Cache:**
- `cellSysutilGetSystemCacheDirectory(path)` — returns a temp cache path
- `cellSysutilMountSystemCache(id, &info)` — creates/mounts cache directory
- `cellSysutilClearSystemCache()` — deletes cache contents

**Disc Game Check:**
- `cellDiscGameGetBootDiscInfo(&info)` — returns disc metadata
- `cellDiscGameRegisterDiscChangeCallback(handler)` — register disc-change callback

---

## Filesystem

### cellFs

**File:** `libs/filesystem/cellFs.c`
**Purpose:** Complete file I/O with PS3-to-host path mapping

All filesystem operations translate PS3 paths (e.g., `/dev_hdd0/game/NPUA80001/`) to host filesystem paths (e.g., `./hdd0/game/NPUA80001/`).

**Functions:**
- `cellFsOpen(path, flags, &fd, attrs, attrs_size)` — open file with PS3-style flags
- `cellFsRead(fd, buf, size, &bytes_read)` — read with big-endian byte count output
- `cellFsWrite(fd, buf, size, &bytes_written)` — write
- `cellFsClose(fd)` — close
- `cellFsLseek(fd, offset, whence, &pos)` — seek
- `cellFsStat(path, &stat)` — get file info (size, timestamps, mode) — output in big-endian
- `cellFsFstat(fd, &stat)` — stat by file descriptor
- `cellFsOpendir(path, &fd)` / `cellFsReaddir(fd, &entry)` / `cellFsClosedir(fd)` — directory enumeration
- `cellFsMkdir(path, mode)` — create directory
- `cellFsRename(from, to)` — rename
- `cellFsUnlink(path)` — delete file
- `cellFsRmdir(path)` — delete empty directory
- `cellFsTruncate(path, size)` / `cellFsFtruncate(fd, size)` — truncate
- `cellFsChmod(path, mode)` — change permissions

### cellFsUtility

**File:** `libs/filesystem/cellFsUtility.c`
**Purpose:** Higher-level filesystem helpers

- `cellFsUtilityMkdirAll(path)` — recursive mkdir (like `mkdir -p`)
- `cellFsUtilityGetFileSize(path, &size)` — get file size
- `cellFsUtilityReadFile(path, buf, size, &read)` — read entire file
- `cellFsUtilityWriteFile(path, buf, size)` — write entire file
- `cellFsUtilityCopyFile(src, dst)` — copy file
- `cellFsUtilityFileExists(path)` / `cellFsUtilityDirExists(path)` — existence checks

---

## Input

### cellPad

**File:** `libs/input/cellPad.c`
**Purpose:** PlayStation 3 gamepad (DualShock 3 / Sixaxis) input

**Backends:**
- **Windows**: XInput (Xbox controller API — maps naturally to DS3)
- **Cross-platform**: SDL2 GameController API

**Features:**
- Up to 7 controller ports (PS3 maximum)
- Digital buttons: Cross, Circle, Square, Triangle, L1, R1, L2, R2, L3, R3, Start, Select, PS
- Analog sticks: Left X/Y, Right X/Y (0–255 range, 128 = center)
- Analog triggers: L2, R2 (0–255 pressure)
- Pressure-sensitive face buttons (0–255 per button)
- Sixaxis motion sensor data (accelerometer X/Y/Z, gyroscope)
- Rumble/vibration motor control (small/large motor)

**Key Functions:**
- `cellPadInit(max_ports)` — initialize pad system
- `cellPadGetData(port, &data)` — poll controller state
- `cellPadGetInfo2(&info)` — get connected controller info
- `cellPadSetActDirect(port, &params)` — set rumble
- `cellPadClearBuf(port)` — clear input buffer
- `cellPadEnd()` — shutdown

**CellPadData structure:**
```c
struct CellPadData {
    s32 len;            // Number of data words (usually 24)
    u16 button[CELL_PAD_MAX_CODES];
    // [0]:  digital buttons bitmask
    // [1]:  digital buttons bitmask (continued)
    // [2]:  right stick X (0-255)
    // [3]:  right stick Y (0-255)
    // [4]:  left stick X (0-255)
    // [5]:  left stick Y (0-255)
    // [6]:  right pressure (0-255)
    // [7]:  left pressure (0-255)
    // [8]:  up pressure (0-255)
    // [9]:  down pressure (0-255)
    // [10]: triangle pressure (0-255)
    // [11]: circle pressure (0-255)
    // [12]: cross pressure (0-255)
    // [13]: square pressure (0-255)
    // [14]: L1 pressure (0-255)
    // [15]: R1 pressure (0-255)
    // [16]: L2 pressure (0-255)
    // [17]: R2 pressure (0-255)
    // [18]: sensor X (accelerometer)
    // [19]: sensor Y (accelerometer)
    // [20]: sensor Z (accelerometer)
    // [21]: sensor G (gyroscope)
};
```

### cellKb

**File:** `libs/input/cellKb.c`
**Purpose:** USB keyboard input

- Raw mode (scancodes) and ASCII mode (translated characters)
- Modifier key tracking (Shift, Ctrl, Alt, Meta)
- LED state control (Caps Lock, Num Lock, Scroll Lock)
- Ring buffer of key events
- Up to 7 keyboard ports

### cellMouse

**File:** `libs/input/cellMouse.c`
**Purpose:** USB mouse input

- Delta accumulation (X/Y movement since last poll)
- Button state (left, right, middle, extra buttons)
- Scroll wheel events
- Buffered ring-buffer mode for high-frequency polling
- Up to 7 mouse ports

---

## Audio

### cellAudio

**File:** `libs/audio/cellAudio.c`
**Purpose:** PS3 audio output with real mixing

**Backends:**
- **Windows**: WASAPI (Windows Audio Session API) — low-latency exclusive mode
- **Cross-platform**: SDL2 Audio

**Implementation:**
- Background mixing thread runs at **5.33 ms intervals** (256 samples at 48 kHz)
- Up to **8 audio ports**, each producing 256-sample blocks
- Games write PCM float samples to port buffers in guest memory
- Mixing thread reads all active ports, sums them, and outputs to the host audio device
- Supports mono, stereo, and 7.1 surround output
- 7.1 → stereo downmix for hosts without surround

**Key Functions:**
- `cellAudioInit()` — initialize audio system, start mixing thread
- `cellAudioPortOpen(&params, &port)` — open an audio port
- `cellAudioPortStart(port)` — start producing audio
- `cellAudioGetPortTimestamp(port, block, &stamp)` — timing for audio sync
- `cellAudioSetNotifyEventQueue(queue)` — notify when a block is consumed
- `cellAudioPortClose(port)` — close port
- `cellAudioQuit()` — stop mixing thread, release audio device

### cellVoice

**File:** `libs/audio/cellVoice.c`
**Purpose:** Voice chat system (stub)

Port management only — no actual voice data capture or playback. Games can create/connect/disconnect voice ports, but data paths are empty.

### cellMic

**File:** `libs/audio/cellMic.c`
**Purpose:** Microphone input (stub)

Reports no microphone attached. All device queries return `CELL_MIC_ERROR_DEVICE_NOT_FOUND`.

---

## Video / Graphics

### cellGcmSys

**File:** `libs/video/cellGcmSys.c`
**Purpose:** RSX graphics system management — the interface between the PPU and the GPU

This is the most complex graphics module, responsible for managing the RSX command buffer and memory.

**Command Buffer:**
- `cellGcmSetFlipCommand(ctx, buffer_id)` — queue a buffer flip
- Command buffer control structure (`CellGcmControl`):
  - `put` — PPU write pointer (advances as commands are written)
  - `get` — RSX read pointer (advances as commands are consumed)
  - `ref` — reference counter for synchronization

**Memory Management:**
- Local memory bump allocator for VRAM (cellGcmSys manages its own heap within the RSX's 256 MB)
- `cellGcmMapMainMemory(address, size, &offset)` — map main memory for GPU access
- `cellGcmAddressToOffset(address, &offset)` — translate address to RSX-relative offset
- IO offset table: 1 MB page granularity mapping from EA (effective address) to IO offset

**Display Management:**
- `cellGcmSetFlipHandler(handler)` — register flip completion callback
- `cellGcmSetVBlankHandler(handler)` — register vertical blank callback
- `cellGcmSetFlipMode(mode)` — `CELL_GCM_DISPLAY_HSYNC` or `VSYNC`
- Buffer management: up to 8 display buffers

**Tile/Zcull Configuration:**
- 15 tile regions — configurable pitch, format, base, size (8 KB tiles)
- 8 zcull regions — Z-buffer compression configuration
- `cellGcmSetTileInfo(index, location, offset, size, pitch, comp, base, bank)`
- `cellGcmSetZcullInfo(index, location, offset, width, height, cullStart, zFormat, aaFormat, zCullDir, zCullFormat, sFunc, sRef, sMask)`

**Report/Label Areas:**
- 256 report data slots (16 bytes each) — GPU query results
- 256 label slots — CPU/GPU synchronization markers
- Timestamps via platform-native high-resolution timers

### cellResc

**File:** `libs/video/cellResc.c`
**Purpose:** Resolution scaling and conversion

- Configure display modes (720p, 1080p, 480p, etc.)
- Buffer management (up to 8 color buffers)
- Aspect ratio correction (4:3, 16:9)
- Interlace/de-interlace lookup tables
- Flip and VBlank handler registration
- Scaling factor computation

### cellVideoOut

**File:** `libs/video/cellVideoOut.c`
**Purpose:** Video output device configuration

- Resolution configuration: all PS3 resolution IDs (480i, 480p, 576i, 576p, 720p, 1080i, 1080p)
- Default: 720p (1280×720)
- Device state queries (connected, available resolutions)
- Color space and scan mode configuration

---

## Media / Codec

### cellPamf

**File:** `libs/codec/cellPamf.c`
**Purpose:** PAMF (PlayStation Architecture Media Format) container parser

Parses the PS3's native media container format:
- Big-endian header with magic, version, data offset, data size
- Stream enumeration (video, audio, subtitle)
- Codec info per stream:
  - **Video**: H.264/AVC (profile, level, framerate, resolution)
  - **Audio**: ATRAC3+, LPCM, AC3 (channels, sample rate, bit depth)
- Entry point navigation for seeking
- Stream type/codec queries

### cellVdec (Partial)

**File:** `libs/codec/cellVdec.c`
**Purpose:** Video decoder

- Opens/closes decoder instances for H.264 and MPEG2
- Accepts AU (Access Unit) submission via `cellVdecDecodeAu`
- Fires `AUDONE` callback after AU submission
- **Does not actually decode** — needs FFmpeg integration for real frame output
- Returns empty/black frames when the game reads decoded output

### cellAdec (Partial)

**File:** `libs/codec/cellAdec.c`
**Purpose:** Audio decoder

- Same pattern as cellVdec but for audio codecs
- Supports AAC, ATRAC3+, MP3 codec types
- AU submission with AUDONE callback
- **Does not actually decode** — needs FFmpeg for real audio samples

### cellDmux (Partial)

**File:** `libs/codec/cellDmux.c`
**Purpose:** AV demuxer

- Opens demuxer instances for PAMF streams
- Elementary Stream (ES) enable/disable
- Stream feed and reset
- AU retrieval stubs
- Flush callbacks
- **Management only** — no actual demuxing of data

### Image Decoders

**cellJpgDec** (`libs/codec/cellJpgDec.c`):
- JPEG decoding via stb_image backend
- Header-only parsing mode + full decode mode
- File source and memory buffer source
- RGBA and ARGB output formats

**cellPngDec** (`libs/codec/cellPngDec.c`):
- PNG decoding via stb_image backend
- RGBA, ARGB, and RGB output formats
- Alpha handling configuration

**cellGifDec** (`libs/codec/cellGifDec.c`):
- GIF decoding via stb_image backend
- Single-frame decode (no animation)

### Codec Variants

**cellAdecAtrac3p** (`libs/codec/cellAdecAtrac3p.c`):
- ATRAC3plus audio decoder — Sony's advanced lossy codec used for game audio/BGM
- Open/close/decode/reset lifecycle with up to 8 handles
- Outputs silence (zero-filled PCM): 2048 samples per frame, 16-bit
- Supports mono/stereo, 44.1 kHz and 48 kHz sample rates

**cellAdecCelp8** (`libs/codec/cellAdecCelp8.c`):
- CELP8 (Code-Excited Linear Prediction) voice codec — low-bandwidth voice chat audio
- 8 kHz mono, 160 samples per frame (20 ms)
- Multiple bitrate modes: 5700, 6200, 7700, 14400 bps
- Outputs silence (zero-filled 16-bit PCM)

**cellVdecDivx** (`libs/codec/cellVdecDivx.c`):
- DivX/Xvid video decoder for media playback
- Open/close/decode/reset lifecycle with up to 4 handles
- Profiles: Mobile, Home Theater, HD
- Outputs black frames (zero-filled YUV data)

### Image Encoders

**cellJpgEnc** / **cellPngEnc** (`libs/codec/cellJpg/PngEnc.c`):
- Handle management (create/destroy)
- Encode operation returns `CELL_EOPNOTSUPP` (needs stb_image_write integration)

### cellVpost

**File:** `libs/codec/cellVpost.c`
**Purpose:** Video post-processing (stub)

Handle management and query support, but exec operation is a no-op (no actual color conversion or scaling).

### cellSail

**File:** `libs/codec/cellSail.c`
**Purpose:** High-level media player

State machine-driven media player:
- States: Initialized → Opened → Started → Running → Paused → Closed
- `cellSailPlayerInitialize()` — create player
- `cellSailPlayerOpenStream()` — open media source
- `cellSailPlayerStart()` — begin playback
- `cellSailPlayerStop()` / `cellSailPlayerPause()` — control
- **Immediate finish** — opens and immediately reports playback complete (stub)

---

## Font / Text

### cellFont

**File:** `libs/font/cellFont.c`
**Purpose:** TrueType font rendering

**Backend:** stb_truetype (public domain header-only TrueType rasterizer)

**Features:**
- Font library initialization with configurable cache
- Open fonts from file path or memory buffer
- Glyph rendering to bitmap (8-bit alpha)
- Font metrics (ascent, descent, leading, line height)
- Glyph metrics (advance width, bearing, bounding box)
- Fallback metrics when stb_truetype is not available
- Multiple font instances (up to 16 concurrent)

### cellFontFT

**File:** `libs/font/cellFontFT.c`
**Purpose:** FreeType-based font rendering

An alternative font rendering path that wraps FreeType2 (as opposed to cellFont which uses stb_truetype).

**Features:**
- Up to 16 concurrent font instances
- Fallback metrics when no real FreeType library is present:
  - Ascender = 0.8 × requested size
  - Descender = -0.2 × requested size
  - Line height = ascender - descender
  - Advance width = 0.6 × size (monospace approximation)
- Empty glyph bitmaps (zeroed pixel data) for rendering stubs
- Kerning queries return zero offset (no kerning data without FreeType)

**Key Functions:**
- `cellFontFTInit(config, &lib)` — initialize FreeType font library
- `cellFontFTEnd(lib)` — shutdown
- `cellFontFTOpenFontFile(lib, path, index, &font)` — open from file
- `cellFontFTOpenFontMemory(lib, data, size, index, &font)` — open from buffer
- `cellFontFTCloseFont(font)` — close font instance
- `cellFontFTGetGlyphMetrics(font, glyph, &metrics)` — get glyph dimensions
- `cellFontFTGetFontMetrics(font, &metrics)` — get font-wide dimensions
- `cellFontFTRenderGlyph(font, glyph, &image)` — render glyph to bitmap
- `cellFontFTGetKerning(font, left, right, &kern)` — get kerning offset

### cellFreeType

**File:** `libs/font/cellFreeType.c`
**Purpose:** Low-level FreeType2 library wrapper

Minimal wrapper that provides direct access to FreeType2. Reports FreeType 2.4.12 (the version shipped with PS3 firmware 4.x).

**Key Functions:**
- `cellFreeTypeInit(config, &lib)` — initialize FreeType library
- `cellFreeTypeEnd(lib)` — shutdown
- `cellFreeTypeGetVersion(&version)` — returns {major=2, minor=4, patch=12}
- `cellFreeTypeGetLibrary(&lib)` — get current library handle

### cellL10n

**File:** `libs/font/cellL10n.c`
**Purpose:** Unicode and character set conversion

**Supported Conversions:**
- UTF-8 ↔ UTF-16 (both BE and LE)
- UTF-8 ↔ UTF-32
- UTF-8 ↔ UCS-2
- ISO-8859-1 ↔ UTF-8
- ASCII ↔ UTF-8
- Generic converter API: `l10n_convert(src_code, dst_code, src, src_len, dst, dst_len)`

Each conversion function follows the pattern `L10nResult SJIStoUTF8(...)` with proper bounds checking, surrogate pair handling, and error reporting.

---

## Network

### sys_net

**File:** `libs/network/sys_net.c`
**Purpose:** Full BSD socket API

Wraps the host OS socket API with PS3 error code translation:

| PS3 Function | Windows | POSIX |
|---|---|---|
| `sys_net_bnet_socket` | `socket()` | `socket()` |
| `sys_net_bnet_bind` | `bind()` | `bind()` |
| `sys_net_bnet_listen` | `listen()` | `listen()` |
| `sys_net_bnet_accept` | `accept()` | `accept()` |
| `sys_net_bnet_connect` | `connect()` | `connect()` |
| `sys_net_bnet_send` | `send()` | `send()` |
| `sys_net_bnet_recv` | `recv()` | `recv()` |
| `sys_net_bnet_sendto` | `sendto()` | `sendto()` |
| `sys_net_bnet_recvfrom` | `recvfrom()` | `recvfrom()` |
| `sys_net_bnet_poll` | `WSAPoll()` | `poll()` |
| `sys_net_bnet_select` | `select()` | `select()` |
| `sys_net_bnet_setsockopt` | `setsockopt()` | `setsockopt()` |
| `sys_net_bnet_gethostbyname` | `gethostbyname()` | `gethostbyname()` |

**Features:**
- 128-socket descriptor table mapping PS3 FDs to host FDs
- PS3-specific `SO_NBIO` non-blocking mode support
- Winsock error code → PS3 errno translation
- Big-endian sockaddr conversion

### cellHttp

**File:** `libs/network/cellHttp.c`
**Purpose:** Real HTTP/1.1 client

**Not a stub** — performs actual HTTP requests over native TCP sockets.

**Implementation:**
1. DNS resolution via `getaddrinfo()`
2. TCP socket creation and connection
3. HTTP request formatting with method, path, Host header, custom headers
4. Response parsing: status line, headers (Content-Length, Connection), body
5. Streaming body receive for large responses
6. Per-transaction socket lifecycle (connect → send → recv → close)
7. Socket timeouts via `SO_RCVTIMEO` / `SO_SNDTIMEO`

**Key Functions:**
- `cellHttpCreateClient(&client)` — create HTTP client
- `cellHttpCreateTransaction(&client, method, url, &trans)` — create request
- `cellHttpSendRequest(trans, body, body_len)` — send HTTP request
- `cellHttpRecvResponse(trans, &buf, &buf_len)` — receive response
- `cellHttpGetResponseStatusCode(trans, &code)` — get HTTP status

### cellNetCtl

**File:** `libs/network/cellNetCtl.c`
**Purpose:** Network connection management

- Detects real host IP address via host OS APIs
- Reports NAT type (Type 2 / moderate by default)
- Connection state tracking (connected/disconnected)
- Handler callbacks for network state changes
- Reports real network interface info

### cellHttps

**File:** `libs/network/cellHttps.c`
**Purpose:** HTTPS (HTTP over TLS) client

Extends cellHttp with SSL/TLS support. No actual TLS handshake or encryption — certificate management APIs accept data but verification always succeeds.

**Features:**
- Up to 8 concurrent HTTPS handles
- SSL version configuration (SSLv3, TLSv1)
- CA certificate and client certificate management (accepted but not processed)
- Verify level configuration (peer and host verification flags)
- Certificate info queries (returns NOT_SUPPORTED — no real TLS connection)

**Key Functions:**
- `cellHttpsInit(config, &handle)` — initialize with SSL config (version, verify flags)
- `cellHttpsEnd(handle)` — shutdown
- `cellHttpsSetCACert(handle, cert, size, type)` — set CA certificate (X.509 or PKCS12)
- `cellHttpsSetClientCert(handle, cert, certSize, key, keySize)` — set client certificate
- `cellHttpsClearCerts(handle)` — clear all certificates
- `cellHttpsSetVerifyLevel(handle, verifyPeer, verifyHost)` — configure verification
- `cellHttpsGetCertInfo(handle, &info)` — get peer certificate info (returns NOT_SUPPORTED)

### cellSsl

**File:** `libs/network/cellSsl.c`
**Purpose:** SSL/TLS support

- Init/end lifecycle
- Certificate management stubs
- Cryptographic RNG: `BCryptGenRandom` (Windows) / `/dev/urandom` (POSIX)
- No actual TLS handshake (games needing HTTPS would need a real TLS library)

---

## PSN / NP

All sceNp modules simulate an offline PSN environment. Multiplayer features report "not connected" while local features (trophies, user storage) work with host-local persistence.

### sceNpTrophy

**File:** `libs/network/sceNpTrophy.c`
**Purpose:** Trophy/achievement system

- **Persistent JSON storage** — trophies are saved to a JSON file on the host
- Unlock with timestamps
- Progress tracking (percentage-based trophies)
- `sceNpTrophyRegisterContext(ctx, handle, statusCb, arg, opt)` follows the firmware's shape (FW 4.93 `libsysutil_np_trophy`): the status callback runs inline with `NOT_INSTALLED` or `INSTALLED` (from the install record, the store file for the community id) and a negative return is `PROCESSING_ABORTED`; the call then **blocks** while setup, progress, finalize and complete statuses (RPCS3 counts) are queued gradually, paced by vblanks, and delivered by `cellSysutilCheckCallback` on the polling thread (never on the blocked one), and returns after the completion was consumed or at `trophy.register_wait_cap_vblanks` (counted, logged; a title that registers on the thread that polls gets its statuses from its later polls)
- `sceNpTrophyAbortHandle(handle)` (FW 4.93 `0x020014C4`) checks only the handle table (`INVALID_ARGUMENT` outside `1..4`, `UNKNOWN_HANDLE` for a free slot, with or without `Init`) and marks the handle: until `sceNpTrophyDestroyHandle` every call naming it, `RegisterContext` (also one blocked on it) and a second abort included, returns `SCE_NP_TROPHY_ERROR_ABORT` (`0x80022910`)
- Trophy configuration from TROPCONF.SFM
- Trophy types: Bronze, Silver, Gold, Platinum
- `sceNpTrophyUnlockTrophy(ctx, handle, id, &platinum)` — unlock and check for platinum

### sceNpTus

**File:** `libs/network/sceNpTus.c`
**Purpose:** Title User Storage — local per-user data storage

- Set/get/add/delete variables by slot index
- Data storage per slot (binary blobs)
- Async polling pattern (start operation → poll → get result)
- All data stored locally (not uploaded to PSN)

### sceNp

**File:** `libs/network/sceNp.c`
**Purpose:** Core PSN identity

- Configurable fake PSN username
- Fake NP ID generation
- Account region (Americas/Europe/Asia) and age
- Online status queries

### sceNpBasic / sceNpClans / sceNpCommerce / sceNpMatching2 / sceNpSignaling / sceNpSns

All offline stubs — function signatures are implemented, state management works, but all online operations return `NOT_CONNECTED` or `SERVER_NOT_AVAILABLE`.

---

## SPU / Multitasking

### cellSpurs (Partial)

**File:** `libs/spurs/cellSpurs.c`
**Purpose:** SPURS task management framework

SPURS (SPU Runtime System) is Sony's framework for distributing work across SPUs. ps3recomp implements the management layer:

**Implemented:**
- SPURS instance creation and destruction
- Workload management (add/remove workloads)
- Task management (create/start/wait tasks)
- Taskset management (create/destroy tasksets)
- Event flags with **real OS blocking**:
  - Windows: `CRITICAL_SECTION` + `CONDITION_VARIABLE`
  - POSIX: `pthread_mutex` + `pthread_cond`
  - Side table maps event flag IDs to blocking state
  - `cellSpursEventFlagWait()` truly blocks until condition met
  - `cellSpursEventFlagSet()` broadcasts to wake waiters

**Not implemented:**
- Actual SPU program execution on host threads
- SPU job scheduling and dispatch
- DMA coordination between SPU tasks

### cellSpursJq

**Files:** `libs/spurs/cellSpursJq.c` (PPU surface), `libs/spurs/jq2/`
(executor), `libs/spurs/jq2/jq2.h` (C contract)
**Purpose:** SPURS JobQueue2 — Port2 job submission, descriptor pools, and
queue lifecycle

The module implements the 17 `cellSpursJq` exports a title imports, with the
firmware's argument order and error codes. Every handler decodes 32-bit guest
EAs and calls `jq2.h`; the ring, scheduler, descriptor pool, LV2 event flags
and SPU workers live in `libs/spurs/jq2/`. Jobs run through
`spu_run_spurs_job` (interpreted or lifted); no job is ever credited without
running, and a command-ring entry the executor does not model is a named stop.

**Exports:**
- `_cellSpursCreateJobQueue(rev, sdk, spurs, queue, attr, name, ring, depth,
  workers, extra)` (10 args) and
  `_cellSpursCreateJobQueueWithJobDescriptorPool(...)` (12 args, adds `pool`
  and `poolAttr`) — create the queue block, command ring, event flags and
  workers
- `cellSpursShutdownJobQueue(queue)` / `cellSpursJoinJobQueue(queue, &out)` —
  teardown; Join writes `queue+0xF04` to the guest out argument
- `cellSpursJobQueuePort2Create(port, queue)` / `...Port2Destroy(port)` —
  caller-owned 128-byte port block, queue slot in `queue+0x580`
- `_cellSpursJobQueuePort2PushJobBody(port, desc, size, tag, flags)` —
  descriptor fetched at admission (not copied at push)
- `_cellSpursJobQueuePort2CopyPushJobBody(port, src, size, copySize, tag,
  flags)` — synchronous snapshot into the queue's size-class descriptor pool
- `cellSpursJobQueuePort2PushSync(port, tagMask, flags)` /
  `...Port2Sync(port, mode)` — per-tag barriers and completion waits
- the seven `cellSpursJobQueueAttribute*` exports — Initialize and the
  MaxSizeJobDescriptor / GrabParameters / DoBusyWaiting / SubmitWithEntryLock /
  IsJobTypeMemoryCheck / IsHaltOnError setters

**Attribute layout (`CellSpursJobQueueAttribute`):**

| Offset | Type | Meaning |
|---|---|---|
| `+0x00` | u32 | caller's; the exports never touch it |
| `+0x04` | u32 | grab/class parameter, 1..16 (Initialize: 4) |
| `+0x08` | u32 | maximum descriptor size, 256..1023, 128-aligned (Initialize: 256) |
| `+0x0C` | u8 | SubmitWithEntryLock |
| `+0x0D` | u8 | DoBusyWaiting |
| `+0x0E` | u8 | IsHaltOnError |
| `+0x0F` | u8 | IsJobTypeMemoryCheck |
| `+0x10` | u32 | grab parameters, 0..255 (Initialize: 255; create reads byte `+0x13`) |

**Return codes:** 0 or `JQ2_ERR_*` (`0x80410A01` AGAIN, `0x80410A02` INVAL,
`0x80410A0A` BUSY, `0x80410A0F` STAT, `0x80410A10` ALIGN, `0x80410A11`
NULL_POINTER). The exports do not produce any other value.

Other firmware Jq exports (`cellSpursJobQueueSendSignal`,
`cellSpursJobQueueSemaphoreTryAcquire`, the non-Port2 job APIs, ...) are not
part of this surface. They are not registered here, so an import reaches the
`unresolved-nid` named stop instead of a fabricated `CELL_OK`.

### cellDaisy

**File:** `libs/spurs/cellDaisy.c`
**Purpose:** SPURS Daisy Chain — lock-free FIFO pipes

Producer-consumer pipeline framework for streaming data between PPU and SPU. Unlike the other SPURS stubs, cellDaisy has a **real ring buffer implementation** that actually stores and delivers data.

**Implementation:**
- Up to 32 concurrent pipes
- Each pipe has a configurable entry size and depth (max 256 entries)
- Real ring buffer with head/tail pointers and count tracking
- `Push` copies data into the ring buffer; `Pop` retrieves it
- `malloc`/`free` for ring buffer allocation

**Key Functions:**
- `cellDaisyPipeAttributeInitialize(&attr, direction, entrySize, depth)` — configure pipe
- `cellDaisyCreatePipe(spurs, &pipe, &attr, buffer, size)` — create pipe with ring buffer
- `cellDaisyDestroyPipe(&pipe)` — destroy and free ring buffer
- `cellDaisyPipePush(&pipe, data)` / `cellDaisyPipeTryPush(&pipe, data)` — enqueue data
- `cellDaisyPipePop(&pipe, data)` / `cellDaisyPipeTryPop(&pipe, data)` — dequeue data
- `cellDaisyPipeGetCount(&pipe, &count)` — number of entries in pipe
- `cellDaisyPipeGetFreeCount(&pipe, &count)` — available slots

**Pipe Directions:**
- `PPU_TO_SPU` — PPU produces, SPU consumes
- `SPU_TO_PPU` — SPU produces, PPU consumes
- `SPU_TO_SPU` — inter-SPU pipeline

### cellFiber

**File:** `libs/spurs/cellFiber.c`
**Purpose:** Cooperative PPU fibers and persistent UtilWorkerControl workers

- The eight UtilWorkerControl exports use guest EAs and firmware arities:
  `_cellFiberPpuUtilWorkerControlAttributeInitialize(attr,sdk)`,
  `InitializeWithAttribute(control,attr)`,
  `CreateFiber(control,fiber,entry,u64Arg,priority,stack,size,attr)`,
  `RunFibers(control)`, `Wakeup(control)`, `SendSignal(fiber,out)`,
  `Shutdown(control)` and `Finalize(control)`.
- The established scheduler API remains available: Initialize/FinalizeScheduler,
  CreateFiber, RunFibers, WaitSignal/SendSignal, JoinFiber, Yield, Exit and Self.
  Guest priority queues are FIFO within priorities 0..3. There is no invented
  0x400-byte stack minimum in the common creation checks.
- Fiber and scheduler attributes have their BE magic/SDK defaults; the worker
  attribute is 0x180 bytes with SDK at +0x108. Scheduler and worker-control
  attribute handling reflects FW 4.93 `InitializeScheduler` (`0x02001CA4-0x02002000`),
  which reads only `+0x10` autoCheckFlags (`0x02001E10`), `+0x11` debuggerSupport
  (`0x02001E28`), and `+0x14` autoCheckFlagsIntervalUsec (`0x02001E04`); unread
  bytes in `+0x0C..+0xFF` and `+0x100..+0x17F` are ignored. Deviation: libfiber's
  debugger table does not exist in this HLE, so `scheduler+0x168` stays 0 and fibers
  are not registered; this is debugger-only and not visible to guest scheduling.
  `+0x14` is converted to timebase ticks at `scheduler+0x138` with `scheduler+0x140`
  zeroed. If `+0x10` is non-zero, HLE stops with `fiber-unsupported reason=auto-check`
  because periodic stack-canary checks (`0x02002C58`, `0x0200150C`) are not modelled.
  Fiber attribute (`fiber == 1`) settings beyond defaults continue to stop with
  `fiber-unsupported`, naming the field.
- Guest fiber state, priority queues and lifetime links are authoritative and
  published through reservation transactions. Signal bit0x10 coalesces; a
  signal waiter uses bit0x08; notification clears waiting and sets runnable
  bit1 exactly once. All public and host notifications share this transition.
- Worker RunFibers remains on the calling PPU thread until Shutdown, including
  when every current fiber is blocked or finished. The control's LV2 event
  flag at +0x208 wakes parked workers; the packed +0x200 word and +0x288
  worker bitmap remain guest-visible. Wakeup never executes fibers in its
  caller. Finalize returns STAT while active and BUSY while workers remain.
- CreateFiber on a worker control notifies the parked workers as Wakeup does,
  so a fiber created after RunFibers began is run. The firmware's creation
  path (`02002494-24AC`) is not decoded far enough to name its own wake; the
  notification is the minimum a persistent worker needs [INFERENCE].
- JobQueue2 Port2Sync uses the same state: a fiber's blocking Sync registers
  kind 0 with its own EA (`cellFiberPpuSelf`) and parks in
  `cellFiberPpuWaitSignal`; the credit path calls `ps3_fiber_notify`, the
  same transition as SendSignal. See `JQ2.md` for the port line.
- Host mechanics: ucontext on POSIX and Windows fibers on Windows; each fiber
  owns its native stack and `ppu_context`. After first dispatch, a context is
  pinned to that worker to preserve native TLS references in suspended frames.
  The executing worker supplies its guest thread identity and r13. Self reads
  the current fiber EA at `r13 + tlsOffset + 0x20`.
- Suspension preserves `g_active_ctx`, recursion depth and complete live vcall
  storage. The loader's existing slot bridge exchanges a fiber-owned storage
  pointer, so no live frame array is copied on each switch. Game-less hosts
  return NULL slots only because no indirect dispatcher is linked; the
  title-style worker fixture requires the loader's real non-NULL slots.

**Pinned RE:** FW4.93 libfiber SHA-256
`3be3192ee20ea993ad28e498bf96d992373a2057c2ce0ca4323e6798ed183430`.

- `_cellFiberPpuInitialize(u64 anchor)` models the instruction-level ABI, not
  an inferred SDK C declaration or a success code. `02003D18-3D28` computes
  the full wrapped `anchor-r13` in r3 and stores its low word in the module's
  TLS-offset slot (initially FFFFFFFF, the NOSYSINIT sentinel). The handler
  reads r13 from the calling PPU context. The SDK wrapper's complete storage
  allocation remains unproved; only established accessed spans are used.
- Shutdown helper `02001768-17D0` reports whether
  `scheduler[+0xE8] != schedulerEA+0xE8`; `020058F4` / `02005914` /
  `02005964` turn that into BUSY. A blocked fiber keeps its lifetime node:
  creation links `fiber+0x320` at `0200246C-2490`, and `02001464-1488`
  unlinks it. Shutdown cannot cancel or fake completion for blocked work.
  Generic Finalize likewise cannot discard pending stacks
  (`02002864-286C`, `02002A5C-2A74`).
- **Signal routing correction:** helper `02001954-1A2C` returns the scheduler
  active-run byte and fiber ownership kind separately. Wrapper
  `0200544C-54C0` tests **fiber+0x10==1**, not scheduler+0x40. Kind1 wakes the
  control EA in the low word of fiber+0x18 and does not write the optional
  output; ordinary output receives the scheduler's active-run byte.
- RunFibers admission checks old worker count **>32**, not >=32. If the old
  count is32 and all32 bitmap bits are occupied, HLE retains the attempted
  count33 then stops with `fiber-unsupported field=worker-slot
  reason=worker-capacity` instead of pretending that a 33rd event bit exists.
- **Scheduler attribute reads (`0x02001CA4-0x02002000`):** firmware reads only
  `+0x10` (stored at `scheduler+0x148`), `+0x11` (registers scheduler in internal
  debugger table; slot address stored at `scheduler+0x168`), and `+0x14`
  (interval in usec converted to timebase ticks and stored as BE64 at
  `scheduler+0x138`, zeroing `scheduler+0x140`). Unread bytes are ignored.

`tests/fiber_switch` retains the scheduler regression and adds generated HLE
imports with the real PPU indirect dispatcher: wrapped initializer results,
attributes/error precedence, worker lifecycle, shared signals, nested
dispatch state, resource/optional-field stops and guest bounds. These are
synthetic toolkit fixtures, not firmware execution or title acceptance.

---

## Synchronization

### cellSync

**File:** `libs/sync/cellSync.c`
**Purpose:** Low-level atomic synchronization primitives

All cellSync primitives use **busy-waiting (spinlocks)** — they spin in a tight loop checking an atomic variable. This matches the PS3's SPU-oriented design where SPUs don't have OS-level blocking.

**Primitives:**
- **Mutex**: Atomic spinlock, lock/trylock/unlock
- **Barrier**: Counter-based barrier (all threads must arrive before any proceed)
- **RWM (Read-Write-Modify)**: Read a shared value, modify it, write it back — all atomically
- **Queue (bounded)**: Fixed-size circular buffer with atomic push/pop
- **Lock-Free Queue**: CAS-based concurrent queue

### cellSync2

**File:** `libs/sync/cellSync2.c`
**Purpose:** Extended synchronization primitives; firmware semaphore surface

- **Semaphore**: `_cellSync2SemaphoreAttributeInitialize(attrEA,sdk)`,
  `cellSync2SemaphoreInitialize(semEA,bufferEA,initial,attrEA)`,
  `GetCount(semEA,outEA)`, and `Acquire` / `TryAcquire` / `Release`
  `(semEA,count,configEA)`. Null config selects PPU or current-fiber routing.
  All semaphore arguments are guest EAs. Attributes and objects are 128 bytes;
  objects require 128-byte alignment. No host pointers or OS objects overlay
  guest memory. Shared count/state and waiter FIFO updates use reservations.
- An insufficient single-waiter Acquire reserves the available prefix,
  clears the available count and records the remaining debt; Release pays
  debt before notifying. Multi-waiter blocking Acquire accepts count0/1,
  reserving released tokens before wake so GetCount never reports them as
  available. Signed initial range is `[-0x800000,0x7FFFFF]`.
- Default fiber waits yield through `cellFiberPpuWaitSignal`; notification
  shares the existing fiber signal registry. Nonfiber calls park on a host
  condition variable, using opaque integer wake identities in the guest FIFO
  rather than firmware's shared LV2 event-flag allocation. This is a host
  scheduling mechanism, not a different count or reservation contract.
- Non-null custom configs stop with `sync2-semaphore-unsupported` before
  successful-support side effects. Reinitializing a suspended semaphore and
  unmodelled queue/waiter or failed fiber notification paths also fail closed.
- **Mutex / Condition Variable / Queue** retain the unrelated legacy
  OS-backed implementations; their ABI/layout fidelity is not established.

**Step-2 RE correction:** pinned FW4.93 libsync2 SHA-256
`9ffffaefa71c86e21a37177a9b8fa0400899ebe376ead482a9af7afe72bfa7fd`.
`02005300-5324` clears the high available word and stores
`0x40000000 | (requested-available)` in the low word. Release
`02004EF8-4F64` / `02004FEC-4FF8` pays this debt before notification.
This corrects B5-SCOPE's earlier no-prefix-reservation interpretation;
it is firmware behavior, not a deviation.

`tests/sync2_semaphore` exercises generated imported calls, poison/canary
layouts and validation precedence, signed/count-N/overflow semantics,
multiple consumers, real default-fiber yield/wake and named config/range
stops. Fixtures contain synthetic guest data only.

---

## Hardware / Peripherals

### cellUsbd

**File:** `libs/hardware/cellUsbd.c`
**Purpose:** USB device driver framework (stub)

- Logical Device Driver (LDD) registration
- Device enumeration always returns empty list
- Pipe open/transfer operations return `CELL_USBD_ERROR_NO_DEVICE`

### cellCamera

**File:** `libs/hardware/cellCamera.c`
**Purpose:** PlayStation Eye camera (stub)

- Init/end lifecycle
- All device queries return `CELL_CAMERA_ERROR_DEVICE_NOT_FOUND`
- IsAttached returns false

### cellGem

**File:** `libs/hardware/cellGem.c`
**Purpose:** PlayStation Move motion controller (stub)

- Init/end lifecycle
- `cellGemGetInfo()` reports 0 connected controllers
- All controller queries return `CELL_GEM_ERROR_NOT_CONNECTED`

---

## Miscellaneous

### cellSaveData

**File:** `libs/misc/cellSaveData.c`
**Purpose:** Save data management (callback-driven)

PS3 save data uses a callback pattern:
1. Game calls `cellSaveDataAutoSave2()` or `cellSaveDataAutoLoad2()`
2. Runtime invokes game's callback with save directory info
3. Game's callback specifies which files to read/write
4. Runtime performs the file operations

**Features:**
- Directory enumeration (list save data directories)
- Simplified PARAM.SFO parsing (save title, subtitle, detail)
- File read/write within save directories
- Delete save data
- Auto-save and auto-load convenience functions
- Callback results as FW 4.93 maps them: `funcStat` 0 continues, 1 returns `CELL_OK` without a PARAM.SFO write or file loop, -1..-5 return `CELL_SAVEDATA_ERROR_CBRESULT` (-4 `NODATA` included; the firmware returns `NODATA` only when VSH reports it), anything else `PARAM`. `funcFile` ends on 1 or 2 and follows the RPCS3 oracle for the rest. `version > 1` and `errDialog > 2` are `PARAM`, checked before the NULL arguments. `statGet.fileList` points at the title's `setBuf->buf`, also when the list is empty.
- A title that needs a different answer for its own save flow (for example treating `NODATA` as success) carries it in its own port, never in the toolkit.

### cellGame

**File:** `libs/misc/cellGame.c`
**Purpose:** Game content management

- `cellGameBootCheck(&type, &dirName)` — check boot type (disc, HDD, etc.)
- `cellGameContentPermit(&contentInfo)` — get permission to access content
- `cellGameGetParamInt(id, &value)` — read PARAM.SFO integer parameters
- `cellGameGetParamString(id, buf, size)` — read PARAM.SFO string parameters
- Data directory management (create, get path)

### cellRtc

**File:** `libs/misc/cellRtc.c`
**Purpose:** Real-time clock

- PS3 epoch: **January 1, 2000** (not Unix epoch 1970)
- `cellRtcGetCurrentTick(&tick)` — current time as tick count (microseconds since epoch)
- `cellRtcGetCurrentClockLocalTime(&datetime)` — structured date/time
- `cellRtcTickAddYears/Months/Days/Hours/Minutes/Seconds/Microseconds()` — time arithmetic
- `cellRtcFormatRfc2822(buf, &tick, tz_minutes)` — RFC 2822 date string
- `cellRtcFormatRfc3339(buf, &tick, tz_minutes)` — RFC 3339/ISO 8601 date string
- `cellRtcGetDayOfWeek(year, month, day)` — Zeller's formula

### cellMsgDialog / cellOskDialog

**cellMsgDialog** (`libs/misc/cellMsgDialog.c`):
- Message dialog: prints the message to stdout
- Auto-responds `YES` / `OK` (configurable)
- Progress bar tracking with percentage
- Async pattern: open → poll → close

**cellOskDialog** (`libs/misc/cellOskDialog.c`):
- On-screen keyboard: UTF-16 support
- Configurable default response text
- Auto-completes with default text (no actual keyboard UI)
- Async pattern: open → poll → get result → close

### cellUserInfo

**File:** `libs/misc/cellUserInfo.c`
**Purpose:** PS3 user account management

- Default user: ID `00000001`, name `"User"`
- `cellUserInfoGetStat(id, &stat)` — get user info
- `cellUserInfoGetList(&list)` — list all users (returns just the default)
- `cellUserInfoSelectUser(&id)` — select user (returns default)

### cellSubdisplay

**File:** `libs/misc/cellSubdisplay.c`
**Purpose:** PS Vita Remote Play sub-display

Handles second-screen output for PS Vita Remote Play. In a recompiled environment, no Vita is ever connected.

**Features:**
- Init/end/start/stop lifecycle
- Video mode configuration (480p, 272p)
- `cellSubdisplayIsConnected()` — always returns 0 (not connected)
- `cellSubdisplayGetTouchData(&data)` — returns empty touch data (0 points)
- `cellSubdisplayGetRequiredMemory(&size)` — reports 1 MB requirement

### cellImeJp

**File:** `libs/misc/cellImeJp.c`
**Purpose:** Japanese Input Method Editor

Handles kana-to-kanji conversion for Japanese text input. In this stub, input passes through without conversion (raw Unicode characters in = same characters out).

**Features:**
- Up to 4 concurrent IME handles
- Input modes: Hiragana, Katakana, Half-width Katakana, Alphanumeric
- Character-by-character input with `cellImeJpAddChar(handle, ch)`
- Backspace and reset support
- Max 256 input characters per session
- `cellImeJpGetConvertedString()` returns raw input (passthrough — no kanji dictionary)

### cellVideoExport / cellMusicExport / cellPhotoExport / cellPhotoImport

Export/import utilities for saving game content to or loading content from the XMB (PS3's system menu).

| Module | File | What It Does |
|--------|------|-------------|
| **cellVideoExport** | `libs/misc/cellVideoExport.c` | Save captured video to XMB video column. Returns NOT_SUPPORTED with callback. |
| **cellMusicExport** | `libs/misc/cellMusicExport.c` | Save audio to XMB music column. Returns NOT_SUPPORTED with callback. |
| **cellPhotoExport** | `libs/misc/cellPhotoExport.c` | Save screenshots to XMB photo column. Returns NOT_SUPPORTED with callback. |
| **cellPhotoImport** | `libs/misc/cellPhotoImport.c` | Browse/import photos from XMB. Returns NOT_SUPPORTED with callback. |

All follow the same pattern:
- `Init()` / `End()` lifecycle with guard checks
- `Start(param, callback, userdata)` — immediately fires callback with NOT_SUPPORTED
- Progress queries return 0.0

### cellGameRecording

**File:** `libs/misc/cellGameRecording.c`
**Purpose:** In-game video recording

Captures framebuffer output for replay and sharing. In the stub, recording state is tracked but no actual video capture occurs.

**Features:**
- Init with quality/resolution/FPS configuration
- Start/stop/pause/resume with state tracking
- `cellGameRecordingIsRecording()` — returns current recording state
- `cellGameRecordingGetDuration(&duration)` — always returns 0.0 (no real recording)

### cellPrint

**File:** `libs/misc/cellPrint.c`
**Purpose:** USB printer support

Stub for PS3 printing to compatible USB printers. No printers are ever detected.

- `cellPrintInit()` / `cellPrintEnd()` — lifecycle
- `cellPrintGetPrinterCount(&count)` — always returns 0

### cellRemotePlay

**File:** `libs/misc/cellRemotePlay.c`
**Purpose:** Remote Play for PS Vita / PSP

Checks whether Remote Play streaming is available. Always reports unavailable in a recompiled environment.

- `cellRemotePlayInit()` / `cellRemotePlayEnd()` — lifecycle
- `cellRemotePlayIsAvailable()` — always returns 0
- `cellRemotePlayGetStatus(&status)` — reports disconnected (status = 0)

### Other Miscellaneous Modules

| Module | Purpose | Implementation |
|--------|---------|----------------|
| **cellScreenshot** | Screenshot capture | Enable/disable flag, parameter/overlay tracking |
| **cellBGDL** | Background downloads | Init/term, download list always empty |
| **cellOvis** | Overlay system | Init/term, create/destroy (no-op) |
| **cellSheap** | Shared heap allocator | Bump allocator with block tracking, up to 8 heaps |
| **cellLicenseArea** | License region check | Americas default, all regions valid |
| **cellMusicDecode/2** | Music decoding | Init/finish, decode returns NOT_SUPPORTED |
| **cellVideoUpload** | Video upload | Init/term, upload returns NOT_SUPPORTED |
| **cellKey2char** | Keyboard scancode→Unicode | US-101 layout table, shift/caps handling |
| **cellAvconfExt** | AV configuration | Audio output device info, gamma control |
| **cellGameExec** | Game execution | Exit parameters, boot info, ExitToShelf |
