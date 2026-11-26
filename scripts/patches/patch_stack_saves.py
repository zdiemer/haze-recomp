"""Drop callee-save spills that nothing reads from the lifted PPU modules.

The lifter already restores a callee-saved register from a host copy taken at
entry (`ctx->gpr[30] = _cs_30;`), yet it still stores the register to its guest
stack slot. When nothing can read that slot again the store is dead, and it is
the most frequent out-of-line guest memory call in hot producer code.

The transform runs once over the final copy of every module, after their
audited patches, because a slot may be read by another lifted function: a tail
branch continuing the same guest frame (`g_trampoline_fn = func_X`) restores
registers from memory. A store `vm_write64(ctx->gpr[1] + off, ctx->gpr[r]);`
of r14-r31 is dropped only when all of the following hold:

- the store is in the straight-line entry block before r is redefined, so it
  saves the caller's value rather than passing a stack argument (a varargs
  callee reads those from the caller's frame);
- the function snapshots and restores r from the host copy, and writes `off`
  once;
- its stack pointer changes only by one constant frame size, and it never
  computes a stack address from a register index or stores the stack pointer
  elsewhere;
- no read of its own frame overlaps the slot at any of its stack depths, and no
  64-bit or vector load goes through a register derived from the stack pointer
  (frame-pointer and back-chain epilogues restore through one);
- no tail branch leaves it for a function that reads a frame it did not write,
  directly or through further tail branches, or for an unknown target.

Ordinary calls cannot read a caller's save area under the PPC64 ABI, and the
game has no unwinder that walks saved registers.
"""
import re

FUNC = re.compile(r'^void (func_[0-9A-F]+)\(ppu_context\* ctx\) \{$')
NUM = r'-?(?:0x[0-9A-Fa-f]+|\d+)'
SP = r'ctx->gpr\[1\]'
SAVE = re.compile(r'^\s*vm_write64\(' + SP + r' \+ (' + NUM + r'), ctx->gpr\[(\d+)\]\);$')
STDU = re.compile(r'vm_write64\(' + SP + r' \+ (' + NUM + r'), ' + SP + r'\); ' + SP + r' \+= (' + NUM + r');')
FRAME_POP = re.compile(SP + r' = ' + SP + r' \+ \(int64_t\)\((' + NUM + r')\);')
REGISTER_POP = re.compile(SP + r' = ctx->gpr\[(\d+)\] \| ctx->gpr\[\1\];')
ACCESS = re.compile(r'vm_(read|write)(8|16|32|64)\(' + SP + r'\s*\+\s*(' + NUM + r')\s*[,)]')
DERIVE = re.compile(r'^\s*ctx->gpr\[(\d+)\] = (.*);$')
# The stack pointer other than as the base of a load at a non-zero offset.
FRAME_ADDRESS = re.compile(SP + r'(?!\s*\+\s*-?0x0*[1-9A-Fa-f])')
LOAD64 = re.compile(r'vm_read64\(ctx->gpr\[(\d+)\]')
GPR = re.compile(r'ctx->gpr\[(\d+)\]')
TAIL = re.compile(r'g_trampoline_fn = \(void\(\*\)\(void\*\)\)(func_[0-9A-F]+);')
SNAPSHOT = re.compile(r'^\s*uint64_t _cs_(\d+) = ctx->gpr\[\1\];$')
MEMORY_SNAPSHOT = re.compile(r'^\s*uint64_t _cs_\d+ = vm_read64\(')
RESTORE = re.compile(r'ctx->gpr\[(\d+)\] = _cs_\1;')
DEFINE = re.compile(r'ctx->gpr\[(\d+)\]\s*(?:[-+|&^]|<<|>>)?=(?!=)')
# A label, branch, call or return ends the entry block.
ENTRY_END = re.compile(r'^\s*\w+:$|\bgoto\b|\bif \(|\breturn\b|\bswitch\b|ctx->lr = |DRAIN_TRAMPOLINE|g_trampoline_fn')


