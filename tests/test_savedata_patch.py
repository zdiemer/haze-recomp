"""Guard the SDK dependency and prove callers still route through the hooks."""
from pathlib import Path
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "scripts"))
from patches.patch_savedata import patch

source = Path(sys.argv[1])
original = source.read_bytes()
result = patch(source, root / "port")
for name, return_type in (("process_file_op", "s32"), ("write_param_sfo", "void"), ("savedata_execute", "s32")):
    assert result.count(f"static {return_type} {name}_original(") == 1
    assert result.count(f"{name}_original(") == 1  # no SDK caller bypasses the hooks
assert "s32 exc = process_file_op(save_path, &fileSet);" in result
assert "write_param_sfo(save_path, &params);" in result
assert '#include "../../' not in result
assert result.rstrip().endswith('haze_savedata_hooks.h"')
with tempfile.TemporaryDirectory() as directory:
    changed = Path(directory) / "cellSaveData.c"
    changed.write_bytes(original.replace(b"\r\n", b"\n"))
    assert "savedata_execute_original" in patch(changed, root / "port")
    changed.write_bytes(original + b"\n/* SDK changed */\n")
    try:
        patch(changed, root / "port")
    except ValueError as error:
        assert "audit" in str(error)
    else:
        raise AssertionError("Unaudited SDK accepted")
assert source.read_bytes() == original
print("Save-data patch: exact SDK, LF/CRLF, hook routing, mutation rejection and untouched source passed.")
