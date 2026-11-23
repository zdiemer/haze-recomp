"""Route guest stack accesses in a lifted PPU module to the inline accessors.

Applies to the final copy of every module, after its audited patches. A call
whose address is the guest stack pointer plus a constant, such as
`vm_write64(ctx->gpr[1] + 0x70, ...)`, becomes `vm_stack_write64(...)` from the
SDK's ppu_stack_access.h, which accesses ordinary RAM inline and otherwise calls
the original accessor. Nothing else in the module changes.
"""
import re

INCLUDE = '#include "ppu_recomp.h"\n'
STACK_CALL = re.compile(
    r'\bvm_(read|write)(8|16|32|64)\((?=ctx->gpr\[1\] \+ -?(?:0x[0-9A-Fa-f]+|[0-9]+)[,)])')


def patch(source):
    if source.count(INCLUDE) < 1:
        raise ValueError('lifted module no longer includes ppu_recomp.h; audit the stack-access patch')
    if 'vm_stack_' in source:
        raise ValueError('lifted module already uses vm_stack_ names')
    source, sites = STACK_CALL.subn(r'vm_stack_\1\2(', source)
    if not sites:
        return source
    return source.replace(INCLUDE, INCLUDE + '#include "ppu_stack_access.h"\n', 1)
