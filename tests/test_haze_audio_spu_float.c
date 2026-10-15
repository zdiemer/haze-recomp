/* The SDK integer implementation is the independent arithmetic oracle. */
#include "spu_float_native.c"
#define HAZE_AUDIO_FLOAT_NO_REMAP
#include "../port/haze_audio_spu_float.h"
#include "spu_context.h"
#include <setjmp.h>
#include <xmmintrin.h>
#include <windows.h>
#include <assert.h>
#include <stdio.h>
static uint32_t seed = 0x631df028;
static uint32_t random_word(void) {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}
static unsigned long long checks;
static void verify(u128 a, u128 b, u128 c) {
    const unsigned before = _mm_getcsr();
    haze_audio_float_state scope_saved = haze_audio_enter(1);
    u128 actual[9] = {
        spu_float_native_add(a, b), spu_float_native_mul(a, b), spu_float_native_madd(a, b, c),
        haze_audio_fa(a, b),        haze_audio_fs(a, b),        haze_audio_fm(a, b),
        haze_audio_fma(a, b, c),    haze_audio_fms(a, b, c),    haze_audio_fnms(a, b, c)};
    haze_audio_leave(scope_saved);
    assert(_mm_getcsr() == before);
    for (unsigned i = 0; i < 4; ++i) {
        const uint32_t av = a._u32[i], bv = b._u32[i], cv = c._u32[i];
        uint32_t want[] = {spu_float_add(av, bv),
                           spu_float_mul(av, bv),
                           spu_float_madd(av, bv, cv),
                           spu_float_add(av, bv),
                           spu_float_add(av, bv ^ 0x80000000u),
                           spu_float_mul(av, bv),
                           spu_float_madd(av, bv, cv),
                           spu_float_madd(av, bv, cv ^ 0x80000000u),
                           spu_float_madd(av ^ 0x80000000u, bv, cv)};
        for (unsigned op = 0; op < 9; ++op) {
            if (want[op] != actual[op]._u32[i]) {
                fprintf(stderr,
                        "FAIL op=%u a=%08x b=%08x c=%08x expected=%08x actual=%08x mxcsr=%x\n", op,
                        a._u32[i], b._u32[i], c._u32[i], want[op], actual[op]._u32[i], before);
                abort();
            }
            ++checks;
        }
    }
}

static jmp_buf* halt_target;
int spu_run_with_halt(void (*entry)(spu_context*), spu_context* ctx);
/* Model the SDK halt landing pad to test the production outer wrapper. */
int haze_sdk_spu_run_with_halt(void (*entry)(spu_context*), spu_context* ctx) {
    jmp_buf target;
    jmp_buf* outer = halt_target;
    halt_target = &target;
    int halted = setjmp(target);
    if (!halted)
        entry(ctx);
    halt_target = outer;
    return halted;
}
static void halt_entry(spu_context* ctx) {
    (void)ctx;
    assert(haze_audio_scope);
    longjmp(*halt_target, 1);
}
static unsigned entry_csr;
static void other_entry(spu_context* ctx) {
    (void)ctx;
    assert(!haze_audio_scope && _mm_getcsr() == entry_csr);
}
static void audio_entry(spu_context* ctx) {
    assert(haze_audio_scope && (_mm_getcsr() & 0xffc0u) == 0xffc0u);
    unsigned active_csr = _mm_getcsr();
    spu_context nested = {0};
    nested.image_id = 4;
    assert(spu_run_with_halt(other_entry, &nested) == 0);
    assert(haze_audio_scope && _mm_getcsr() == active_csr);
    nested.image_id = 27;
    assert(spu_run_with_halt(halt_entry, &nested) == 1);
    assert(haze_audio_scope && _mm_getcsr() == active_csr);
    (void)ctx;
}
static DWORD WINAPI other_thread(void* unused) {
    (void)unused;
    assert(!haze_audio_scope);
    return 0;
}

