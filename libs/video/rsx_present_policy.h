#ifndef PS3RECOMP_RSX_PRESENT_POLICY_H
#define PS3RECOMP_RSX_PRESENT_POLICY_H

#include <stdint.h>

static inline int rsx_present_flip_already_rendered(
    unsigned clear_present_count,
    unsigned last_clear_presented_flip,
    unsigned current_flip)
{
    return clear_present_count != 0 &&
           last_clear_presented_flip == current_flip;
}

/* ---- Windowed display: is a drawable needed? (#94) -------------------------------------------------------------
 *
 * Every [CAMetalLayer nextDrawable] waits for a display refresh. The engine presents once at a flip head with new
 * content and again from the host's frame clock with nothing new, so a guest flip took two acquisitions (three before
 * #101), and the guest ran at refresh divided by that count: 30 fps at 60 Hz, measured. A present that would put the
 * picture already on screen back on screen is pure cost, so it is skipped: no drawable, no blit, no presentDrawable.
 * Everything the guest can see (present identity, present_seq, dumps, capture, flip events) stays with the caller.
 *
 * The compositor keeps the last presented image on the layer, which is why "same surface, no new records, same layer
 * geometry, and that image did reach the screen" is enough. Each condition guards a way the screen can differ:
 *   - another surface is a different picture;
 *   - a record since the last blit changed a surface the blit reads (a flush commit encodes records too, so the count
 *     survives it; see rsx_display_note_encoded);
 *   - the layer's size or content scale changed, so the stored image no longer matches the layer;
 *   - the video-out gamma the blit applies changed: the same surface now blits to different pixels;
 *   - the last present dropped its image (nil drawable) or its blit did not encode, so what is on screen is not the
 *     last picture and the surface must be shown again.
 * The ObjC side gathers the plain values; nothing here touches Metal. */
typedef struct rsx_display_view {
    uint32_t width, height;     /* layer drawable size, pixels */
    uint32_t scale_milli;       /* layer contentsScale * 1000 */
    uint32_t gamma_bits;        /* IEEE-754 bits of the gamma the blit applies */
} rsx_display_view;

typedef struct rsx_display_state {
    uint32_t         recs_since_blit;   /* records encoded since the last blit that reached the screen */
    int              reached;           /* the last present obtained a drawable and encoded its blit */
    rsx_display_view view;              /* the layer geometry and gamma at that blit */
} rsx_display_state;

static inline void rsx_display_reset(rsx_display_state* s)
{
    s->recs_since_blit = 0;
    s->reached = 0;
    s->view.width = s->view.height = s->view.scale_milli = s->view.gamma_bits = 0;
}

/* Records that would change the picture if shown: the encoded ones plus the `open` ones still recorded. */
static inline uint32_t rsx_display_pending(const rsx_display_state* s, uint32_t open)
{
    const uint64_t n = (uint64_t)s->recs_since_blit + open;
    return n > UINT32_MAX ? UINT32_MAX : (uint32_t)n;
}

/* `same_surface`: the surface to present is the one the last blit showed. Nothing shown yet is "not the same". */
static inline int rsx_display_can_skip(const rsx_display_state* s, int same_surface, uint32_t open_records,
                                       const rsx_display_view* now)
{
    return same_surface && s->reached && rsx_display_pending(s, open_records) == 0 &&
           s->view.width == now->width && s->view.height == now->height &&
           s->view.scale_milli == now->scale_milli && s->view.gamma_bits == now->gamma_bits;
}

/* A commit encoded `records` records. A skipped present encodes none and calls nothing: it must not clear the count. */
static inline void rsx_display_note_encoded(rsx_display_state* s, uint32_t records)
{
    const uint64_t n = (uint64_t)s->recs_since_blit + records;
    s->recs_since_blit = n > UINT32_MAX ? UINT32_MAX : (uint32_t)n;
}

/* The blit to a drawable was encoded and presented under `now`. */
static inline void rsx_display_note_shown(rsx_display_state* s, const rsx_display_view* now)
{
    s->recs_since_blit = 0;
    s->reached = 1;
    s->view = *now;
}

/* The present did not reach the screen (nil drawable, or the blit bailed out): the next one must show a surface. */
static inline void rsx_display_note_missed(rsx_display_state* s)
{
    s->reached = 0;
}

#endif
