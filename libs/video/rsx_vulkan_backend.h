/*
 * ps3recomp - Vulkan RSX Backend
 *
 * The Linux renderer the README calls "the obvious target". Same host-facing
 * entry points and test hooks as the Metal and headless null backends, so
 * runtime/host/host_posix.c drives it unchanged.
 *
 * Two draw paths, one registered at a time (the FIFO walker feeds both):
 *
 *   - the shared register-file draw engine (rsx_draw_engine.h), the path the
 *     Metal backend takes. The engine owns surfaces, render-to-texture, MRT,
 *     depth textures, vertex compaction and pipeline keys; this backend builds
 *     Vulkan pipelines from the guest's programs (HLSL -> SPIR-V through
 *     glslang, rsx_shader_spirv.h) and executes. Every ps3recomp_host scene
 *     passes on it. Default when guest programs are on.
 *   - the rsx_state vtable: the headless null backend's fixed-function
 *     contract (flat colour, depth test, texture unit 0, point sampling), so
 *     the two agree pixel for pixel. Default otherwise; with guest programs it
 *     also runs them, but has no surfaces (--rtt, --depthtex, --mrt, --mrt-a).
 *
 * Rendering is offscreen; a present reads the frame back (the host's pixel
 * checks), optionally dumps it, and optionally shows it in a window.
 *
 * Vulkan is loaded at run time (dlopen of libvulkan.so.1), not linked, so:
 *   - building needs only the Vulkan headers, not a target-arch libvulkan
 *     (which is what makes cross-compiling for aarch64 straightforward);
 *   - a machine with no Vulkan loader fails init() cleanly instead of failing
 *     to start at all.
 * Only Vulkan 1.0 core entry points are used, so it runs on 1.2-level drivers
 * such as NVIDIA's L4T driver for Tegra X1.
 *
 * Environment:
 *   PS3RECOMP_VK_DEVICE=<n>   pick physical device n (vkEnumeratePhysicalDevices
 *                             order) instead of the first non-CPU device.
 *   PS3RECOMP_VK_DUMP=<path>  write presented frames to <path> as a binary
 *                             PPM: with PS3RECOMP_VK_DUMP_EVERY=<n> every nth
 *                             present (default every one), one file overwritten
 *                             each time.
 *   PS3RECOMP_VK_DUMP_LIST=<a,b,c> | PS3RECOMP_VK_DUMP_RANGE=<first>-<last>
 *                             instead dump exactly those presents (at most 32
 *                             in a list; a list wins over a range), each to
 *                             <path>_s<present_seq>_f<flip_id>.ppm as the Metal
 *                             backend names its PS3RECOMP_METAL_FRAME_DUMP
 *                             files (rsx_frame_select.h). A malformed value is
 *                             a config-invalid stop at init.
 *   PS3RECOMP_VK_WINDOW=1     also show each presented frame in an SDL2
 *                             window. Rendering and readback are unchanged, so
 *                             every test gives the same result with or without
 *                             it; if the window cannot be opened the run
 *                             continues headless and says why.
 *   PS3RECOMP_VK_FULLSCREEN=1 with a window: borderless full screen.
 *   PS3RECOMP_VK_HOLD=<sec>   with a window: keep the last frame on screen
 *                             this long before shutdown closes it.
 *   PS3RECOMP_VK_GUEST_PROGRAMS=1
 *                             run the guest's own vertex/fragment programs
 *                             (needs the translator: glslang at build time),
 *                             which also makes the draw engine the default.
 *   PS3RECOMP_RSX_ENGINE=dispatch|vtable
 *                             pick the path explicitly (the engine's own
 *                             switch, shared with the Metal backend).
 */

#ifndef PS3RECOMP_RSX_VULKAN_BACKEND_H
#define PS3RECOMP_RSX_VULKAN_BACKEND_H

#include "rsx_commands.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Load Vulkan, create the device and the offscreen target, and register as
 * the RSX backend. `title` is only logged (no window at V0).
 * Returns 0 on success, -1 on any failure (reason printed to stderr). */
