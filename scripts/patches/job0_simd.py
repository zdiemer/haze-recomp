"""Generate a host-SIMD kernel for one straight-line job-0 SPU loop (#66).

The input is the lifted loop function. Each SPU register becomes an __m128i
local; loop-invariant shuffle controls become byte-permute indexes once per
call. Arithmetic runs under the MXCSR mode in port/haze_job0_simd.h, and an
iteration whose operands or flags leave the exact domain is repeated by an
unchanged copy of the translated body before it stores anything.

Only the operations listed here are supported; anything else fails closed.
The emitted source is game-derived and stays in the build directory.
"""
import re

FLOAT = {'spu_fa', 'spu_fs', 'spu_fm', 'spu_fma', 'spu_fnms', 'spu_csflt', 'spu_cuflt'}
# Operand positions where an exponent-255 value must be detected explicitly.
CHECKED = {'spu_fa', 'spu_fs', 'spu_fm', 'spu_fma', 'spu_fnms'}
# Consumers that cannot observe the sign of a zero operand.
ZERO_BLIND = CHECKED | {'spu_cflts'}
REG = r'ctx->gpr\[(\d+)\]'
ASSIGN = re.compile(r'ctx->gpr\[(\d+)\] = (spu_\w+)\((.*)\);$')
STORE = re.compile(r'spu_ls_write128\(ctx, ctx->gpr\[(\d+)\]\._u32\[0\] \+ (0x[0-9A-Fa-f]+), ctx->gpr\[(\d+)\]\);$')
ADDRESS = re.compile(r'ctx, ctx->gpr\[(\d+)\]\._u32\[0\] \+ (?:(0x[0-9A-Fa-f]+)|ctx->gpr\[(\d+)\]\._u32\[0\])$')
BRANCH = re.compile(r'if \(ctx->gpr\[(\d+)\]\._u32\[0\] != 0\) goto (loc_[0-9A-F]+);$')
EXIT = re.compile(r'\{ ctx->pc = (0x[0-9A-F]+); g_spu_trampoline_fn = (\w+); return; \}$')
IGNORED = ('/* nop */;', '/* branch hint (ignored) */;')


class Op:
    def __init__(self, kind, dest=None, name=None, args=(), imm=None, line=''):
        self.kind, self.dest, self.name, self.args, self.imm, self.line = kind, dest, name, list(args), imm, line

    def reads(self):
        return list(self.args)


def parse(function):
    lines = [l.strip() for l in function.strip().split('\n')]
    header = lines[0]
    match = re.match(r'void (\w+)\(spu_context\* ctx\) \{$', header)
    if not match or lines[1] != match.group(1).split('_spu_func_')[1].join(('loc_', ':')) or lines[-1] != '}':
        raise ValueError('Unexpected loop function shape')
    ops, branch, exit_line = [], None, None
    for line in lines[2:-1]:
        if not line or line in IGNORED:
            continue
        if branch:
            if exit_line or not EXIT.match(line):
                raise ValueError('Unexpected statement after loop branch: ' + line)
            exit_line = line
            continue
        if m := BRANCH.match(line):
            if m.group(2) + ':' != lines[1]:
                raise ValueError('Loop branch does not target the entry')
            branch = int(m.group(1))
            continue
        if m := STORE.match(line):
            ops.append(Op('store', args=(int(m.group(1)), int(m.group(3))), imm=int(m.group(2), 16), line=line))
            continue
        m = ASSIGN.match(line)
        if not m:
            raise ValueError('Unsupported statement: ' + line)
        dest, name, text = int(m.group(1)), m.group(2), m.group(3)
        if name == 'spu_ls_read128':
            a = ADDRESS.match(text)
            if not a:
                raise ValueError('Unsupported load address: ' + line)
            if a.group(2):
                ops.append(Op('load', dest, name, (int(a.group(1)),), int(a.group(2), 16), line))
            else:
                ops.append(Op('load', dest, name, (int(a.group(1)), int(a.group(3))), None, line))
            continue
        parts = [p.strip() for p in text.split(',')]
        regs, imm = [], None
        for p in parts:
            if r := re.fullmatch(REG, p):
                if imm is not None:
                    raise ValueError('Unsupported operand order: ' + line)
                regs.append(int(r.group(1)))
            elif re.fullmatch(r'-?\d+', p):
                if imm is not None:
                    raise ValueError('Unsupported operands: ' + line)
                imm = int(p)
            else:
                raise ValueError('Unsupported operand: ' + line)
        ops.append(Op('op', dest, name, regs, imm, line))
    if branch is None or exit_line is None:
        raise ValueError('Loop branch or exit is missing')
    return match.group(1), ops, branch, exit_line


