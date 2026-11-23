"""Drop guest-stack spills of callee-saved GPRs that no lifted code reads.

The lifter restores each callee-saved register it recognizes from a host copy
taken at entry (`_cs_N`), so the prologue's store of that register to the guest
stack is usually write-only. These stores are about a third of all 64-bit
guest writes. Some lifted functions share a frame with the code that saved it
(tail entries and shared epilogues reload a register from its slot), so the
whole lift is scanned first: a (register, stack offset) pair that any function
reloads from the stack pointer stays written everywhere. A store is dropped
only if it is a prologue save as the lifter recognizes one (the slot's only
write, before the register changes, address never taken) and, in its own
function, the register is restored only from `_cs_N`, never reloaded from
memory, and its slot is never read.
"""
from pathlib import Path
import re

FUNCTION = re.compile(r'^void func_[0-9A-F]{8}\(ppu_context\* ctx\) \{\n.*?^\}\n', re.M | re.S)
SAVE = re.compile(r'vm_write64\(ctx->gpr\[1\] \+ (-?0x[0-9A-F]+|-?\d+), ctx->gpr\[(\d+)\]\);')
RELOAD = re.compile(r'(?:ctx->gpr\[|uint64_t _cs_)(\d+)\]? = vm_read64\(ctx->gpr\[1\] \+ (-?0x[0-9A-F]+|-?\d+)\);')


def reloaded_slots(sources):
    """(register, offset) pairs any lifted function reloads from the stack pointer."""
    slots = set()
    for source in sources:
        slots.update((int(register), offset) for register, offset in RELOAD.findall(source)
                     if 14 <= int(register) <= 31)
    return slots


def lift_slots(lift):
    return reloaded_slots(path.read_bytes().decode('latin1') for path in sorted(Path(lift).glob('ppu_recomp_*.cpp')))


def removable(body, start, register, offset, shared):
    """A prologue save, as the lifter recognizes one: the slot's only write, made
    before the register changes, at an offset whose address is never taken."""
    if not (14 <= register <= 31 and (register, offset) not in shared
            and f'ctx->gpr[{register}] = _cs_{register};' in body
            and f'ctx->gpr[{register}] = vm_read64(' not in body
            and f'vm_read64(ctx->gpr[1] + {offset})' not in body):
        return False
    if len(re.findall(r'vm_write\d+\(ctx->gpr\[1\] \+ ' + re.escape(offset) + ',', body)) != 1:
        return False
    if re.search(r'= ctx->gpr\[1\] \+ \(int64_t\)\(' + re.escape(offset) + r'\);', body):
        return False
    changed = re.search(r'ctx->gpr\[' + str(register) + r'\] (?:[-+|&^]|<<|>>)?= ', body)
    return not changed or changed.start() > start


def rewrite(body, shared):
    lines = body.split('\n')
    position = 0
    for index, line in enumerate(lines):
        match = SAVE.search(line)
        if match and removable(body, position + match.start(), int(match.group(2)), match.group(1), shared):
            stripped = line.replace(match.group(0), '', 1)
            lines[index] = None if not stripped.strip() else stripped
        position += len(line) + 1
    return '\n'.join(line for line in lines if line is not None)


def patch(source, shared):
    if '/* haze dead spills */' in source:
        raise ValueError('Dead spills already removed')
    # Earlier Haze transforms of this module may add reloads of their own.
    shared = shared | reloaded_slots([source])
    result = FUNCTION.sub(lambda match: rewrite(match.group(0), shared), source)
    return '/* haze dead spills */\n' + result