class Frame:
    def __init__(self, body):
        self.reads = []
        self.writes = {}
        self.saves = []
        self.sizes = set()
        self.tails = set()
        self.opaque = False
        self.snapshots = set()
        self.restores = set()
        derived = set()
        assignments = []
        entry = True
        defined = set()
        for index, line in enumerate(body):
            if entry and ENTRY_END.search(line):
                entry = False
            snapshot = SNAPSHOT.match(line)
            if snapshot:
                self.snapshots.add(int(snapshot.group(1)))
            if MEMORY_SNAPSHOT.match(line):
                self.opaque = True
            self.restores.update(int(r) for r in RESTORE.findall(line))
            self.tails.update(TAIL.findall(line))
            if 'ps3_indirect_call(ctx); return;' in line:
                self.opaque = True
            assignment = DERIVE.match(line)
            if assignment:
                assignments.append((int(assignment.group(1)), assignment.group(2)))
            if 'ctx->gpr[1]' not in line:
                defined.update(int(r) for r in DEFINE.findall(line))
                continue
            rest = line
            for stdu in STDU.finditer(line):
                self._write(int(stdu.group(1), 0), 8)
                self.sizes.add(abs(int(stdu.group(2), 0)))
            rest = STDU.sub('', rest)
            for pop in FRAME_POP.finditer(rest):
                self.sizes.add(abs(int(pop.group(1), 0)))
            rest = REGISTER_POP.sub('', FRAME_POP.sub('', rest))
            save = SAVE.match(line)
            # A save stores the caller's value: in the straight-line entry block,
            # before the register is redefined. The same store elsewhere passes
            # a stack argument, which the callee reads (varargs formatters do).
            if save and entry and int(save.group(2)) not in defined:
                self.saves.append((index, int(save.group(1), 0), int(save.group(2))))
            for access in ACCESS.finditer(rest):
                offset, width = int(access.group(3), 0), int(access.group(2)) // 8
                if access.group(1) == 'read':
                    self.reads.append((offset, width))
                else:
                    self._write(offset, width)
            rest = ACCESS.sub('', rest)
            # A register computed from the stack pointer, or loaded from the
            # back chain at offset 0, can address this frame or the caller's.
            if assignment and assignment.group(1) != '1' and \
                    FRAME_ADDRESS.search(assignment.group(2)):
                derived.add(int(assignment.group(1)))
                rest = rest.replace(assignment.group(2), '')
            # Anything else (indexed stack addresses, the stack pointer stored as
            # a value, other stack-pointer updates) makes the frame unknown.
            if 'ctx->gpr[1]' in rest:
                self.opaque = True
            defined.update(int(r) for r in DEFINE.findall(line))
        changed = True
        while changed:
            changed = False
            for target, value in assignments:
                if target != 1 and target not in derived and \
                        any(int(r) in derived for r in GPR.findall(value)):
                    derived.add(target)
                    changed = True
        self.derived_load = False
        if derived:
            for line in body:
                bases = {int(r) for r in LOAD64.findall(line)}
                if bases & derived or ('vm_base' in line and
                                       {int(r) for r in GPR.findall(line)} & derived):
                    self.derived_load = True
                    break
        if len(self.sizes) > 1:
            self.opaque = True
        self.reads_foreign = (self.opaque or self.derived_load or
                              any(offset not in self.writes for offset, _ in self.reads))

    def _write(self, offset, width):
        self.writes[offset] = self.writes.get(offset, 0) + 1

    def elidable(self, unsafe_tails):
        if self.opaque or self.derived_load or self.tails & unsafe_tails:
            return []
        size = next(iter(self.sizes), 0)
        lines = []
        for index, offset, register in self.saves:
            if not 14 <= register <= 31 or register not in self.snapshots \
                    or register not in self.restores or self.writes.get(offset) != 1:
                continue
            slots = {offset, offset + size, offset - size}
            if any(start < slot + 8 and start + width > slot
                   for start, width in self.reads for slot in slots):
                continue
            lines.append(index)
        return lines


def functions(lines):
    name = None
    for index, line in enumerate(lines):
        if name is None:
            match = FUNC.match(line)
            if match:
                name, start = match.group(1), index + 1
        elif line == '}':
            yield name, start, index
            name = None


POISON = 0x5AFE5AFE5AFE5AFE


def patch_modules(sources, poison=False):
    """Return the rewritten module texts and the number of stores dropped.

    With poison, each store writes a marker value instead, so a missed reader
    fails visibly rather than seeing the stale value from an earlier call.
    """
    modules = {key: text.split('\n') for key, text in sources.items()}
    frames = {}
    for key, lines in modules.items():
        for name, start, end in functions(lines):
            if name in frames:
                raise ValueError(f'{name} is defined twice; audit the stack-saves patch')
            frames[name] = (key, start, Frame(lines[start:end]))
    # A frame is unsafe to tail-branch into while the caller's frame is live if
    # it reads a slot it never wrote, or tail-branches on to such a frame.
    unsafe = {name for name, (_, _, frame) in frames.items() if frame.reads_foreign}
    unsafe.update(target for _, _, frame in frames.values()
                  for target in frame.tails if target not in frames)
    changed = True
    while changed:
        changed = False
        for name, (_, _, frame) in frames.items():
            if name not in unsafe and frame.tails & unsafe:
                unsafe.add(name)
                changed = True
    dropped = 0
    for key, start, frame in frames.values():
        lines = modules[key]
        for index in frame.elidable(unsafe):
            line = lines[start + index]
            if not SAVE.match(line):
                raise ValueError('stack-saves: analysed line moved')
            indent = line[:len(line) - len(line.lstrip())]
            if poison:
                offset = SAVE.match(line).group(1)
                lines[start + index] = f'{indent}vm_write64(ctx->gpr[1] + {offset}, 0x{POISON:X}ull);'
            else:
                lines[start + index] = indent + '/* callee save elided */;'
            dropped += 1
    return {key: '\n'.join(lines) for key, lines in modules.items()}, dropped