ARITY = {'spu_fa': (2, False), 'spu_fs': (2, False), 'spu_fm': (2, False), 'spu_fma': (3, False),
         'spu_fnms': (3, False), 'spu_csflt': (1, True), 'spu_cuflt': (1, True), 'spu_cflts': (1, True),
         'spu_shufb': (3, False), 'spu_rotqbyi': (1, True), 'spu_shli': (1, True), 'spu_andi': (1, True),
         'spu_ai': (1, True), 'spu_a': (2, False), 'spu_and': (2, False), 'spu_andc': (2, False),
         'spu_or': (2, False), 'spu_xor': (2, False), 'spu_selb': (3, False), 'spu_cgt': (2, False)}


def scale(power):
    if not -126 <= power <= 127:
        raise ValueError('Conversion scale is outside the normal range')
    return f'_mm_set1_ps({2.0 ** power!r}f)' if power else None


def rotate(value, count):
    count &= 15
    if not count:
        return value
    if count % 4 == 0:
        words = count // 4
        imm = sum(((i + words) & 3) << (2 * i) for i in range(4))
        return f'_mm_shuffle_epi32({value}, {imm})'
    w = lambda p: (p & ~3) | (3 - (p & 3))
    mask = ', '.join(str(w((w(h) + count) & 15)) for h in range(16))
    return f'_mm_shuffle_epi8({value}, _mm_setr_epi8({mask}))'


def expression(op, values, controls):
    a = [values(r) for r in op.args]
    f = lambda x: f'_mm_castsi128_ps({x})'
    i = lambda x: f'_mm_castps_si128({x})'
    n, imm = op.name, op.imm
    if n == 'spu_fa': return i(f'_mm_add_ps({f(a[0])}, {f(a[1])})')
    if n == 'spu_fs': return i(f'_mm_sub_ps({f(a[0])}, {f(a[1])})')
    if n == 'spu_fm': return i(f'_mm_mul_ps({f(a[0])}, {f(a[1])})')
    if n == 'spu_fma': return i(f'_mm_fmadd_ps({f(a[0])}, {f(a[1])}, {f(a[2])})')
    if n == 'spu_fnms': return i(f'_mm_fnmadd_ps({f(a[0])}, {f(a[1])}, {f(a[2])})')
    if n in ('spu_csflt', 'spu_cuflt'):
        value = ('_mm_cvtepi32_ps' if n == 'spu_csflt' else '_mm_cvtepu32_ps') + f'({a[0]})'
        factor = scale(imm - 155)
        return i(f'_mm_mul_ps({value}, {factor})' if factor else value)
    if n == 'spu_cflts':
        factor = scale(173 - imm)
        return f'_mm_cvttps_epi32({f"_mm_mul_ps({f(a[0])}, {factor})" if factor else f(a[0])})'
    if n == 'spu_shufb':
        index = controls[op.args[2]]
        if op.args[0] == op.args[1]:
            return f'_mm_permutexvar_epi8({index}, {a[0]})'
        return f'_mm_permutex2var_epi8({a[0]}, {index}, {a[1]})'
    if n == 'spu_rotqbyi': return rotate(a[0], imm)
    if n == 'spu_shli':
        imm &= 0x3F
        return '_mm_setzero_si128()' if imm > 31 else f'_mm_slli_epi32({a[0]}, {imm})'
    if n == 'spu_andi': return f'_mm_and_si128({a[0]}, _mm_set1_epi32({imm}))'
    if n == 'spu_ai': return f'_mm_add_epi32({a[0]}, _mm_set1_epi32({imm}))'
    if n == 'spu_a': return f'_mm_add_epi32({a[0]}, {a[1]})'
    if n == 'spu_and': return f'_mm_and_si128({a[0]}, {a[1]})'
    if n == 'spu_andc': return f'_mm_andnot_si128({a[1]}, {a[0]})'
    if n == 'spu_or': return f'_mm_or_si128({a[0]}, {a[1]})'
    if n == 'spu_xor': return f'_mm_xor_si128({a[0]}, {a[1]})'
    if n == 'spu_selb': return f'_mm_ternarylogic_epi32({a[2]}, {a[1]}, {a[0]}, 0xCA)'
    if n == 'spu_cgt': return f'_mm_cmpgt_epi32({a[0]}, {a[1]})'
    raise ValueError('Unsupported operation: ' + op.line)


