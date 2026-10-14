"""Route the audited SDK save implementation through Haze's Audio transaction."""
import hashlib
from pathlib import Path

AUDITED_SHA256 = "81998318af0b82a09074e97de287fd0e6e60bdb243f7600073b198fe554feb1f"


def patch(source: Path, port: Path) -> str:
    text = source.read_text(encoding="utf-8")
    if hashlib.sha256(text.encode()).hexdigest() != AUDITED_SHA256:
        raise ValueError("Save-data SDK changed: audit the Haze Audio transaction before building")
    for name, result in (("process_file_op", "s32"), ("write_param_sfo", "void"), ("savedata_execute", "s32")):
        old = f"static {result} {name}("
        if text.count(old) != 1:
            raise ValueError(f"Unexpected definition count for {name}")
        text = text.replace(old, f"static {result} {name}_original(", 1)
    for relative in ("cellSaveData.h", "../../runtime/ppu/ppu_memory.h", "../../runtime/memory/vm.h", "../../runtime/ppu/ppu_context.h"):
        text = text.replace(f'"{relative}"', f'"{(source.parent / relative).resolve().as_posix()}"')
    preamble = f'''#include "{(source.parent / 'cellSaveData.h').resolve().as_posix()}"
#include "{(port / 'haze_profile_transaction.h').resolve().as_posix()}"
static s32 process_file_op(const char*,CellSaveDataFileSet*);
static void write_param_sfo(const char*,const CellSaveDataSystemFileParam*);
static s32 savedata_execute(const char*,int,CellSaveDataSetBuf*,CellSaveDataStatCallback,CellSaveDataFileCallback,void*);
'''
    return preamble + text + f'\n#include "{(port / "haze_savedata_hooks.h").resolve().as_posix()}"\n'
