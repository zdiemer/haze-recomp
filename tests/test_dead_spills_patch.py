"""Dead callee-save spills: only stores no lifted code can read are dropped."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from patches.patch_dead_spills import patch as patch_with, reloaded_slots


def patch(source, shared=frozenset()):
    return patch_with(source, set(shared))


def function(address, body):
    return f'void func_{address}(ppu_context* ctx) {{\n{body}}}\n'


PROLOGUE = ('        uint64_t _cs_30 = ctx->gpr[30];\n'
            '        uint64_t _cs_31 = ctx->gpr[31];\n'
            '        vm_write64(ctx->gpr[1] + -0x30, ctx->gpr[1]); ctx->gpr[1] += -0x30;\n'
            '        ctx->gpr[0] = ctx->lr;\n'
            '        vm_write64(ctx->gpr[1] + 0x20, ctx->gpr[30]);\n'
            '        vm_write64(ctx->gpr[1] + 0x28, ctx->gpr[31]);\n'
            '        vm_write64(ctx->gpr[1] + 0x40, ctx->gpr[0]);\n')
EPILOGUE = ('        ctx->gpr[0] = vm_read64(ctx->gpr[1] + 0x40);\n'
            '        ctx->gpr[30] = _cs_30;\n'
            '        ctx->gpr[31] = _cs_31;\n'
            '        ctx->lr = ctx->gpr[0];\n'
            '        ctx->gpr[1] = ctx->gpr[1] + (int64_t)(0x30);\n'
            '        return;\n')


class DeadSpills(unittest.TestCase):
    def test_drops_only_host_restored_callee_saves(self):
        result = patch(function('00010000', PROLOGUE + EPILOGUE))
        self.assertNotIn('ctx->gpr[30]);', result)
        self.assertNotIn('ctx->gpr[31]);', result)
        # The back chain and the link register are read back and stay.
        self.assertIn('vm_write64(ctx->gpr[1] + -0x30, ctx->gpr[1]);', result)
        self.assertIn('vm_write64(ctx->gpr[1] + 0x40, ctx->gpr[0]);', result)
        self.assertEqual(result.count('\n'), (PROLOGUE + EPILOGUE).count('\n') + 3 - 2)

    def test_keeps_saves_that_are_read_or_reloaded(self):
        reloaded = EPILOGUE.replace('ctx->gpr[31] = _cs_31;', 'ctx->gpr[31] = vm_read64(ctx->gpr[11] + -0x8);')
        result = patch(function('00010000', PROLOGUE + reloaded))
        self.assertIn('vm_write64(ctx->gpr[1] + 0x28, ctx->gpr[31]);', result)
        self.assertNotIn('ctx->gpr[30]);', result)
        slot_read = '        ctx->gpr[3] = vm_read64(ctx->gpr[1] + 0x20);\n'
        result = patch(function('00010000', PROLOGUE + slot_read + EPILOGUE))
        self.assertIn('vm_write64(ctx->gpr[1] + 0x20, ctx->gpr[30]);', result)
        unrestored = EPILOGUE.replace('        ctx->gpr[30] = _cs_30;\n', '')
        self.assertIn('ctx->gpr[30]);', patch(function('00010000', PROLOGUE + unrestored)))

    def test_functions_are_independent(self):
        reader = function('00010100', '        ctx->gpr[3] = vm_read64(ctx->gpr[1] + 0x20);\n        return;\n')
        result = patch(function('00010000', PROLOGUE + EPILOGUE) + '\n' + reader)
        self.assertNotIn('ctx->gpr[30]);', result)

    def test_slots_reloaded_anywhere_stay_written(self):
        # Tail entries and shared epilogues reload a slot another function saved.
        tail = function('00010040', '        uint64_t _cs_31 = vm_read64(ctx->gpr[1] + 0x28);\n'
                                    '        ctx->gpr[31] = _cs_31;\n        return;\n')
        shared_epilogue = function('00010080', '        ctx->gpr[30] = vm_read64(ctx->gpr[1] + 0x20);\n'
                                               '        return;\n')
        self.assertEqual(reloaded_slots([tail, shared_epilogue]), {(31, '0x28'), (30, '0x20')})
        result = patch(function('00010000', PROLOGUE + EPILOGUE), reloaded_slots([tail, shared_epilogue]))
        self.assertIn('vm_write64(ctx->gpr[1] + 0x20, ctx->gpr[30]);', result)
        self.assertIn('vm_write64(ctx->gpr[1] + 0x28, ctx->gpr[31]);', result)
        # A reload inside the module itself is honored even if the lift scan missed it.
        result = patch(function('00010000', PROLOGUE + EPILOGUE) + '\n' + shared_epilogue)
        self.assertIn('vm_write64(ctx->gpr[1] + 0x20, ctx->gpr[30]);', result)
        self.assertNotIn('ctx->gpr[31]);', result)

    def test_only_prologue_saves_are_dropped(self):
        # A later store of a callee-saved register into a local passed by
        # address is data, not a save, even though the epilogue uses _cs_.
        local = ('        ctx->gpr[30] = ctx->gpr[3] | ctx->gpr[3];\n'
                 '        vm_write64(ctx->gpr[1] + 0x18, ctx->gpr[30]);\n'
                 '        ctx->gpr[4] = ctx->gpr[1] + (int64_t)(0x18);\n')
        result = patch(function('00010000', PROLOGUE + local + EPILOGUE))
        self.assertIn('vm_write64(ctx->gpr[1] + 0x18, ctx->gpr[30]);', result)
        self.assertNotIn('vm_write64(ctx->gpr[1] + 0x20, ctx->gpr[30]);', result)
        # A slot written twice, or whose address escapes, is not a save slot.
        twice = '        vm_write32(ctx->gpr[1] + 0x28, ctx->gpr[5]);\n'
        self.assertIn('0x28, ctx->gpr[31]);', patch(function('00010000', PROLOGUE + twice + EPILOGUE)))
        escaped = '        ctx->gpr[4] = ctx->gpr[1] + (int64_t)(0x28);\n'
        self.assertIn('0x28, ctx->gpr[31]);', patch(function('00010000', PROLOGUE + escaped + EPILOGUE)))
        # A save after the register already changed is not a prologue save.
        late = PROLOGUE.replace("        vm_write64(ctx->gpr[1] + 0x28, ctx->gpr[31]);\n",
                                "        ctx->gpr[31] = ctx->gpr[3] | ctx->gpr[3];\n"
                                "        vm_write64(ctx->gpr[1] + 0x28, ctx->gpr[31]);\n")
        self.assertIn('0x28, ctx->gpr[31]);', patch(function('00010000', late + EPILOGUE)))

    def test_rejects_repeated_application(self):
        with self.assertRaises(ValueError):
            patch(patch(function('00010000', PROLOGUE + EPILOGUE)))


if __name__ == '__main__':
    unittest.main()
