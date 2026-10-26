/*
 * ps3recomp - RSX capture (ps3diff.rsx/1), the producer side
 *
 * A capture is the command stream the RSX consumed and every guest-memory
 * input it consumed with it, recorded at the moment of consumption, so that
 * rsx_replay can feed the same walker, draw engine and Metal backend again
 * without the title. Format and rules: docs/PS3DIFF.md, "ps3diff.rsx/1".
 *
 * The walker (cellGcmSys.c) and the draw engine (rsx_draw_engine.c) never
 * call into the capture module directly. Each owns one `const rsx_capture_sink*`
 * that is NULL until rsx_capture_install() registers the module, so a run with
 * capture off pays one pointer test per hook and every translation unit that
 * includes the walker or the engine still links without rsx_capture.cpp.
 *
 * Hooks are called on the FIFO walker thread, under the walker's lock. The
 * module is not meant to be driven from any other thread.
 */
#ifndef PS3RECOMP_RSX_CAPTURE_H
#define PS3RECOMP_RSX_CAPTURE_H

#include "ps3emu/ps3types.h"

#ifdef __cplusplus
extern "C" {
#endif

struct rsx_dispatch;

/* Where a consumed span lives. The numbers are rsx_dsp's RSX_LOCATION_*. */
#define RSX_CAP_LOC_LOCAL 0u
#define RSX_CAP_LOC_MAIN  1u

/* What a read was for. Informational: it is written as the blob's `kind` and
 * never changes how a replay treats the bytes. */
typedef enum {
    RSX_CAP_READ_OTHER = 0,
    RSX_CAP_READ_FIFO,        /* command words (the capture records these itself) */
    RSX_CAP_READ_LABEL,       /* a NV406E semaphore acquire's label */
    RSX_CAP_READ_VERTEX,      /* a vertex attribute span */
    RSX_CAP_READ_INDEX,       /* an index run */
    RSX_CAP_READ_TEXTURE,     /* a texture level or face */
    RSX_CAP_READ_PROGRAM,     /* a fragment program */
    RSX_CAP_READ_DMA_SOURCE,  /* a 2D transfer's source (NV0039, NV3089) */
    RSX_CAP_READ_KIND_COUNT
} rsx_cap_read_kind;

/* What the RSX wrote into guest memory. The capture keeps the bytes it left
 * there so a replay can check its own effect against them. */
typedef enum {
    RSX_CAP_WRITE_OTHER = 0,
    RSX_CAP_WRITE_LABEL,      /* a NV406E semaphore release */
    RSX_CAP_WRITE_REPORT,     /* a GET_REPORT result */
    RSX_CAP_WRITE_DMA,        /* a 2D transfer's destination */
    RSX_CAP_WRITE_KIND_COUNT
} rsx_cap_write_kind;

typedef enum {
    RSX_CAP_STEP_METHOD = 0,  /* a method packet (type 0 or 2), count 0 included */
    RSX_CAP_STEP_JUMP,
    RSX_CAP_STEP_CALL,
    RSX_CAP_STEP_RET,
    RSX_CAP_STEP_WORD         /* a word the walker consumes as one: a flip marker, an unknown word */
} rsx_cap_step_kind;

/* One step of the FIFO walker, as it decided it: the executed order of the
 * stream is the order of these calls, never the order of the ring. */
typedef struct rsx_cap_step {
    u32 kind;                 /* rsx_cap_step_kind */
    u32 get;                  /* IO offset the step starts at */
    u32 put;                  /* PUT as this walker pass saw it */
    u32 call_ret;             /* the CALL return slot before the step, 0 when empty */
    u32 subch;                /* method steps */
    u32 incrementing;         /* method steps: 1 for type 0, 0 for type 2 */
    u32 count;                /* method steps: data words */
    u32 target;               /* JUMP and CALL: the target; RET: where it returns */
    const u32* words;         /* the words the walker read, host order: header, then data */
    u32 nwords;
    u32 (*io2ea)(u32 io);     /* the walker's IO offset -> EA map, 0 when unmapped */
} rsx_cap_step;

/* Why a frame cannot be a baseline. The capture keeps a count per reason for
 * the frame in progress and writes them into its frame-complete record. */
typedef enum {
    RSX_CAP_NOTE_RESYNC = 0,       /* a walker recovery path dropped commands */
    RSX_CAP_NOTE_UNKNOWN_METHOD,   /* a method no class handles (a = subch, b = method, c = data) */
    RSX_CAP_NOTE_COUNT
} rsx_cap_note_kind;

typedef struct rsx_cap_frame {
    u32 flip_id;              /* the contract triple: flip_id, guest_frame, display_buffer */
    u32 guest_frame;
    u32 display_buffer;
    u32 present_seq;          /* the engine's own present ordinal, an extra field */
    const struct rsx_dispatch* rsx;
    /* The presented surface, read back after the command buffer completed.
     * `readback` is 0 on success; otherwise the reason it is missing. */
    u32 readback;             /* 0 ok, 1 unsupported format, 2 backend cannot, 3 not completed */
    u32 surface_format;       /* rsx_be_format of the presented surface (for the error text) */
    const char* format;       /* "rgba8-unorm", "rgba16-float", "rgba32-float" */
    const char* channel_order;
    const char* colorspace;   /* "linear" or "srgb": the pixel format's transfer function */
    u32 width, height, row_pitch;
    const u8* bytes;          /* row_pitch * height bytes, unmodified */
} rsx_cap_frame;

typedef struct rsx_capture_sink {
    /* ---- GCM state the replay has to rebuild ----------------------------- */
    /* cellGcmSetupContext, after its checks: the clean reset a capture starts from. */
    void (*gcm_init)(u32 cmd_size, u32 io_size, u32 io_ea);
    /* The IO offset table changed: `mapped` 1 = ea..ea+size now answers io, 0 = it no longer does. */
    void (*io_map)(u32 mapped, u32 ea, u32 io, u32 size);
    void (*display_buffer)(u32 id, u32 offset, u32 pitch, u32 width, u32 height);

    /* ---- the FIFO walker ------------------------------------------------- */
    /* begin opens the event for one step; every read and write until step_end
     * belongs to it. step_end writes it out. */
    void (*step_begin)(const rsx_cap_step* step);
    void (*step_end)(void);
    /* The step did not complete (a semaphore acquire still waiting: the walker
     * reads the packet again next pass). Everything since step_begin is dropped. */
    void (*step_cancel)(void);
    /* A span the RSX is about to consume. Returns an immutable copy that the
     * caller must consume instead of `bytes`, valid until step_end. The span
     * has already been range-checked by the caller. */
    const u8* (*read)(u32 location, u32 offset, u32 ea, const u8* bytes, u32 size, u32 kind);
    /* A span the RSX just wrote: `bytes` is what is in guest memory now. */
    void (*write)(u32 location, u32 offset, u32 ea, const u8* bytes, u32 size, u32 kind);
    void (*note)(u32 what, u32 a, u32 b, u32 c);
    /* End the process with the named rsx-capture stop. Never returns. The
     * walker raises a capture's own failures through this, so that it carries
     * no reference to the stop machinery that a capture-less link lacks. */
    void (*fail)(const char* reason, const char* detail);

    /* ---- the draw engine ------------------------------------------------- */
    /* One draw group left sink_end: executed, or dropped for `reason`
     * (rsx_drop_reason, named `reason_name`). `fixed` is 1 when it ran the
     * built-in pipeline. */
    void (*draw)(const struct rsx_dispatch* rsx, u32 executed, u32 reason,
                 const char* reason_name, u32 fixed);
    /* A present that completed a new flip head. */
    void (*frame)(const rsx_cap_frame* frame);
} rsx_capture_sink;

typedef struct rsx_capture_environment {
    const char* gpu;
    const char* macos;
    const char* metal;
    const char* build;
    const char* glslang;
    const char* spirv_cross;
    const char* toolkit_sha;
    const char* title_sha;
} rsx_capture_environment;

typedef struct rsx_capture_options {
    const char* dir;          /* must not already hold a capture */
    u32 max_frames;           /* close after this many frames; 0 = at exit */
} rsx_capture_options;

/* Register the module with the walker and the engine and open the capture.
 * Returns 1 when capture is on. Stops (rsx-capture) when the output cannot be used. */
int  rsx_capture_install(const rsx_capture_environment* env, const rsx_capture_options* opt);

/* The same, driven by video.rsx_capture / rsx_capture_dir / rsx_capture_frames:
 * a no-op returning 0 while video.rsx_capture is off. */
int  rsx_capture_install_from_config(const rsx_capture_environment* env);

/* The environment record the open capture carries, defaults filled in (build type, toolkit sha, "n/a"),
 * or NULL while none is open. rsx_replay compares it with the capture it replays. */
const rsx_capture_environment* rsx_capture_environment_record(void);

/* Write capture-end and publish capture.ndjson. Idempotent; also runs at normal
 * process exit once a capture is open. */
void rsx_capture_finish(void);

/* Set by rsx_capture_install; the walker and the engine read these. */
void cellGcm_set_capture_sink(const rsx_capture_sink* sink);
void rsx_draw_engine_set_capture_sink(const rsx_capture_sink* sink);

/* The most recent tool's view of the toolkit build, for the environment record:
 * "git short sha" plus "-dirty", fixed when CMake configured. */
const char* rsx_capture_toolkit_sha(void);

#ifdef __cplusplus
}
#endif
#endif /* PS3RECOMP_RSX_CAPTURE_H */
