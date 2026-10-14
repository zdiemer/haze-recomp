"""Add profiling-only scopes at audited Haze job boundaries, without guest reads."""
import hashlib

# Input is the build-local result of the pacer, completion, and VP patches.
DIGESTS = {
    '00682100': 'd813f25e8ca6832131489c5db869db00db58a133cd260993c3bd7209deda3fe6',
    '00682538': '60a7eeebdc65bf3ea8ef2169da849ebce9c26b03979719eaede12b67532ada7a',
    '00682648': 'cf058cabbf70f872c0dccf7f4b0547bbd1314ce4d5506199d8bc1a785e2b6fea',
    '00682900': '50e16269a192dc8f1de06b2c68384f6e79d08dedcdcd0da85de910de3a923420',
    '007E56E0': '297be409c2afb11835d7e9b1141ff9c85fda2039e067b41228713b25470be06a',
}
KINDS = {'00682100': 'Completion', '00682538': 'Wait', '00682648': 'Submit',
         '00682900': 'Handler', '007E56E0': 'Kernel'}
INCLUDE = '#include "haze_job_trace.h"\n'
CALL = '        ps3_indirect_call(ctx); DRAIN_TRAMPOLINE(ctx);'


def guarded(text):
    return '#if defined(HAZE_JOB_TRACE) && defined(_WIN32)\n' + text + '#endif\n'


def entry(address):
    text = ('        HazeJobTraceScope job_trace(static_cast<unsigned>(HazeJobTraceKind::'
            + KINDS[address] + '), ctx->thread_id, ctx->gpr[2], ctx->lr, &ctx->gpr[3], 8);\n')
    if address in ('00682538', '00682100'):
        text += '        if (ctx->gpr[3] < 512) job_trace.set_slot(static_cast<uint32_t>(ctx->gpr[3]));\n'
    return guarded(text)


SLOT = guarded('        if (ctx->gpr[28] < 512) job_trace.set_slot(static_cast<uint32_t>(ctx->gpr[28]));\n')
ROUTE = guarded('        job_trace.set_route(static_cast<uint32_t>(ctx->gpr[27]));\n')
BASE = guarded('        job_trace.set_slot_base(static_cast<uint32_t>(ctx->gpr[3]));\n')
BASE_CALLS = {'00682648': '        ctx->lr = 0x00682758; func_00682068(ctx); DRAIN_TRAMPOLINE(ctx);\n',
              '00682100': '        ctx->lr = 0x00682118; func_00682068(ctx); DRAIN_TRAMPOLINE(ctx);\n'}
DISPATCH_BEGIN = guarded(
    '        {\n'
    '        const uint64_t job_args[] = {ctx->gpr[3], ctx->gpr[27], ctx->gpr[9]};\n'
    '        HazeJobTraceScope dispatch_trace(static_cast<unsigned>(HazeJobTraceKind::Dispatch),\n'
    '            ctx->thread_id, ctx->gpr[2], ctx->lr, job_args, 3);\n'
    '        dispatch_trace.set_dispatch(ctx->ctr, ctx->gpr[2]);\n')
DISPATCH_END = guarded('        }\n')


def patch(source):
    bodies = {}
    for address, digest in DIGESTS.items():
        start = source.index(f'void func_{address}(ppu_context* ctx) {{')
        end = source.index('\nvoid ', start + 10)
        body = source[start:end]
        if hashlib.sha256(body.encode()).hexdigest() != digest:
            raise ValueError(f'Haze job trace boundary {address} changed; re-audit before profiling')
        bodies[address] = body
    for address, body in bodies.items():
        changed = body.replace('{\n', '{\n' + entry(address), 1)
        if address in BASE_CALLS:
            call = BASE_CALLS[address]
            assert changed.count(call) == 1
            changed = changed.replace(call, call + BASE, 1)
        if address == '00682648':
            # Both successful allocation paths converge here, before r28 is reused.
            marker = 'loc_00682740:\n'
            assert changed.count(marker) == 1 and changed.count(CALL) == 1
            changed = changed.replace(marker, marker + SLOT, 1)
            # Mode 3 has completed its table lookup on both arrivals here.
            marker = 'loc_0068274C:\n'
            assert changed.count(marker) == 1
            changed = changed.replace(marker, marker + ROUTE, 1)
            changed = changed.replace(CALL, DISPATCH_BEGIN + CALL + '\n' + DISPATCH_END.rstrip('\n'), 1)
        source = source.replace(body, changed, 1)
    return INCLUDE + source