int  rsx_vulkan_backend_init(u32 width, u32 height, const char* title);

/* Unregister and destroy every Vulkan object, then unload the library. */
void rsx_vulkan_backend_shutdown(void);

/* Headless: always 0. Windowed: drains SDL's event queue; -1 once the
 * window has been closed or Esc pressed. */
int  rsx_vulkan_backend_pump_messages(void);

/* Read the frame back to host memory (and to PS3RECOMP_VK_DUMP if set), and
 * show it in the window if there is one. Same as _present_begin() followed
 * by _present_finish(). */
void rsx_vulkan_backend_present(void);

/* The same present in two halves, for a host that presents on a thread of its
 * own beside the FIFO walker.
 *
 * _present_begin() reads the flipped surface: it must not run while the
 * walker is draining (call it under the same lock as
 * cellGcm_rsx_process_fifo). On the draw engine path it only records and
 * submits the copy out of that surface, and returns at once.
 *
 * _present_finish() waits for that copy, reads the frame back, writes the
 * dump and presents to the window. It may run while the walker drains the
 * next frame: the next frame's commands are ordered after the copy, so they
 * never reach the presented image. On the fixed-function path, where the
 * walker draws into the presented image itself, _present_begin() does all of
 * it and _present_finish() has nothing left to do. */
void rsx_vulkan_backend_present_begin(void);
void rsx_vulkan_backend_present_finish(void);

/* Centre pixel of the last frame a present copied out, as 0xFFRRGGBB, or 0
 * before the first. Unlike _readback_center(), which on the draw engine path
 * reads the surface as it is now, this is what was shown. */
u32  rsx_vulkan_backend_presented_center(void);

/* The clear colour most recently set through the command stream, ARGB8888. */
u32  rsx_vulkan_backend_debug_color(void);

/* Centre pixel of the last presented frame as 0xFFRRGGBB, or 0 if nothing
 * has been presented yet. */
u32  rsx_vulkan_backend_readback_center(void);

/* Draws of the last presented frame that ran the guest's own programs. */
u32  rsx_vulkan_backend_guest_draws(void);

/* 1 when draws honour the depth bounds test: the draw engine drives the
 * backend and the device has the depthBounds feature (lavapipe in Mesa 25
 * does not). Valid after init. */
int  rsx_vulkan_backend_depth_bounds(void);

/* 1 when draws run the guest's own programs: the HLSL -> SPIR-V translator
 * was built (glslang found) and PS3RECOMP_VK_GUEST_PROGRAMS=1 opts in. Then
 * the shared register-file draw engine (rsx_draw_engine.h) is the default
 * path, as it is Metal's; PS3RECOMP_RSX_ENGINE=vtable selects the older
 * rsx_state path instead. Opt-in while the path grows; needs no initialised
 * backend. */
int  rsx_vulkan_backend_guest_programs(void);
/* What rsx_vulkan_backend_init settled on (rsx_replay refuses any other path). */
#define RSX_VULKAN_HEADLESS       1u   /* offscreen, no window */
#define RSX_VULKAN_GUEST_SHADERS  2u   /* guest programs translated and active */
#define RSX_VULKAN_ENGINE_PATH    4u   /* the register-file draw engine drives the backend */
u32  rsx_vulkan_backend_status(void);

/* Machine fields for ps3diff.rsx/2. build, toolkit_sha and title_sha are left NULL
 * for the capture writer; strings returned through env outlive this call. */
struct rsx_capture_environment;
int  rsx_vulkan_backend_capture_environment(struct rsx_capture_environment* env);

#if defined(PS3RECOMP_RSX_VULKAN_TEST_SEAMS)
/* Fail the next `count` register-file engine submissions. Test builds only. */
void rsx_vulkan_backend_test_fail_next_engine_submissions(u32 count);
#endif

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_RSX_VULKAN_BACKEND_H */
