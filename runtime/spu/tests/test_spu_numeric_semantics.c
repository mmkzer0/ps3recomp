/* clang -std=gnu17 -I runtime/spu -I include \
 * runtime/spu/tests/test_spu_numeric_semantics.c -lm -o /tmp/spu-numeric && /tmp/spu-numeric
 * Covers SPU operations used by Yakuza's geometry and audio programs. */
#include "spu_helpers.h"
#include <math.h>
#include <stdio.h>
static int passed, failed;
#define CHECK(c) do { if(c) ++passed; else { ++failed; printf("FAIL line %d: %s\n",__LINE__,#c); } } while(0)
int main(void)
{
    u128 a = spu_zero(), r;
    a._f32[0]=1.0f; a._f32[1]=-2.75f; a._f32[2]=42.5f; a._f32[3]=1024.0f;
    r=spu_cflts(a,173);
    CHECK(r._s32[0]==1 && r._s32[1]==-2 && r._s32[2]==42 && r._s32[3]==1024);
    r=spu_cfltu(a,173);
    CHECK(r._u32[0]==1 && r._u32[1]==0 && r._u32[2]==42 && r._u32[3]==1024);
    r=spu_cflts(a,172); CHECK(r._s32[0]==2 && r._s32[1]==-5);
    /* cflts/cfltu of an exponent-0 (zero/denormal) input: the significand is 0, but the
     * hardware still saturates on the shift amount sh = be + 23 - i8 (signed at sh >= 8,
     * unsigned at sh >= 9; sh == 8 does not saturate unsigned and a negative unsigned
     * input stays 0). Hardware, direct: hwprobe `spuexh` lanes r007/r008 on a CECHC04
     * with 4.81 REBUG, w32 block 0 -- every group dumps the same 1024 input words, so
     * these three exponent-0 lanes are measured at each I8 below; issue #126, fit
     * CONV_RULE.md. The I8 15/16 rows are the fit's prediction (no lanes taken there). */
    u128 q=spu_zero();
    q._u32[0]=0x00000000u;                         /* +0 */
    q._u32[1]=0x803EA729u;                         /* -denormal */
    q._u32[2]=0x80652112u;                         /* -denormal */
    for (int i8=0; i8<=3; ++i8) {
        r=spu_cflts(q,i8);                         /* sh = 20..23: measured saturating */
        CHECK(r._u32[0]==0x7FFFFFFFu && r._u32[1]==0x80000000u
              && r._u32[2]==0x80000000u && r._u32[3]==0x7FFFFFFFu);
        r=spu_cfltu(q,i8);
        CHECK(r._u32[0]==0xFFFFFFFFu && r._u32[1]==0 && r._u32[2]==0 && r._u32[3]==0xFFFFFFFFu);
    }
    r=spu_cflts(q,10);                             /* sh = 13: measured */
    CHECK(r._u32[0]==0x7FFFFFFFu && r._u32[1]==0x80000000u
          && r._u32[2]==0x80000000u && r._u32[3]==0x7FFFFFFFu);
    r=spu_cflts(q,15);                             /* sh = 8: the signed threshold */
    CHECK(r._u32[0]==0x7FFFFFFFu && r._u32[1]==0x80000000u
          && r._u32[2]==0x80000000u && r._u32[3]==0x7FFFFFFFu);
    r=spu_cfltu(q,15);                             /* sh = 8: unsigned does not saturate */
    CHECK(r._u32[0]==0 && r._u32[1]==0 && r._u32[2]==0 && r._u32[3]==0);
    r=spu_cflts(q,16);                             /* sh = 7: below both thresholds */
    CHECK(r._u32[0]==0 && r._u32[1]==0 && r._u32[2]==0 && r._u32[3]==0);
    r=spu_cfltu(q,16);
    CHECK(r._u32[0]==0 && r._u32[1]==0 && r._u32[2]==0 && r._u32[3]==0);
    /* A nonzero lane at I8 15 still converts: 1.0 * 2^158 saturates signed, and the
     * exponent-0 lanes saturate too (sh = 8 for every be at this scale). */
    a=spu_zero(); a._u32[0]=0x3F800000u; a._u32[1]=0x3F800000u;
    r=spu_cflts(a,15);
    CHECK(r._u32[0]==0x7FFFFFFFu && r._u32[1]==0x7FFFFFFFu
          && r._u32[2]==0x7FFFFFFFu && r._u32[3]==0x7FFFFFFFu);
    a=spu_splat_u32(8); r=spu_csflt(a,155); CHECK(r._f32[0]==8.0f);
    r=spu_cuflt(a,156); CHECK(r._f32[0]==16.0f);
    r=spu_csflt(a,154); CHECK(r._f32[0]==4.0f);
    a._u32[0]=0x12345678; a._u32[1]=0x80000000;
    a._u32[2]=0xffffffff; a._u32[3]=7;
    r=spu_xswd(a);
    CHECK(r._u32[0]==0xffffffff && r._u32[1]==0x80000000 && r._u32[2]==0 && r._u32[3]==7);
    a._f32[0]=1.0f; a._f32[1]=99.0f; a._f32[2]=2.0f; a._f32[3]=-99.0f;
    r=spu_fesd(a); CHECK(spu__dget(r,0)==1.0 && spu__dget(r,1)==2.0);
    r=spu_frds(r); CHECK(r._f32[0]==1.0f && r._u32[1]==0 && r._f32[2]==2.0f && r._u32[3]==0);
    /* A denormal single extends to a zero that keeps its sign (CBE hardware: -denormal -> -0.0). */
    a._u32[0]=0x80000001u; a._u32[2]=0x007FFFFFu;
    r=spu_fesd(a); CHECK(spu__dw_get(r,0)==0x8000000000000000ull && spu__dw_get(r,1)==0);
    /* The estimate/FI pair feeds one Newton refinement in the guest code. */
    const float values[]={1.0f,2.0f,5.3311235f,-76.91874f};
    u128 one=spu_zero(); for(int i=0;i<4;++i) one._f32[i]=1.0f;
    for(int i=0;i<4;++i) a._f32[i]=values[i];
    r=spu_fi(a,spu_frest(a)); r=spu_fma(spu_fnms(a,r,one),r,r);
    for(int i=0;i<4;++i) CHECK(fabsf(r._f32[i]*values[i]-1.0f)<2e-6f);
    for(int i=0;i<4;++i) a._f32[i]=fabsf(values[i]);
    u128 half=one; for(int i=0;i<4;++i) half._f32[i]=0.5f;
    r=spu_fi(a,spu_frsqest(a));
    r=spu_fma(spu_fnms(spu_fm(a,r),r,one),spu_fm(r,half),r);
    for(int i=0;i<4;++i) CHECK(fabsf(r._f32[i]*sqrtf(a._f32[i])-1.0f)<2e-6f);
    printf("SPU numeric semantics: %d passed, %d failed\n",passed,failed);
    return failed?1:0;
}
