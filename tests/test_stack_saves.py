"""Check which callee-save stores the stack-saves transform drops, on synthetic lifted code."""
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from patches.patch_stack_saves import patch_modules, POISON

SAVE = '        vm_write64(ctx->gpr[1] + 0x78, ctx->gpr[31]);'
ELIDED = '        /* callee save elided */;'


def function(name, *body, snapshot=True):
    lines = [f'void {name}(ppu_context* ctx) {{']
    if snapshot:
        lines.append('        uint64_t _cs_31 = ctx->gpr[31];')
    lines += ['        ' + line for line in body]
    return '\n'.join(lines + ['}'])


def prologue(*middle, tail='return;'):
    return ('vm_write64(ctx->gpr[1] + -0x80, ctx->gpr[1]); ctx->gpr[1] += -0x80;',
            SAVE.strip(),
            'ctx->gpr[31] = ctx->gpr[3];',
            *middle,
            'ctx->gpr[31] = _cs_31;',
            'ctx->gpr[1] = ctx->gpr[1] + (int64_t)(0x80);',
            tail)


def run(*functions):
    source = '#include "ppu_recomp.h"\n' + '\n\n'.join(functions) + '\n'
    results, dropped = patch_modules({'ppu_recomp_000.cpp': source})
    return results['ppu_recomp_000.cpp'], dropped