static __declspec(noinline) u128 double_reference(unsigned op, u128 a, u128 b, u128 c) {
    switch (op) {
    case 0:
        return spu_dfa(a, b);
    case 1:
        return spu_dfs(a, b);
    case 2:
        return spu_dfm(a, b);
    case 3:
        return spu_dfma(a, b, c);
    case 4:
        return spu_dfms(a, b, c);
    case 5:
        return spu_dfnms(a, b, c);
    case 6:
        return spu_dfnma(a, b, c);
    case 7:
        return spu_dfceq(a, b);
    case 8:
        return spu_dfcmeq(a, b);
    case 9:
        return spu_dfcgt(a, b);
    case 10:
        return spu_dfcmgt(a, b);
    case 11:
        return spu_fesd(a);
    case 12:
        return spu_frds(a);
    default:
        abort();
    }
}
static u128 double_actual(unsigned op, u128 a, u128 b, u128 c) {
    switch (op) {
    case 0:
        return haze_audio_dfa(a, b);
    case 1:
        return haze_audio_dfs(a, b);
    case 2:
        return haze_audio_dfm(a, b);
    case 3:
        return haze_audio_dfma(a, b, c);
    case 4:
        return haze_audio_dfms(a, b, c);
    case 5:
        return haze_audio_dfnms(a, b, c);
    case 6:
        return haze_audio_dfnma(a, b, c);
    case 7:
        return haze_audio_dfceq(a, b);
    case 8:
        return haze_audio_dfcmeq(a, b);
    case 9:
        return haze_audio_dfcgt(a, b);
    case 10:
        return haze_audio_dfcmgt(a, b);
    case 11:
        return haze_audio_fesd(a);
    case 12:
        return haze_audio_frds(a);
    default:
        abort();
    }
}
static void verify_environment(void) {
    const unsigned saved = _mm_getcsr();
    const uint64_t edges[] = {0,
                              1,
                              0x000fffffffffffffull,
                              0x0010000000000000ull,
                              0x3ff0000000000000ull,
                              0x3ca0000000000000ull,
                              0x3ff0000010000000ull,
                              0x7fefffffffffffffull,
                              0x8000000000000000ull,
                              0xbff0000000000000ull};
    for (unsigned mode = 0; mode < 16; ++mode) {
        entry_csr = (saved & ~0xe07fu) | ((mode & 3) << 13) | ((mode & 4) ? 0x40u : 0) |
                    ((mode & 8) ? 0x8000u : 0) | 0x1f80u;
        _mm_setcsr(entry_csr);
        spu_context ctx = {0};
        ctx.image_id = 27;
        assert(spu_run_with_halt(audio_entry, &ctx) == 0);
        assert(!haze_audio_scope && _mm_getcsr() == entry_csr);
        assert(spu_run_with_halt(halt_entry, &ctx) == 1);
        assert(!haze_audio_scope && _mm_getcsr() == entry_csr);
        for (unsigned trial = 0; trial < 1024; ++trial) {
            u128 a, b, c;
            for (unsigned lane = 0; lane < 4; ++lane) {
                a._u32[lane] = random_word();
                b._u32[lane] = random_word();
                c._u32[lane] = random_word();
            }
            if (trial < 100) {
                for (unsigned lane = 0; lane < 2; ++lane) {
                    uint64_t av = edges[trial % 10], bv = edges[trial / 10];
                    a._u32[lane * 2] = (uint32_t)(av >> 32);
                    a._u32[lane * 2 + 1] = (uint32_t)av;
                    b._u32[lane * 2] = (uint32_t)(bv >> 32);
                    b._u32[lane * 2 + 1] = (uint32_t)bv;
                }
            }
            for (unsigned op = 0; op < 13; ++op) {
                _mm_setcsr(entry_csr);
                u128 expected = double_reference(op, a, b, c);
                _mm_setcsr(entry_csr);
                haze_audio_float_state scope = haze_audio_enter(1);
                unsigned active = _mm_getcsr();
                u128 actual = double_actual(op, a, b, c);
                assert(_mm_getcsr() == active);
                haze_audio_leave(scope);
                assert(_mm_getcsr() == entry_csr);
                if (memcmp(&actual, &expected, sizeof(actual))) {
                    fprintf(stderr, "Double environment mismatch: mode=%u op=%u trial=%u\n", mode,
                            op, trial);
                    abort();
                }
            }
        }
    }
    haze_audio_float_state scope = haze_audio_enter(1);
    HANDLE thread = CreateThread(NULL, 0, other_thread, NULL, 0, NULL);
    assert(thread && WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0);
    CloseHandle(thread);
    haze_audio_leave(scope);
    _putenv_s("SPU_FLOAT_NATIVE_VALIDATE", "1");
    scope = haze_audio_enter(1);
    assert(!haze_audio_scope);
    haze_audio_leave(scope);
    _putenv_s("SPU_FLOAT_NATIVE_VALIDATE", "");
    _mm_setcsr(saved);
    puts("Double/conversion semantics, nested jobs, guest halt, TLS isolation and validation "
         "bypass passed");
}

