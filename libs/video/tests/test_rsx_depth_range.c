/*
 * The guest depth range (SET_CLIP_MIN / SET_CLIP_MAX) reaches the draw that uses it.
 *
 * libgcm's cellGcmSetViewport writes both registers in the viewport packet (docs/rsxfifo.txt
 * "SetClipMinMax", "SetViewport"), so they are part of every draw's state, not an unrelated clip plane:
 * window z is mapped to [near, far] before the depth test and the depth write. The test drives the real
 * register-file engine with a recording backend and checks what the backend is told, per draw:
 *
 *   1. at reset the range is [0, 1], the value the renderer hard-coded before S1;
 *   2. 0x0394/0x0398 reach rsx_dsp_depth_range and the engine's set_depth_range hook, per draw;
 *   3. an INVERTED range (near > far) is passed through, not clamped or swapped: the guest may ask
 *      for it (a reversed depth buffer), and a renderer that silently normalises it changes the test.
 *
 * Host only, no GPU: the backend is the recording stub. The real Metal readback (0.25/0.75 ->
 * znear/zfar) is test_depth_range_metal.m, labelled gpu.
 *
 * cc -std=gnu17 ... (CMake target test_rsx_depth_range, labels rsx;t0)
 */
#define main engine_probe_main
#include "test_rsx_draw_engine.c"
#undef main
#include "rsx_draw_engine.c"
#include "rsx_dispatch.c"
#include <math.h>
#include <string.h>

static unsigned checks, failures;
#define T(x, label) do { checks++; if (!(x)) { failures++; fprintf(stderr, "FAIL %s\n", label); } } while (0)

int main(void)
{
    vm_base = calloc(1, GUEST_BYTES);
    ppu_vm_size = GUEST_BYTES;

    /* 1. Register reset: the depth range is [0, 1] before the guest writes anything. */
    {
        static rsx_dispatch rsx;
        rsx_dispatch_init(&rsx, NULL);
        float n = -1.0f, f = -1.0f;
        rsx_dsp_depth_range(&rsx, &n, &f);
        T(n == 0.0f && f == 1.0f, "a reset register file has the [0, 1] depth range");
        T(rsx_dsp_method_class(0x0394u) != RSX_DSP_CLASS_TODO &&
          rsx_dsp_method_class(0x0398u) != RSX_DSP_CLASS_TODO,
          "CLIP_MIN/MAX are classified as consumed by the register-file dispatch");
    }

    /* 2. Through the engine, per draw. */
    engine_up();
    draw_triangle();
    T(stub.n_depth_range == 1 && stub.depth_range[0] == 0.0f && stub.depth_range[1] == 1.0f,
      "the draw is told [0, 1] by default");
    m(0x0394u, 0x3E800000u);            /* 0.25f */
    m(0x0398u, 0x3F400000u);            /* 0.75f */
    draw_triangle();
    T(stub.n_depth_range == 2 && fabsf(stub.depth_range[0] - 0.25f) < 1e-6f &&
      fabsf(stub.depth_range[1] - 0.75f) < 1e-6f,
      "the engine hands the guest's 0.25/0.75 to the backend, per draw");

    /* 3. Inverted: near > far is the guest's request, not an error. */
    m(0x0394u, 0x3F800000u);            /* 1.0f */
    m(0x0398u, 0x00000000u);            /* 0.0f */
    draw_triangle();
    T(stub.n_depth_range == 3 && stub.depth_range[0] == 1.0f && stub.depth_range[1] == 0.0f,
      "an inverted range is passed through unchanged");
    engine_down();

    printf("depth range checks=%u failures=%u\n", checks, failures);
    free(vm_base);
    return failures ? 1 : 0;
}
