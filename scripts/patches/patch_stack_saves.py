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

Where the gotos allow it, a forward pass places every access exactly instead:
it tracks the stack depth of each line and which registers hold the stack
pointer plus a known offset (`addi r11,r1,K`, the back chain this frame pushed,
`mr`) or a constant (`li r0,K` indexing the vector save area). Loads through
those registers are then ordinary frame accesses, and the derived-register
rule above applies only to stack addresses the pass cannot place. Calls
clobber the volatile registers; a stack address loaded from memory is assumed
not to reach the save area, as epilogues restore through r1, the back chain or
an `addi` from r1.

A save the function reloads itself is promoted to a host local instead: an
entry-block save of r14-r31 or f14-f31 below the entry stack pointer, or of the
link register (`mflr r0`) to its ABI slot at entry +0x10. With every access
placed, the slot must be written only by the save and read only as whole
64-bit reloads, which then read the local. The frame and tail-branch rules
above apply too. A guest back-chain walk no longer sees these slots, so return
addresses it reports (PPU_NULL_READ_STACK) can be stale.
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
        self.indexed_vector = False
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
            # A vector access at the stack pointer plus an index register (the
            # vector-register save area) needs the index's value: layout().
            if STACK_VECTOR.search(rest):
                self.indexed_vector = True
                rest = STACK_VECTOR.sub('', rest)
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
        # The frame may be read through a register whose stack offset only the
        # flow-sensitive layout() knows.
        self.derived_load = self.indexed_vector
        if derived and not self.derived_load:
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
        frame = self._layout(unsafe_tails)
        if frame is None:
            return []
        depth, reads, writes = frame
        found = []
        for save in self.entry_saves:
            index, offset, kind = save
            slot = depth[index] + offset
            if kind == 'lr' and slot != 16 or kind != 'lr' and not -0x200 <= slot < 0:
                continue
            if any(start < slot + 8 and start + width > slot and at != index
                   for at, start, width, _ in writes):
                continue
            touching = [read for read in reads if read[1] < slot + 8 and read[1] + read[2] > slot]
            if not touching or any(start != slot or width != 8 for _, start, width, _ in touching):
                continue
            # Each reload is the line's only memory access, so replacing its text
            # replaces exactly that load.
            lines = []
            for at, _, _, load in touching:
                text = self.body[at]
                if len(MEMORY.findall(text)) != 1 or text.count(load) != 1:
                    break
                lines.append((at, load))
            else:
                found.append((save, lines))
        return found

    def _layout(self, unsafe_tails):
        if self.opaque or self.tails & unsafe_tails or len(self.sizes) != 1 or \
                not (self.saves or self.entry_saves):
            return None
        if not hasattr(self, 'placed'):
            self.placed = layout(self.body, next(iter(self.sizes)))
        return self.placed

    def elidable(self, unsafe_tails):
        frame = self._layout(unsafe_tails)
        if frame is not None:
            return self._elidable_exact(frame)
        # Without a layout, offsets are only known relative to the stack
        # pointer at one of its two depths.
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

    def _elidable_exact(self, frame):
        """Saves no access reaches, with every frame address placed by layout()."""
        depth, reads, writes = frame
        lines = []
        for index, offset, register in self.saves:
            if not 14 <= register <= 31 or register not in self.snapshots \
                    or register not in self.restores:
                continue
            slot = depth[index] + offset
            if not -0x200 <= slot < 0 or \
                    any(at != index and start < slot + 8 and start + width > slot
                        for at, start, width, _ in writes) or \
                    any(start < slot + 8 and start + width > slot for _, start, width, _ in reads):
                continue
            lines.append(index)
        return lines


LABEL = re.compile(r'^\s*(\w+):$')
GOTO = re.compile(r'\bgoto (\w+);')
BASE_ACCESS = re.compile(r'vm_(read|write)(8|16|32|64)\(ctx->gpr\[(\d+)\]\s*\+\s*(' + NUM + r')\s*([,)])')
VECTOR = re.compile(r'^\s*\{ uint64_t ea = \((.+?)\) & ~0xFULL; memcpy\((&ctx->vr\[\d+\]|vm_base \+ \(uint32_t\)ea), '
                    r'(?:&ctx->vr\[\d+\]|vm_base \+ \(uint32_t\)ea), 16\); \}$')