def kernel(function, name, step_name):
    """Return C source for `step_name` (one original iteration) and `name`.

    `name(ctx)` returns 0 without side effects when a call-level precondition
    fails, otherwise 1 plus the number of iterations repeated by the step.
    """
    entry, ops, branch, exit_line = parse(function)
    for op in ops:
        if op.kind == 'op':
            count, has_imm = ARITY.get(op.name, (None, None))
            if count is None or len(op.args) != count or (op.imm is not None) != has_imm:
                raise ValueError('Unsupported operation: ' + op.line)
    first_store = next(k for k, op in enumerate(ops) if op.kind == 'store')
    if any(op.kind == 'load' or op.name in FLOAT or op.name == 'spu_cflts' for op in ops[first_store:]):
        raise ValueError('Loads and arithmetic must precede the first store')
    written = sorted({op.dest for op in ops if op.dest is not None})
    read = {r for op in ops for r in op.reads()} | {branch}
    invariant = sorted(read - set(written))
    defined, carried = set(), set()
    for op in ops:
        carried |= {r for r in op.reads() if r not in defined and r in written}
        if op.dest is not None:
            defined.add(op.dest)
    if branch not in defined:
        raise ValueError('Loop condition is not computed in the body')
    control_regs = sorted({op.args[2] for op in ops if op.name == 'spu_shufb'})
    if set(control_regs) - set(invariant):
        raise ValueError('Shuffle control changes inside the loop')

    # Reaching definitions within one iteration; None means the value at entry.
    last, origin = {}, []
    for k, op in enumerate(ops):
        origin.append([last.get(r) for r in op.reads()])
        if op.dest is not None:
            last[op.dest] = k
    final = dict(last)
    # A host NaN or infinity (an SPU exponent-255 operand) propagates through
    # arithmetic or raises invalid, and -0 differs from the SPU only in sign.
    # Both are checked and canonicalized where an arithmetic result escapes:
    # to a consumer that sees its bits, or as a register's final value (the
    # next iteration, the loop condition and the exported state).
    # Final values only escape after the last iteration; the loop condition
    # is known before the first store.
    escapes, finals = set(), set()
    for k, op in enumerate(ops):
        if op.name not in CHECKED:
            continue
        for j in range(k + 1, len(ops)):
            if k in origin[j] and ops[j].name not in ZERO_BLIND:
                escapes.add(k)
            if ops[j].dest == op.dest:
                break
        if final[op.dest] == k and k not in escapes:
            if op.dest in carried:
                escapes.add(k)
            else:
                finals.add(k)
    if final[branch] > first_store or ops[final[branch]].name in FLOAT:
        raise ValueError('Loop condition is not an integer known before the first store')

    out = []
    emit = out.append
    body = function[function.index('\n') + 1:function.rindex('}')]
    body = body[body.index('\n') + 1:]
    loop_branch = body.index(f'if (ctx->gpr[{branch}]._u32[0] != 0) goto ')
    step = body[:body.rindex('\n', 0, loop_branch) + 1]
    emit(f'static __attribute__((noinline)) void {step_name}(spu_context* ctx) {{\n{step}}}\n\n')
    emit(f'static HAZE_JOB0_SIMD_TARGET __attribute__((noinline)) unsigned {name}(spu_context* ctx) {{\n')
    emit('#pragma STDC FENV_ACCESS ON\n')
    for r in invariant:
        emit(f'    const __m128i r{r} = haze_job0_simd_v(ctx->gpr[{r}]);\n')
    for r in written:
        emit(f'    __m128i r{r} = haze_job0_simd_v(ctx->gpr[{r}]);\n')
    controls = {}
    emit('    int valid = 1;\n')
    for r in control_regs:
        controls[r] = f'x{r}'
        emit(f'    __m128i x{r}; valid &= haze_job0_simd_control(r{r}, &x{r});\n')
    emit('    if (!valid) return 0;\n')
    emit('    uint8_t* const ls = ctx->ls;\n    unsigned repeated = 0;\n')
    emit('    const unsigned csr = _mm_getcsr();\n    _mm_setcsr(HAZE_JOB0_SIMD_MXCSR);\n')
    emit('    do {\n')
    for r in sorted(carried):
        emit(f'        const __m128i s{r} = r{r};\n')
    emit('        __mmask8 bad = 0;\n')
    values = lambda r: f'r{r}'
    for k, op in enumerate(ops):
        if k == first_store:
            if finals:
                emit(f'        if (!haze_job0_simd_word(r{branch})) {{\n')
                for d in sorted(finals):
                    r = ops[d].dest
                    emit(f'            r{r} = _mm_castps_si128(_mm_add_ps(_mm_castsi128_ps(r{r}), _mm_setzero_ps()));\n')
                    emit(f'            bad |= haze_job0_simd_special(r{r});\n')
                emit('        }\n')
            emit('        if (bad | (_mm_getcsr() & HAZE_JOB0_SIMD_FLAGS)) {\n')
            emit('            _mm_setcsr(csr);\n')
            for r in sorted(carried):
                emit(f'            ctx->gpr[{r}] = haze_job0_simd_u(s{r});\n')
            emit(f'            {step_name}(ctx);\n')
            for r in written:
                emit(f'            r{r} = haze_job0_simd_v(ctx->gpr[{r}]);\n')
            emit('            _mm_setcsr(HAZE_JOB0_SIMD_MXCSR);\n            ++repeated;\n            continue;\n        }\n')
        if op.kind == 'load':
            address = f'haze_job0_simd_word(r{op.args[0]}) + ' + (
                f'0x{op.imm:X}u' if op.imm is not None else f'haze_job0_simd_word(r{op.args[1]})')
            emit(f'        r{op.dest} = haze_job0_simd_ld(ls, {address});\n')
        elif op.kind == 'store':
            emit(f'        haze_job0_simd_st(ls, haze_job0_simd_word(r{op.args[0]}) + 0x{op.imm:X}u, r{op.args[1]});\n')
            continue
        else:
            emit(f'        r{op.dest} = {expression(op, values, controls)};\n')
        if k in escapes:
            emit(f'        r{op.dest} = _mm_castps_si128(_mm_add_ps(_mm_castsi128_ps(r{op.dest}), _mm_setzero_ps()));\n')
            emit(f'        bad |= haze_job0_simd_special(r{op.dest});\n')
    emit(f'    }} while (haze_job0_simd_word(r{branch}) != 0);\n')
    emit('    _mm_setcsr(csr);\n')
    for r in written:
        emit(f'    ctx->gpr[{r}] = haze_job0_simd_u(r{r});\n')
    m = EXIT.match(exit_line)
    emit(f'    ctx->pc = {m.group(1)}; g_spu_trampoline_fn = {m.group(2)};\n    return 1 + repeated;\n}}\n')
    stats = {'ops': len(ops), 'escapes': len(escapes), 'finals': len(finals), 'carried': sorted(carried), 'controls': len(control_regs)}
    return ''.join(out), stats
