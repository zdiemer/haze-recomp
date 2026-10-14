"""Build-local regression fixture for Haze's recovered 0x29788 switch."""
from pathlib import Path
import argparse
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from patches import patch_local_dispatch as local_dispatch

DEFAULT_SOURCE_PATH = ROOT / 'generated/ppu/ppu_recomp_000.cpp'
EXPECTED_CALLS = (
    'func_00029328', 'func_00028110', 'func_0001E310', 'func_0001E4F0',
    'func_00015960', 'func_0001E6F0', 'func_00015590', 'func_000193F8',
    'func_00018820',
)


def _function(source):
    start = source.index(local_dispatch.FUNCTION)
    end = source.index('\nvoid ', start + len(local_dispatch.FUNCTION))
    return source[start:end]


def _harness(function: str) -> str:
    callees = sorted(set(re.findall(r'\b(func_[0-9A-Fa-f]+)\(ctx\);', function)))
    callees = [name for name in callees if name != 'func_000296A0']
    if 'func_00018D48' not in callees:
        callees.append('func_00018D48')  # referenced by the preserved tail trampoline
    case_specs = ',\n'.join(
        f'    {{0x{target:08X}u, 0x{return_pc:08X}u, "{callee}"}}'
        for (target, return_pc), callee in zip(local_dispatch.CASES, EXPECTED_CALLS))
    case_fpr1 = ', '.join('false' if i in (4, 7, 8) else 'true'
                          for i in range(len(EXPECTED_CALLS)))
    stubs = '\n'.join(
        f'''void {name}(ppu_context* ctx) {{
    assert(g_expected_callee && std::string(g_expected_callee) == "{name}");
    assert(ctx->lr == g_expected_return_pc);
    assert(ctx->gpr[1] == kStack - 0xA0);
    assert(ctx->gpr[3] == kArg3);
    if (g_expected_case == 1) assert(ctx->gpr[4] == kArg4);
    if (g_case_sets_fpr1[g_expected_case]) assert(ctx->fpr[1] == ctx->fpr[31]);
    g_last_call = "{name}";
    ctx->gpr[3] = 0xDEAD0003; ctx->gpr[4] = 0xDEAD0004;
    ctx->lr = 0xDEAD0005; ctx->fpr[1] = -999.0;
}}'''
        for name in callees)
    return r'''#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>

struct ppu_context {
    uint64_t gpr[32]{};
    double fpr[32]{};
    uint32_t cr{};
    uint64_t lr{}, ctr{};
};
static std::unordered_map<uint64_t, uint64_t> memory;
static const char* g_last_call = nullptr;
static const char* g_expected_callee = nullptr;
static uint64_t g_expected_return_pc = 0;
static unsigned g_expected_case = 0;
static unsigned g_indirect_calls = 0;
using trampoline_t = void (*)(void*);
static trampoline_t g_trampoline_fn = nullptr;
static constexpr uint64_t kToc = 0x00E94EE0u;
static constexpr uint64_t kRoot = 0x00200000u;
static constexpr uint64_t kState = 0x00210000u;
static constexpr uint64_t kArg3 = 0x00500000u;
static constexpr uint64_t kArg4 = 0x00600000u;
static constexpr uint64_t kStack = 0x00700000u;
static constexpr uint64_t kTable = 0x0002978Cu;
static const bool g_case_sets_fpr1[] = {''' + case_fpr1 + r'''};

uint32_t vm_read32(uint64_t a) { return (uint32_t)memory[a]; }
uint64_t vm_read64(uint64_t a) { return memory[a]; }
void vm_write32(uint64_t a, uint64_t v) { memory[a] = (uint32_t)v; }
void vm_write64(uint64_t a, uint64_t v) { memory[a] = v; }
uint64_t ppc_rldicl(uint64_t x, unsigned sh, unsigned mb) {
    const uint64_t rotated = sh ? (x << sh) | (x >> (64 - sh)) : x;
    const uint64_t mask = mb ? (UINT64_MAX >> mb) : UINT64_MAX;
    return rotated & mask;
}
uint64_t ppc_rldic(uint64_t x, unsigned sh, unsigned mb) {
    return (x << sh) & (UINT64_MAX >> mb);
}
uint32_t ppc_rlwinm(uint32_t x, unsigned sh, unsigned mb, unsigned me) {
    (void)mb; (void)me;
    return sh ? (x << sh) | (x >> (32 - sh)) : x;
}
double ppu_fp_single(double x) { return (float)x; }
double ppu_fsub(double a, double b) { return a - b; }
double ppu_fmul(double a, double b) { return a * b; }
double ppu_fmadd_core(double a, double b, double c, int, int) { return a * b + c; }
double ppu_fdiv(double a, double b) { return a / b; }
void ps3_indirect_call(ppu_context*) { ++g_indirect_calls; }
#define DRAIN_TRAMPOLINE(ctx) ((void)0)
''' + stubs + r'''

''' + function + r'''

struct Case { uint32_t target, return_pc; const char* callee; };
static const Case cases[] = {
''' + case_specs + r'''
};

static ppu_context initial_context() {
    ppu_context c{};
    c.gpr[1] = kStack; c.gpr[2] = kToc;
    c.gpr[3] = kArg3; c.gpr[4] = kArg4;
    c.gpr[27] = 0x27272727; c.gpr[28] = 0x28282828;
    c.gpr[29] = 0x29292929; c.gpr[30] = 0x30303030;
    c.gpr[31] = 0x31313131; c.lr = 0x123456789ABCDEF0ull;
    c.fpr[1] = 11.25; c.fpr[31] = 31.5;
    memory.clear();
    memory[kToc - 0x7FB4] = kRoot;
    memory[kRoot - 0x7FD8] = kState;
    memory[kState] = 0; memory[kState + 4] = 0; memory[kState + 8] = 0;
    memory[kArg3 + 0x3C] = 0;
    memory[kRoot - 0x793C] = kTable;
    g_last_call = nullptr; g_indirect_calls = 0; g_trampoline_fn = nullptr;
    return c;
}

static void run_case(const Case& item, uint32_t index) {
    ppu_context c = initial_context();
    memory[kArg3 + 0x1C] = index;
    memory[kTable + index * 4] = item.target - kTable;
    g_expected_case = index;
    g_expected_callee = item.callee;
    g_expected_return_pc = item.return_pc;
    const uint64_t saved_stack = c.gpr[1];
    const uint64_t saved_lr = c.lr;
    const uint64_t saved_gpr[5] = {c.gpr[27],c.gpr[28],c.gpr[29],c.gpr[30],c.gpr[31]};
    const double saved_fpr31 = c.fpr[31];
    func_000296A0(&c);
    assert(g_last_call && std::string(g_last_call) == item.callee);
    assert(g_indirect_calls == 0);
    assert(c.gpr[1] == saved_stack && c.lr == saved_lr && c.gpr[2] == kToc);
    for (unsigned i = 0; i != 5; ++i) assert(c.gpr[27 + i] == saved_gpr[i]);
    assert(c.gpr[3] == kArg3 && c.gpr[4] == kArg4);
    assert(c.fpr[31] == saved_fpr31);
    assert(g_trampoline_fn == reinterpret_cast<trampoline_t>(func_00018D48));
    assert(memory[saved_stack - 0xA0 + 0x88] == saved_gpr[3]);
    assert(memory[saved_stack - 0xA0 + 0xB0] == saved_lr);
}

int main() {
    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i)
        run_case(cases[i], i);

    // An in-range table entry producing an unknown target uses the preserved
    // indirect-call fallback and returns before the shared epilogue.
    ppu_context c = initial_context();
    memory[kArg3 + 0x1C] = 0;
    memory[kTable] = 0x0002A000u - (uint32_t)kTable;
    func_000296A0(&c);
    assert(g_indirect_calls == 1 && g_last_call == nullptr);
    assert(c.ctr == 0x0002A000u && c.gpr[1] == kStack - 0xA0);
    assert(g_trampoline_fn == nullptr);

    // Index 9 follows the original out-of-range branch to its tail trampoline.
    c = initial_context();
    memory[kArg3 + 0x1C] = 9;
    func_000296A0(&c);
    assert(g_indirect_calls == 0 && g_last_call == nullptr);
    assert(c.gpr[1] == kStack && c.lr == 0x123456789ABCDEF0ull);
    assert(c.gpr[27] == 0x27272727 && c.gpr[28] == 0x28282828);
    assert(c.gpr[29] == 0x29292929 && c.gpr[30] == 0x30303030);
    assert(c.gpr[31] == 0x31313131);
    assert(g_trampoline_fn == reinterpret_cast<trampoline_t>(func_00018D48));
}
'''


