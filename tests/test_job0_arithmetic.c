/* Independent integer reference for the experimental inlined arithmetic. */
#include "haze_job0_arithmetic.h"
#include "spu_float_native.h"
#include <xmmintrin.h>
#include <assert.h>
static uint32_t seed=0x8ac05731;
static uint32_t random_word(void) { seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed; }
static uint64_t checks;
__attribute__((target("avx512f"))) static void verify(u128 a,u128 b,u128 c)
{
    unsigned before=_mm_getcsr();
    u128 results[]={haze_job0_add(a,b),haze_job0_sub(a,b),haze_job0_mul(a,b),
                    haze_job0_madd(a,b,c),haze_job0_nms(a,b,c)};
    assert(before==_mm_getcsr());
    for(unsigned lane=0;lane<4;++lane) {
        uint32_t av=a._u32[lane],bv=b._u32[lane],cv=c._u32[lane];
        uint32_t expected[]={spu_float_add(av,bv),spu_float_add(av,bv^0x80000000u),
            spu_float_mul(av,bv),spu_float_madd(av,bv,cv),spu_float_madd(av^0x80000000u,bv,cv)};
        for(unsigned op=0;op<5;++op) {
            if(results[op]._u32[lane]!=expected[op]) {
                fprintf(stderr,"Inline arithmetic mismatch op=%u a=%08x b=%08x c=%08x\n",op,av,bv,cv);abort();
            }
            ++checks;
        }
    }
}
int main(void)
{
    if(!spu_float_native_available()) { puts("AVX512F unavailable: inline arithmetic test skipped");return 0; }
    const uint32_t edges[]={0,1,0x007fffff,0x00800000,0x00800001,0x3f7fffff,0x3f800000,
        0x3f800001,0x7effffff,0x7f000000,0x7f7fffff,0x7f800000,0x7fffffff,0x80000000,
        0x80000001,0xbf800000,0xff800000,0xffffffff};
    unsigned saved=_mm_getcsr();
    for(unsigned mode=0;mode<8;++mode) {
        _mm_setcsr((saved&~(0x6000u|0x8040u))|((mode&3)<<13)|(mode&4?0x8040u:0));
        for(unsigned i=0;i<18;++i) for(unsigned j=0;j<18;++j) for(unsigned k=0;k<18;++k) {
            u128 a,b,c;for(unsigned lane=0;lane<4;++lane) {
                a._u32[lane]=edges[(i+lane)%18];b._u32[lane]=edges[(j+lane)%18];c._u32[lane]=edges[(k+lane)%18];
            }
            verify(a,b,c);
        }
        for(unsigned n=0;n<10000;++n) {
            u128 a,b,c;for(unsigned lane=0;lane<4;++lane) {
                a._u32[lane]=random_word();b._u32[lane]=random_word();c._u32[lane]=random_word();
            }
            verify(a,b,c);
        }
    }
    _mm_setcsr(saved);printf("%llu inline arithmetic lane comparisons passed; MXCSR preserved\n",(unsigned long long)checks);
    return 0;
}