int main(void) {
    printf("AVX512F CPU+OS available=%d\n", spu_float_native_available());
    if (!spu_float_native_available()) {
        assert(!spu_float_native_set_mode(1) && !spu_float_native_active());
        puts("Native float: unsupported CPU/OS retains scalar fallback; hardware oracle skipped");
        return 0;
    }
    assert(spu_float_native_set_mode(1) && spu_float_native_active());
    verify_environment();
    const uint32_t edge[] = {0,          1,          0x007fffff, 0x00800000, 0x00800001,
                             0x3f7fffff, 0x3f800000, 0x3f800001, 0x7effffff, 0x7f000000,
                             0x7f7fffff, 0x7f800000, 0x7fffffff, 0x80000000, 0x80000001,
                             0xbf800000, 0xff800000, 0xffffffff};
    const unsigned saved = _mm_getcsr();
    for (unsigned mode = 0; mode < 8; ++mode) {
        _mm_setcsr((saved & ~(0x6000u | 0x8040u)) | ((mode & 3) << 13) | (mode & 4 ? 0x8040u : 0));
        for (unsigned i = 0; i < 18; ++i)
            for (unsigned j = 0; j < 18; ++j)
                for (unsigned k = 0; k < 18; ++k) {
                    u128 a, b, c;
                    for (unsigned l = 0; l < 4; ++l) {
                        a._u32[l] = edge[i];
                        b._u32[l] = edge[j];
                        c._u32[l] = edge[k];
                    }
                    verify(a, b, c);
                }
        for (unsigned i = 0; i < 250000; ++i) {
            u128 a, b, c;
            for (unsigned l = 0; l < 4; ++l) {
                a._u32[l] = random_word();
                b._u32[l] = random_word();
                c._u32[l] = random_word();
            }
            verify(a, b, c);
        }
    }
    assert(!spu_float_native_set_mode(0));
    haze_audio_float_state disabled = haze_audio_enter(1);
    assert(!haze_audio_scope);
    haze_audio_leave(disabled);
    u128 zero = {0};
    verify(zero, zero, zero);
    assert(spu_float_native_set_mode(1));
    _mm_setcsr(saved);
    printf("%llu independent hardware/reference lane comparisons passed\n", checks);
    // Deliberate corruption must publish the scalar result and latch failure.
#ifdef SPU_FLOAT_NATIVE_HOST
    validate = 1;
    u128 bad = {0};
    bad._u32[0] = 1;
    u128 corrected = validate_result(bad, zero, zero, zero, 0);
    assert(!memcmp(&corrected, &zero, sizeof(zero)) && failures == 1 && audits == 1);
    assert(!spu_float_native_active() && !spu_float_native_set_mode(1));
    puts(
        "Native float audit: corruption replaced with scalar output; failure cannot be re-enabled");
#endif
}