class LocalDispatchPatchTests(unittest.TestCase):
    source_path = DEFAULT_SOURCE_PATH
    compiler_value = None
    compiler_was_explicit = False
    source_was_explicit = False

    @classmethod
    def setUpClass(cls):
        if not cls.source_path.exists():
            if cls.source_was_explicit:
                raise AssertionError(f'generated source does not exist: {cls.source_path}')
            raise unittest.SkipTest('Requires the private BLUS30094 generated lift')
        cls.source = cls.source_path.read_text()
        cls.patched = local_dispatch.patch(cls.source)

    def test_exactly_expected_local_edits_and_hash_guard(self):
        restored = self.patched.replace(local_dispatch.SWITCH,
                                        local_dispatch.FALLBACK, 1)
        self.assertEqual(restored.count(local_dispatch.SWITCH), 0)
        for label in local_dispatch.LABELS:
            self.assertEqual(restored.count(label), 1)
            restored = restored.replace(label, '', 1)
        self.assertEqual(restored, self.source)
        with self.assertRaises(ValueError):
            local_dispatch.patch(self.patched)
        changed = self.source.replace('void func_000296A0(ppu_context* ctx) {',
                                      'void func_000296A0(ppu_context* ctx) {\n /* changed */', 1)
        with self.assertRaises(ValueError):
            local_dispatch.patch(changed)

    def test_actual_patched_function_routes_cases_and_fallback(self):
        if self.compiler_value:
            if self.compiler_was_explicit:
                compiler = [self.compiler_value]
            else:
                compiler = shlex.split(self.compiler_value)
            if len(compiler) == 1 and not (Path(compiler[0]).exists() or shutil.which(compiler[0])):
                self.fail(f'explicit C++ compiler not found: {compiler[0]}')
        else:
            auto = shutil.which('g++') or shutil.which('clang++')
            compiler = [auto] if auto else None
        if not compiler:
            self.skipTest('g++ or clang++ required for extracted-function fixture')
        compiler_name = Path(compiler[0]).name.lower()
        is_msvc_style = compiler_name in ('clang-cl', 'clang-cl.exe', 'cl', 'cl.exe')
        fixture = _harness(_function(self.patched))
        with tempfile.TemporaryDirectory(prefix='haze-local-dispatch-') as tmp:
            source = Path(tmp) / 'fixture.cpp'
            binary = Path(tmp) / 'fixture.exe'
            obj = Path(tmp) / 'fixture.obj'
            source.write_text(fixture)
            if is_msvc_style:
                command = compiler + [
                    '/nologo', '/std:c++17', '/Od', '/EHsc',
                    f'/Fe{binary}', f'/Fo{obj}', str(source)]
            else:
                command = compiler + ['-std=c++17', '-O0', str(source), '-o', str(binary)]
            built = subprocess.run(command,
                                   text=True, capture_output=True)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(binary)], text=True, capture_output=True)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument('--compiler')
    parser.add_argument('--source', type=Path)
    args, remainder = parser.parse_known_args()
    if args.source is not None:
        LocalDispatchPatchTests.source_path = args.source
        LocalDispatchPatchTests.source_was_explicit = True
    if args.compiler is not None:
        LocalDispatchPatchTests.compiler_value = args.compiler
        LocalDispatchPatchTests.compiler_was_explicit = True
    elif os.environ.get('CC'):
        LocalDispatchPatchTests.compiler_value = os.environ['CC']
        LocalDispatchPatchTests.compiler_was_explicit = True
    unittest.main(argv=[sys.argv[0], *remainder])