class StackSaves(unittest.TestCase):
    def assertKept(self, *functions):
        text, dropped = run(*functions)
        self.assertEqual(dropped, 0, text)
        self.assertIn(SAVE, text)

    def test_drops_unread_save(self):
        functions = (function('func_00010000', *prologue(
            'ctx->lr = 0x00010010; func_00020000(ctx); DRAIN_TRAMPOLINE(ctx);',
            'ctx->gpr[3] = vm_read32(ctx->gpr[1] + 0x70);')),
            function('func_00020000', 'return;', snapshot=False))
        text, dropped = run(*functions)
        self.assertEqual(dropped, 1)
        # Only the store line changes, so line numbers stay put.
        self.assertEqual(text.replace(ELIDED, SAVE),
                         '#include "ppu_recomp.h"\n' + '\n\n'.join(functions) + '\n')
        self.assertNotIn(SAVE, text)

    def test_poison_keeps_a_marker_store(self):
        source = '#include "ppu_recomp.h"\n' + function('func_00010000', *prologue()) + '\n'
        results, dropped = patch_modules({'m.cpp': source}, poison=True)
        self.assertEqual(dropped, 1)
        self.assertIn(f'vm_write64(ctx->gpr[1] + 0x78, 0x{POISON:X}ull);', results['m.cpp'])

    def test_keeps_save_without_host_restore(self):
        self.assertKept(function('func_00010000', *prologue(), snapshot=False))
        body = [line for line in prologue() if line != 'ctx->gpr[31] = _cs_31;']
        self.assertKept(function('func_00010000', *body))

    def test_keeps_non_callee_saved_register(self):
        text, dropped = run(function('func_00010000',
                                     'vm_write64(ctx->gpr[1] + 0x78, ctx->gpr[3]);', 'return;'))
        self.assertEqual(dropped, 0)

    def test_keeps_slot_read_at_any_frame_depth(self):
        for read in ('ctx->gpr[4] = vm_read32(ctx->gpr[1] + 0x7C);',
                     'ctx->gpr[4] = vm_read64(ctx->gpr[1] + -0x8);',
                     'ctx->gpr[4] = vm_read64(ctx->gpr[1] + 0xF8);'):
            with self.subTest(read=read):
                self.assertKept(function('func_00010000', *prologue(read)))

    def test_keeps_stack_argument_store(self):
        # Passing r31 as the ninth argument of a varargs call looks like a save
        # but the callee reads it from this frame.
        call = 'ctx->lr = 0x00010010; func_00020000(ctx); DRAIN_TRAMPOLINE(ctx);'
        for body in (('ctx->gpr[31] = vm_read32(ctx->gpr[3] + 0x0);', SAVE.strip(), call),
                     ('loc_00010008:', SAVE.strip(), call),
                     (call, SAVE.strip())):
            with self.subTest(body=body):
                self.assertKept(function('func_00010000',
                                         'vm_write64(ctx->gpr[1] + -0x80, ctx->gpr[1]); ctx->gpr[1] += -0x80;',
                                         *body, 'ctx->gpr[31] = _cs_31;',
                                         'ctx->gpr[1] = ctx->gpr[1] + (int64_t)(0x80);', 'return;'))

    def test_keeps_slot_written_twice(self):
        self.assertKept(function('func_00010000', *prologue(
            'vm_write64(ctx->gpr[1] + 0x78, ctx->gpr[5]);')))

    def test_keeps_frame_reached_through_derived_register(self):
        for middle in (('ctx->gpr[11] = vm_read64(ctx->gpr[1] + 0x0);',
                        'ctx->gpr[30] = vm_read64(ctx->gpr[11] + -0x10);'),
                       ('ctx->gpr[12] = ctx->gpr[1] | ctx->gpr[1];',
                        'ctx->gpr[9] = ctx->gpr[12] + (int64_t)(0x8);',
                        'ctx->gpr[30] = vm_read64(ctx->gpr[9] + 0x70);'),
                       ('ctx->gpr[9] = ctx->gpr[1] + (int64_t)(0x60);',
                        '{ uint64_t ea = (ctx->gpr[9]) & ~0xFULL; memcpy(&ctx->vr[31], vm_base + (uint32_t)ea, 16); }')):
            with self.subTest(middle=middle):
                self.assertKept(function('func_00010000', *prologue(*middle)))

    def test_derived_address_alone_does_not_block(self):
        text, dropped = run(function('func_00010000', *prologue(
            'ctx->gpr[4] = ctx->gpr[1] + (int64_t)(0x70);',
            'ctx->gpr[5] = vm_read32(ctx->gpr[4] + 0x0);',
            'ctx->lr = 0x00010010; func_00010000(ctx); DRAIN_TRAMPOLINE(ctx);')))
        self.assertEqual(dropped, 1)

    def test_keeps_unknown_frames(self):
        for middle in ('{ uint64_t ea = ctx->gpr[1] + ctx->gpr[0]; vm_write64(ea, ctx->gpr[1]); ctx->gpr[1] = ea; }',
                       '{ uint64_t ea = ((ctx->gpr[1] + ctx->gpr[0])) & ~0xFULL; memcpy(&ctx->vr[31], vm_base + (uint32_t)ea, 16); }',
                       'vm_write32(ctx->gpr[3] + 0x0, ctx->gpr[1]);',
                       'ctx->gpr[1] = ctx->gpr[1] + (int64_t)(0x40);',
                       'ps3_indirect_call(ctx); return;'):
            with self.subTest(middle=middle):
                self.assertKept(function('func_00010000', *prologue(middle)))

    def test_keeps_save_read_by_tail_branch_target(self):
        tail = '{ g_trampoline_fn = (void(*)(void*))func_00020000; return; }'
        continuation = function('func_00020000',
                                'ctx->gpr[31] = vm_read64(ctx->gpr[1] + 0x78);', 'return;', snapshot=False)
        self.assertKept(function('func_00010000', *prologue(tail=tail)), continuation)
        # Through a further tail branch, and to a target outside the lift.
        relay = function('func_00030000', '{ g_trampoline_fn = (void(*)(void*))func_00020000; return; }',
                         snapshot=False)
        relay_tail = '{ g_trampoline_fn = (void(*)(void*))func_00030000; return; }'
        self.assertKept(function('func_00010000', *prologue(tail=relay_tail)), relay, continuation)
        self.assertKept(function('func_00010000', *prologue(tail=tail)))

    def test_tail_branch_to_self_contained_frame_is_safe(self):
        tail = '{ g_trampoline_fn = (void(*)(void*))func_00020000; return; }'
        sibling = function('func_00020000', 'vm_write64(ctx->gpr[1] + -0x8, ctx->gpr[30]);',
                           'ctx->gpr[30] = vm_read64(ctx->gpr[1] + -0x8);', 'return;', snapshot=False)
        text, dropped = run(function('func_00010000', *prologue(tail=tail)), sibling)
        self.assertEqual(dropped, 1)

    def test_tail_entry_memory_snapshot_is_unsafe(self):
        tail = '{ g_trampoline_fn = (void(*)(void*))func_00020000; return; }'
        entry = '\n'.join(['void func_00020000(ppu_context* ctx) {',
                           '        uint64_t _cs_31 = vm_read64(ctx->gpr[1] + 0x78);',
                           '        ctx->gpr[31] = _cs_31;', '        return;', '}'])
        self.assertKept(function('func_00010000', *prologue(tail=tail)), entry)

    def assertPromoted(self, functions, count, *expected):
        source = '#include "ppu_recomp.h"\n' + '\n\n'.join(functions) + '\n'
        results, dropped = patch_modules({'m.cpp': source})
        text = results['m.cpp']
        self.assertEqual(dropped, count, text)
        for line in expected:
            self.assertIn(line, text)
        self.assertNotIn('vm_write64(ctx->gpr[1] + 0x78', text)
        return text

    def test_promotes_save_reloaded_whole(self):
        # No host restore: the epilogue reloads r31 from the slot.
        body = [line for line in prologue('ctx->gpr[31] = vm_read64(ctx->gpr[1] + 0x78);')
                if line != 'ctx->gpr[31] = _cs_31;']
        text = self.assertPromoted([function('func_00010000', *body, snapshot=False)], 1,
                                   '        uint64_t _ss_0 = ctx->gpr[31];',
                                   '        ctx->gpr[31] = _ss_0;')
        self.assertNotIn('vm_read64', text)

    def test_promotes_link_register_and_float_saves(self):
        lr = function('func_00010000',
                      'vm_write64(ctx->gpr[1] + -0x80, ctx->gpr[1]); ctx->gpr[1] += -0x80;',
                      'ctx->gpr[0] = ctx->lr;',
                      'vm_write64(ctx->gpr[1] + 0x90, ctx->gpr[0]);',
                      '{ uint64_t tmp; memcpy(&tmp, &ctx->fpr[31], 8); vm_write64(ctx->gpr[1] + 0x78, tmp); }',
                      'ctx->fpr[31] = ctx->fpr[1];',
                      'ctx->lr = 0x00010010; func_00020000(ctx); DRAIN_TRAMPOLINE(ctx);',
                      'if (((ctx->cr >> 0) & 2)) goto loc_00010040;',
                      'ctx->gpr[0] = vm_read64(ctx->gpr[1] + 0x90);',
                      '{ uint64_t tmp = vm_read64(ctx->gpr[1] + 0x78); memcpy(&ctx->fpr[31], &tmp, 8); }',
                      'ctx->gpr[1] = ctx->gpr[1] + (int64_t)(0x80);',
                      'ctx->lr = ctx->gpr[0];',
                      'return;',
                      'loc_00010040:',
                      '{ uint64_t tmp = vm_read64(ctx->gpr[1] + 0x78); memcpy(&ctx->fpr[31], &tmp, 8); }',
                      'ctx->gpr[1] = ctx->gpr[1] + (int64_t)(0x80);',
                      # After the pop the same slot is 0x80 nearer the stack pointer.
                      'ctx->gpr[0] = vm_read64(ctx->gpr[1] + 0x10);',
                      'ctx->lr = ctx->gpr[0];',
                      'return;', snapshot=False)
        text = self.assertPromoted([lr, function('func_00020000', 'return;', snapshot=False)], 2,
                                   '        uint64_t _ss_0 = ctx->gpr[0];',
                                   '        uint64_t _ss_1; memcpy(&_ss_1, &ctx->fpr[31], 8);',
                                   '        ctx->gpr[0] = _ss_0;',
                                   '        { uint64_t tmp = _ss_1; memcpy(&ctx->fpr[31], &tmp, 8); }')
        self.assertNotIn('vm_read64', text)
        self.assertNotIn('vm_write64(ctx->gpr[1] + 0x90', text)

    def test_keeps_saves_not_reloaded_exactly(self):
        no_restore = [line for line in prologue() if line != 'ctx->gpr[31] = _cs_31;']
        stdu, save, *rest = no_restore
        reload = 'ctx->gpr[31] = vm_read64(ctx->gpr[1] + 0x78);'
        for body in ((stdu, save, 'ctx->gpr[4] = vm_read32(ctx->gpr[1] + 0x7C);', reload, *rest),
                     (stdu, save, 'vm_write32(ctx->gpr[1] + 0x7C, ctx->gpr[4]);', reload, *rest),
                     # A label reached at two stack depths.
                     (stdu, save, 'loc_00010008:', reload, *rest[:-1],
                      'if (((ctx->cr >> 0) & 2)) goto loc_00010008;', 'return;'),
                     # A frame popped through another register leaves depths unknown.
                     (stdu, save, reload, 'ctx->gpr[11] = ctx->gpr[1] + (int64_t)(0x80);',
                      'ctx->gpr[1] = ctx->gpr[11] | ctx->gpr[11];', 'return;'),
                     # r0 stored outside the link register's slot is an argument.
                     (stdu, 'ctx->gpr[0] = ctx->lr;', 'vm_write64(ctx->gpr[1] + 0x78, ctx->gpr[0]);',
                      'ctx->gpr[0] = vm_read64(ctx->gpr[1] + 0x78);', *rest)):
            with self.subTest(body=body):
                text, dropped = run(function('func_00010000', *body, snapshot=False))
                self.assertEqual(dropped, 0, text)

    def test_poison_keeps_a_marker_store_for_promoted_saves(self):
        body = [line for line in prologue('ctx->gpr[31] = vm_read64(ctx->gpr[1] + 0x78);')
                if line != 'ctx->gpr[31] = _cs_31;']
        source = '#include "ppu_recomp.h"\n' + function('func_00010000', *body, snapshot=False) + '\n'
        results, dropped = patch_modules({'m.cpp': source}, poison=True)
        self.assertEqual(dropped, 1)
        self.assertIn(f'uint64_t _ss_0 = ctx->gpr[31]; vm_write64(ctx->gpr[1] + 0x78, 0x{POISON:X}ull);',
                      results['m.cpp'])

    def test_rejects_duplicate_definitions(self):
        with self.assertRaises(ValueError):
            run(function('func_00010000', 'return;'), function('func_00010000', 'return;'))


if __name__ == '__main__':
    unittest.main()
