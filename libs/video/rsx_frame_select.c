/* ps3recomp - frame dump selection and naming (rsx_frame_select.h). */
#include "rsx_frame_select.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One unsigned number at *p: decimal, or hex with a 0x prefix (base 0 would read 0120 as octal 80). strtoull
 * skips leading space, takes a sign, and wraps nothing at 32 bits, so the first digit and the range are
 * checked here. */
static int parse_u32(const char** p, uint32_t* v)
{
    const char* s = *p;
    const int hex = s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
    const char* digits = hex ? s + 2 : s;
    if (!(hex ? isxdigit((unsigned char)*digits) : (*digits >= '0' && *digits <= '9'))) return 0;
    char* end = NULL;
    errno = 0;
    const unsigned long long x = strtoull(digits, &end, hex ? 16 : 10);
    if (errno == ERANGE || x > 0xFFFFFFFFull) return 0;
    *v = (uint32_t)x;
    *p = end;
    return 1;
}

static const char* skip_blank(const char* p)
{
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

static int parse_range(rsx_frame_select* out, const char* r)
{
    const char* p = skip_blank(r);
    uint32_t a, b;
    if (!parse_u32(&p, &a)) return 0;
    p = skip_blank(p);
    if (*p != '-' && *p != ':') return 0;
    p = skip_blank(p + 1);
    if (!parse_u32(&p, &b)) return 0;
    if (*skip_blank(p) || b < a) return 0;
    out->have_range = 1;
    out->range_lo = a;
    out->range_hi = b;
    return 1;
}

static rsx_frame_select_status parse_list(rsx_frame_select* out, const char* l, unsigned* count)
{
    const char* p = skip_blank(l);
    unsigned total = 0;
    for (;;) {
        uint32_t v;
        if (!parse_u32(&p, &v)) return RSX_FRAME_SELECT_BAD_LIST;
        if (total < RSX_FRAME_SELECT_LIST_MAX) out->list[out->n_list++] = v;
        total++;
        p = skip_blank(p);
        if (!*p) break;
        if (*p != ',') return RSX_FRAME_SELECT_BAD_LIST;
        p = skip_blank(p + 1);
    }
    if (total > RSX_FRAME_SELECT_LIST_MAX) {
        if (count) *count = total;
        return RSX_FRAME_SELECT_LIST_TOO_LONG;
    }
    out->have_list = 1;
    return RSX_FRAME_SELECT_OK;
}

rsx_frame_select_status rsx_frame_select_parse(rsx_frame_select* out, const char* range, const char* list,
                                               unsigned* count)
{
    memset(out, 0, sizeof *out);
    if (range && *range && !parse_range(out, range)) {
        memset(out, 0, sizeof *out);
        return RSX_FRAME_SELECT_BAD_RANGE;
    }
    if (list && *list) {
        const rsx_frame_select_status st = parse_list(out, list, count);
        if (st != RSX_FRAME_SELECT_OK) {
            memset(out, 0, sizeof *out);
            return st;
        }
    }
    return RSX_FRAME_SELECT_OK;
}

int rsx_frame_select_wanted(const rsx_frame_select* s, uint32_t seq)
{
    if (s->have_list) {
        for (unsigned i = 0; i < s->n_list; i++)
            if (s->list[i] == seq) return 1;
        return 0;
    }
    if (s->have_range) return seq >= s->range_lo && seq <= s->range_hi;
    return 0;
}

size_t rsx_frame_dump_name(char* out, size_t n, const char* base, uint32_t present_seq, uint32_t flip_id,
                           uint32_t no_flip)
{
    char flip[16];
    if (flip_id == no_flip) snprintf(flip, sizeof flip, "none");
    else snprintf(flip, sizeof flip, "%05u", (unsigned)flip_id);
    size_t len = strlen(base);
    if (len > 4 && strcmp(base + len - 4, ".ppm") == 0) len -= 4;
    const int w = snprintf(out, n, "%.*s_s%05u_f%s.ppm", (int)len, base, (unsigned)present_seq, flip);
    return w < 0 ? 0 : (size_t)w;
}
