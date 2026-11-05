/* spu_shufb against the ISA's selector rules, in every build: the pshufb / NEON tbl path
 * (-msse4.1 on x86, always on arm64) and the byte loop that SPU_SCALAR_HELPERS selects must
 * both match a model written here from the ISA text. The model is independent of
 * spu_shufb_ref, so a scalar build compares the helper with something other than itself;
 * spu_shufb_ref is checked against it too. */
#include "../spu_helpers.h"
#include <stdio.h>
#include <stdlib.h>

/* ISA: control byte 10xxxxxx -> 0x00, 110xxxxx -> 0xFF, 111xxxxx -> 0x80, otherwise byte
 * (sel & 0x1F) of the 32-byte concatenation a:b. All byte numbers are SPU (big-endian) byte
 * positions, which u128 keeps at _u8[SPU_W(p)]. */
static uint8_t model_byte(const uint8_t cat[32], uint8_t sel)
{
    if ((sel & 0xE0) == 0xE0) return 0x80;
    if ((sel & 0xC0) == 0xC0) return 0xFF;
    if ((sel & 0xC0) == 0x80) return 0x00;
    return cat[sel & 0x1F];
}

static u128 model_shufb(u128 a, u128 b, u128 c)
{
    uint8_t cat[32];
    for (int j = 0; j < 16; j++) { cat[j] = a._u8[SPU_W(j)]; cat[16 + j] = b._u8[SPU_W(j)]; }
    u128 r;
    for (int t = 0; t < 16; t++) r._u8[SPU_W(t)] = model_byte(cat, c._u8[SPU_W(t)]);
    return r;
}

static int check(u128 a, u128 b, u128 c, const char* what)
{
    const u128 want = model_shufb(a, b, c), got = spu_shufb(a, b, c), ref = spu_shufb_ref(a, b, c);
    if (memcmp(&got, &want, 16)) { printf("test_spu_shufb: spu_shufb differs from the ISA model (%s)\n", what); return 1; }
    if (memcmp(&ref, &want, 16)) { printf("test_spu_shufb: spu_shufb_ref differs from the ISA model (%s)\n", what); return 1; }
    return 0;
}

int main(void)
{
    /* Every control value in every result position, over sources whose bytes are all distinct
     * and none of them 0x00/0x80/0xFF, so a wrong source byte or a wrong special cannot hide. */
    u128 a, b;
    for (int p = 0; p < 16; p++) { a._u8[SPU_W(p)] = (uint8_t)(0x10 + p); b._u8[SPU_W(p)] = (uint8_t)(0x30 + p); }
    for (int sel = 0; sel < 256; sel++)
        for (int pos = 0; pos < 16; pos++) {
            u128 c;
            for (int p = 0; p < 16; p++) c._u8[SPU_W(p)] = (uint8_t)(p == pos ? sel : 0x00);
            if (check(a, b, c, "single selector")) return 1;
        }

    srand(12345);
    for (int it = 0; it < 200000; it++) {
        u128 x, y, z;
        for (int i = 0; i < 16; i++) { x._u8[i] = (uint8_t)rand(); y._u8[i] = (uint8_t)rand(); z._u8[i] = (uint8_t)rand(); }
        if (check(x, y, z, "random")) { printf("  at iteration %d\n", it); return 1; }
    }
    puts("test_spu_shufb: ok (256 selectors x 16 positions, 200000 random cases)");
    return 0;
}
