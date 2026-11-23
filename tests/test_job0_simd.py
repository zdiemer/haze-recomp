"""Asset-free checks of the job-0 SIMD kernel generator on a synthetic loop."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from patches.job0_simd import kernel

LOOP = '''void spu_test_spu_func_00000100(spu_context* ctx) {
loc_00000100:
        ctx->gpr[10] = spu_ai(ctx->gpr[10], 1);
        ctx->gpr[3] = spu_ls_read128(ctx, ctx->gpr[20]._u32[0] + 0x0);
        ctx->gpr[11] = spu_cgt(ctx->gpr[30], ctx->gpr[10]);
        ctx->gpr[4] = spu_shufb(ctx->gpr[3], ctx->gpr[3], ctx->gpr[40]);
        /* nop */;
        ctx->gpr[5] = spu_fm(ctx->gpr[4], ctx->gpr[41]);
        ctx->gpr[6] = spu_fma(ctx->gpr[5], ctx->gpr[41], ctx->gpr[3]);
        ctx->gpr[7] = spu_and(ctx->gpr[6], ctx->gpr[42]);
        ctx->gpr[8] = spu_fa(ctx->gpr[5], ctx->gpr[6]);
        ctx->gpr[9] = spu_cflts(ctx->gpr[8], 150);
        ctx->gpr[20] = spu_ai(ctx->gpr[20], 16);
        spu_ls_write128(ctx, ctx->gpr[21]._u32[0] + 0x0, ctx->gpr[9]);
        ctx->gpr[21] = spu_ai(ctx->gpr[21], 16);
        if (ctx->gpr[11]._u32[0] != 0) goto loc_00000100;
        { ctx->pc = 0x200; g_spu_trampoline_fn = spu_test_spu_func_00000200; return; }
}
'''


class Job0Simd(unittest.TestCase):
    def generate(self, source=LOOP):
        return kernel(source, 'simd', 'step')

    def test_shape(self):
        text, stats = self.generate()
        self.assertEqual(stats['carried'], [10, 20, 21])
        self.assertEqual(stats['controls'], 1)
        # r6 reaches a bitwise consumer; r8 is final but only after the loop.
        self.assertEqual(stats['escapes'], 1)
        self.assertEqual(stats['finals'], 2)
        self.assertIn('static __attribute__((noinline)) void step(spu_context* ctx) {', text)
        self.assertIn('_mm_permutexvar_epi8(x40, r3)', text)
        self.assertIn('_mm_fmadd_ps(', text)
        self.assertIn('_mm_cvttps_epi32(_mm_mul_ps(', text)
        self.assertIn('ctx->pc = 0x200; g_spu_trampoline_fn = spu_test_spu_func_00000200;', text)
        # The flag check and fallback precede the store; the store follows.
        check = text.index('if (bad | (_mm_getcsr()')
        self.assertLess(text.index('r9 = _mm_cvttps_epi32'), check)
        self.assertLess(check, text.index('haze_job0_simd_st('))
        step = text[:text.index('static HAZE_JOB0_SIMD_TARGET')]
        self.assertIn('spu_ls_write128(ctx', step)
        self.assertNotIn('goto loc_00000100', step)

    def test_rejects_unsupported(self):
        cases = {
            'operation': LOOP.replace('spu_and(', 'spu_nand('),
            'store order': LOOP.replace('        ctx->gpr[20] = spu_ai(ctx->gpr[20], 16);\n', '').replace(
                '        ctx->gpr[21] = spu_ai(ctx->gpr[21], 16);\n',
                '        ctx->gpr[12] = spu_fa(ctx->gpr[5], ctx->gpr[5]);\n'),
            'loop-variant control': LOOP.replace('gpr[21] = spu_ai(ctx->gpr[21]', 'gpr[40] = spu_ai(ctx->gpr[40]'),
            'channel': LOOP.replace('/* nop */;', 'spu_wrch(ctx, 16, ctx->gpr[3]);'),
            'branch target': LOOP.replace('goto loc_00000100', 'goto loc_00000104'),
        }
        for name, source in cases.items():
            with self.subTest(name), self.assertRaises(ValueError):
                self.generate(source)


if __name__ == '__main__':
    unittest.main()