INDEX = re.compile(r'\(?ctx->gpr\[(\d+)\](?: \+ ctx->gpr\[(\d+)\])?\)?')
STACK_VECTOR = re.compile(r'\(\((?:' + SP + r' \+ ctx->gpr\[\d+\]|ctx->gpr\[\d+\] \+ ' + SP + r')\)\)(?= & ~0xFULL; memcpy)')
LOAD = re.compile(r'vm_read(?:8|16|32|64)\([^()]*\)')
MEMORY = re.compile(r'vm_(?:read|write)(?:8|16|32|64)\(|vm_base')
REGISTER_COPY = re.compile(r'^ctx->gpr\[(\d+)\] \| ctx->gpr\[\1\]$')
ADD_CONSTANT = re.compile(r'^ctx->gpr\[(\d+)\] \+ \(int64_t\)\((' + NUM + r')\)$')
ADD_REGISTER = re.compile(r'^ctx->gpr\[(\d+)\] \+ ctx->gpr\[(\d+)\]$')
CONSTANT = re.compile(r'^(?:\(int64_t\)\(int32_t\)\((' + NUM + r')\)|(' + NUM + r')ULL)$')
BACK_CHAIN_LOAD = re.compile(r'^vm_read64\(ctx->gpr\[(\d+)\] \+ (' + NUM + r')\)$')
CALL = re.compile(r'ctx->lr = 0x|lv2_syscall\(ctx\)|ps3_hle_call')
VOLATILE = (0,) + tuple(range(3, 13))
UNKNOWN = ('u',)


def fmt(offset):
    return f'-0x{-offset:X}' if offset < 0 else f'0x{offset:X}'


# Register values for layout(): ('s', offset) is the entry stack pointer plus
# offset, ('c', value) a constant, UNKNOWN may address the stack, and a
# register without an entry holds no stack address.
def _stacky(value):
    return value is not None and value[0] != 'c'


def _add(a, b):
    if a is None or b is None:
        return UNKNOWN if _stacky(a) or _stacky(b) else None
    if a[0] == 'c' and b[0] == 'c':
        return ('c', a[1] + b[1])
    if a[0] == 'c' and b[0] == 's' or a[0] == 's' and b[0] == 'c':
        return ('s', a[1] + b[1])
    return UNKNOWN


def _join(a, b):
    if a[0] != b[0]:
        return None
    regs = {}
    for register in a[1].keys() | b[1].keys():
        x, y = a[1].get(register), b[1].get(register)
        if x == y:
            regs[register] = x
        elif _stacky(x) or _stacky(y):
            regs[register] = UNKNOWN
    return a[0], regs


