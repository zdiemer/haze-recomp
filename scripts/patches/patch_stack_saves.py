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

A save the function reloads itself is promoted to a host local instead: an
entry-block save of r14-r31 or f14-f31 below the entry stack pointer, or of the
link register (`mflr r0`) to its ABI slot at entry +0x10. With the stack depth
of every line known from the gotos, the slot must be written only by the save
and read only as whole 64-bit reloads, which then read the local. The frame
rules above apply too. A guest back-chain walk no longer sees these slots, so
return addresses it reports (PPU_NULL_READ_STACK) can be stale.
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
FPR_SAVE = re.compile(r'^\s*\{ uint64_t tmp; memcpy\(&tmp, &ctx->fpr\[(\d+)\], 8\); vm_write64\(' + SP +
                      r' \+ (' + NUM + r'), tmp\); \}$')
FPR_DEFINE = re.compile(r'ctx->fpr\[(\d+)\]\s*=(?!=)|memcpy\(&ctx->fpr\[(\d+)\]')
DEFINE = re.compile(r'ctx->gpr\[(\d+)\]\s*(?:[-+|&^]|<<|>>)?=(?!=)')
# A label, branch, call or return ends the entry block.
ENTRY_END = re.compile(r'^\s*\w+:$|\bgoto\b|\bif \(|\breturn\b|\bswitch\b|ctx->lr = |DRAIN_TRAMPOLINE|g_trampoline_fn')


class Frame:
    def __init__(self, body):
        self.body = body
        self.entry_saves = []
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
        fpr_defined = set()
        lr_in_r0 = False
        for index, line in enumerate(body):
            if entry and ENTRY_END.search(line):
                entry = False
            if entry:
                fpr_save = FPR_SAVE.match(line)
                if fpr_save and 14 <= int(fpr_save.group(1)) <= 31 and \
                        int(fpr_save.group(1)) not in fpr_defined:
                    self.entry_saves.append((index, int(fpr_save.group(2), 0), 'fpr'))
                fpr_defined.update(int(a or b) for a, b in FPR_DEFINE.findall(line))
            # mflr r0 in the entry block; the LR save stores r0 until it changes.
            if line.strip() == 'ctx->gpr[0] = ctx->lr;':
                lr_in_r0 = entry
            elif '0' in DEFINE.findall(line):
                lr_in_r0 = False
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
                if 14 <= int(save.group(2)) <= 31:
                    self.entry_saves.append((index, int(save.group(1), 0), 'gpr'))
            elif save and entry and save.group(2) == '0' and lr_in_r0:
                self.entry_saves.append((index, int(save.group(1), 0), 'lr'))
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

    def promotable(self, unsafe_tails):
        """Return (save, reload lines, depths) for saves read only as whole reloads.

        The slot's address is fixed relative to the entry stack pointer, so with
        the stack depth of every line known, each access to it is found exactly.
        A slot written only by its save and read only as a whole is a host local.
        """
        if self.opaque or self.derived_load or self.tails & unsafe_tails or \
                len(self.sizes) != 1:
            return []
        size = next(iter(self.sizes))
        depth = line_depths(self.body, size)
        if depth is None:
            return []
        writes, reads = [], []
        for index, line in enumerate(self.body):
            if 'ctx->gpr[1]' not in line:
                continue
            if depth[index] is None:
                return []
            base = depth[index]
            for stdu in STDU.finditer(line):
                writes.append((index, base + int(stdu.group(1), 0), 8))
            rest = STDU.sub('', line)
            for access in ACCESS.finditer(rest):
                entry = (index, base + int(access.group(3), 0), int(access.group(2)) // 8)
                (reads if access.group(1) == 'read' else writes).append(entry)
        found = []
        for save in self.entry_saves:
            index, offset, kind = save
            slot = depth[index] + offset
            if kind == 'lr' and slot != 16 or kind != 'lr' and not -0x200 <= slot < 0:
                continue
            if any(start < slot + 8 and start + width > slot and at != index
                   for at, start, width in writes):
                continue
            touching = [(at, start, width) for at, start, width in reads
                        if start < slot + 8 and start + width > slot]
            if not touching or any(start != slot or width != 8 for _, start, width in touching):
                continue
            lines = []
            for at, _, _ in touching:
                text = self.body[at]
                load = f'vm_read64(ctx->gpr[1] + {fmt(slot - depth[at])})'
                if text.count('ctx->gpr[1]') != 1 or load not in text:
                    break
                lines.append(at)
            else:
                found.append((save, lines, depth))
        return found

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


LABEL = re.compile(r'^\s*(\w+):$')
GOTO = re.compile(r'\bgoto (\w+);')


def fmt(offset):
    return f'-0x{-offset:X}' if offset < 0 else f'0x{offset:X}'


def line_depths(body, size):
    """Stack depth below the entry stack pointer at the start of each line.

    Lifted bodies hold one statement per line, with gotos to labels and
    single-line conditionals. Return None when a line is reached at two depths
    or the control flow is anything else.
    """
    labels = {}
    for index, line in enumerate(body):
        label = LABEL.match(line)
        if label:
            labels[label.group(1)] = index
        if line.rstrip().endswith('{') or line.strip() == '}':
            return None
    depth = [None] * len(body)
    depth[0] = 0
    work = [0]
    while work:
        index = work.pop()
        line = body[index].strip()
        after = depth[index]
        conditional = line.startswith('if ')
        push, pop = STDU.search(line), FRAME_POP.search(line)
        if (push or pop) and conditional or \
                '1' in DEFINE.findall(line) and not (push or pop):
            return None
        if push:
            after -= size
        if pop:
            after += size
        successors = []
        for target in GOTO.findall(line):
            if target not in labels:
                return None
            successors.append(labels[target])
        terminal = not conditional and (line.startswith('goto ') or 'return;' in line)
        if not terminal and index + 1 < len(body):
            successors.append(index + 1)
        for successor in successors:
            if depth[successor] is None:
                depth[successor] = after
                work.append(successor)
            elif depth[successor] != after:
                return None
    return depth


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
        # A save reloaded only as a whole becomes a host local at both ends. The
        # entry block precedes every label and goto, so no jump skips the
        # declaration.
        for number, ((index, offset, kind), reads, depth) in enumerate(frame.promotable(unsafe)):
            line = lines[start + index]
            indent = line[:len(line) - len(line.lstrip())]
            local = f'_ss_{number}'
            if kind == 'fpr':
                register = FPR_SAVE.match(line).group(1)
                saved = f'uint64_t {local}; memcpy(&{local}, &ctx->fpr[{register}], 8);'
            else:
                saved = f'uint64_t {local} = ctx->gpr[{SAVE.match(line).group(2)}];'
            if poison:
                saved += f' vm_write64(ctx->gpr[1] + {fmt(offset)}, 0x{POISON:X}ull);'
            lines[start + index] = indent + saved
            slot = depth[index] + offset
            for at in reads:
                load = f'vm_read64(ctx->gpr[1] + {fmt(slot - depth[at])})'
                lines[start + at] = lines[start + at].replace(load, local)
            dropped += 1
    return {key: '\n'.join(lines) for key, lines in modules.items()}, dropped