def _step(line, state, size):
    """Return the state after one line and the frame accesses it makes.

    None means the line reads the stack through an address it cannot place.
    """
    depth, regs = state

    def value(register):
        return ('s', depth) if register == 1 else regs.get(register)

    def mentions_stack(text):
        return any(_stacky(value(int(r))) for r in GPR.findall(text))

    accesses = []
    rest = line
    for access in BASE_ACCESS.finditer(line):
        address = _add(value(int(access.group(3))), ('c', int(access.group(4), 0)))
        width = int(access.group(2)) // 8
        if address == UNKNOWN and access.group(1) == 'read' and width == 8:
            return None
        if address is not None and address[0] == 's':
            accesses.append((access.group(1), address[1], width, access.group(0)))
    rest = BASE_ACCESS.sub('', rest)
    vector = VECTOR.match(line)
    if vector:
        index = INDEX.fullmatch(vector.group(1))
        if not index:
            return None
        address = value(int(index.group(1)))
        if index.group(2):
            address = _add(address, value(int(index.group(2))))
        kind = 'read' if vector.group(2).startswith('&') else 'write'
        if address == UNKNOWN and kind == 'read':
            return None
        if address is not None and address[0] == 's':
            if size % 16:
                return None
            accesses.append((kind, address[1] & ~15, 16, None))
        rest = ''
    # Any other 64-bit or vector load through a stack address.
    if ('vm_read64(' in rest or 'vm_base' in rest) and mentions_stack(rest):
        return None

    regs = dict(regs)
    stripped = line.strip()
    assignment = DERIVE.match(line)
    defined = [int(r) for r in DEFINE.findall(line)]
    if STDU.search(line):
        depth -= size
    elif FRAME_POP.search(line):
        depth += size
    elif REGISTER_POP.search(line):
        source = value(int(REGISTER_POP.search(line).group(1)))
        if source is None or source[0] != 's':
            return None
        depth = source[1]
    elif 1 in defined:
        return None
    if assignment and assignment.group(1) != '1':
        target, expr = int(assignment.group(1)), assignment.group(2)
        new = None
        if ADD_CONSTANT.match(expr):
            m = ADD_CONSTANT.match(expr)
            new = _add(value(int(m.group(1))), ('c', int(m.group(2), 0)))
        elif ADD_REGISTER.match(expr):
            m = ADD_REGISTER.match(expr)
            new = _add(value(int(m.group(1))), value(int(m.group(2))))
        elif REGISTER_COPY.match(expr):
            new = value(int(REGISTER_COPY.match(expr).group(1)))
        elif CONSTANT.match(expr):
            m = CONSTANT.match(expr)
            new = ('c', int(m.group(1) or m.group(2), 0))
        elif BACK_CHAIN_LOAD.match(expr):
            m = BACK_CHAIN_LOAD.match(expr)
            # The back chain this frame pushed holds the entry stack pointer; the
            # one at the stack pointer before the push holds a caller's.
            address = _add(value(int(m.group(1))), ('c', int(m.group(2), 0)))
            if address == ('s', -size):
                new = ('s', 0)
            elif address == UNKNOWN or address == ('s', depth):
                new = UNKNOWN
        elif mentions_stack(LOAD.sub('', expr)):
            new = UNKNOWN
        defined = [r for r in defined if r != target]
        if new is None:
            regs.pop(target, None)
        else:
            regs[target] = new
    other = UNKNOWN if mentions_stack(LOAD.sub('', line)) else None
    for register in defined:
        if register != 1:
            if other is None:
                regs.pop(register, None)
            else:
                regs[register] = other
    if CALL.search(line):
        for register in VOLATILE:
            regs.pop(register, None)
    after = (depth, regs)
    if stripped.startswith('if '):
        if after[0] != state[0]:
            return None
        after = _join(state, after)
    return after, accesses


def layout(body, size):
    """Place every access to this frame relative to the entry stack pointer.

    Lifted bodies hold one statement per line, with gotos to labels and
    single-line conditionals. A forward pass over that control flow tracks the
    stack depth and which registers hold a known stack address or constant;
    calls clobber the volatile ones. Return (depth per line, reads, writes),
    each access (line, entry offset, width, load text), or None when a line is
    reached at two depths, a 64-bit or vector load uses a stack address that
    cannot be placed, or the control flow is anything else.
    """
    labels = {}
    for index, line in enumerate(body):
        label = LABEL.match(line)
        if label:
            labels[label.group(1)] = index
        if line.rstrip().endswith('{') or line.strip() == '}':
            return None
    states = [None] * len(body)
    states[0] = (0, {})
    work = [0]
    while work:
        index = work.pop()
        line = body[index].strip()
        stepped = _step(line, states[index], size)
        if stepped is None:
            return None
        after = stepped[0]
        conditional = line.startswith('if ')
        successors = []
        for target in GOTO.findall(line):
            if target not in labels:
                return None
            successors.append(labels[target])
        terminal = not conditional and (line.startswith('goto ') or 'return;' in line)
        if not terminal and index + 1 < len(body):
            successors.append(index + 1)
        for successor in successors:
            if states[successor] is None:
                states[successor] = after
                work.append(successor)
            else:
                joined = _join(states[successor], after)
                if joined is None:
                    return None
                if joined != states[successor]:
                    states[successor] = joined
                    work.append(successor)
    depth = [None if state is None else state[0] for state in states]
    reads, writes = [], []
    for index, line in enumerate(body):
        if states[index] is None:
            if 'ctx->gpr[1]' in line:
                return None
            continue
        stepped = _step(line.strip(), states[index], size)
        if stepped is None:
            return None
        for kind, start, width, text in stepped[1]:
            (reads if kind == 'read' else writes).append((index, start, width, text))
    return depth, reads, writes


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
        for number, ((index, offset, kind), reads) in enumerate(frame.promotable(unsafe)):
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
            for at, load in reads:
                lines[start + at] = lines[start + at].replace(load, local)
            dropped += 1
    return {key: '\n'.join(lines) for key, lines in modules.items()}, dropped
