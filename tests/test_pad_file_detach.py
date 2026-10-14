"""Exercise PAD_FILE detach using the command block extracted from cellPad.c."""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
BEGIN = "    /* BEGIN PAD_FILE COMMAND BLOCK */"
END = "    /* END PAD_FILE COMMAND BLOCK */"


def extract_block(source: str) -> str:
    if source.count(BEGIN) != 1 or source.count(END) != 1:
        raise RuntimeError("expected unique PAD_FILE extraction markers")
    start = source.index(BEGIN)
    finish = source.index(END, start + len(BEGIN))
    if finish <= start:
        raise RuntimeError("invalid PAD_FILE extraction marker order")
    return source[start + len(BEGIN):finish]


def fixture_source(block: str) -> str:
    return f'''#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned char u8;
typedef unsigned short u16;
typedef struct {{ u16 button[8]; }} CellPadData;
typedef struct {{ unsigned char analog_lx, analog_ly, analog_rx, analog_ry; }} PadHostState;
enum {{ CELL_PAD_BTN_OFFSET_DIGITAL1=0, CELL_PAD_BTN_OFFSET_DIGITAL2=1 }};
static unsigned fopen_calls;
static FILE* tracked_fopen(const char* path, const char* mode)
{{ fopen_calls++; return fopen(path, mode); }}
static void write_command(const char* path, const char* command)
{{ FILE* f=fopen(path,"wb"); if(!f) abort(); fputs(command,f); fclose(f); }}

static void poll_pad_file(unsigned port_no, CellPadData* data, PadHostState* hs)
{{
#define fopen tracked_fopen
{block}
#undef fopen
}}

int main(int argc, char** argv)
{{
    if(argc!=2) return 2;
    const char* path=argv[1];
    _putenv_s("PAD_FILE",path);
    CellPadData data={{0}}; PadHostState hs={{128,128,128,128}};
    write_command(path,"detach junk\\n");
    poll_pad_file(0,&data,&hs);
    if(fopen_calls!=1) return 3;
    poll_pad_file(0,&data,&hs); /* trailing junk must not detach */
    if(fopen_calls!=2) return 4;

    write_command(path,"sticks 11 22 33 44 2\\n");
    poll_pad_file(0,&data,&hs);
    if(fopen_calls!=4 || hs.analog_lx!=11 || hs.analog_ly!=22 ||
       hs.analog_rx!=33 || hs.analog_ry!=44) return 5;
    poll_pad_file(0,&data,&hs);
    if(fopen_calls!=4 || hs.analog_lx!=11 || hs.analog_ly!=22 ||
       hs.analog_rx!=33 || hs.analog_ry!=44) return 6;

    write_command(path,"0x0008 2\\n");
    memset(&data,0,sizeof(data));
    poll_pad_file(0,&data,&hs);
    if(fopen_calls!=6 || data.button[0]!=8) return 7;
    memset(&data,0,sizeof(data));
    poll_pad_file(0,&data,&hs); /* held input consumes its second poll */
    if(fopen_calls!=6 || data.button[0]!=8) return 8;

    write_command(path," \\t detach \\r\\n");
    memset(&data,0,sizeof(data));
    poll_pad_file(0,&data,&hs);
    if(fopen_calls!=7 || data.button[0]!=0) return 9;

    /* Once detached, both a missing file and a later command are ignored. The
       caller's controller/keyboard state is left untouched by this block. */
    if(remove(path)!=0) return 10;
    data.button[0]=0x20; data.button[1]=0x40;
    hs.analog_lx=17; hs.analog_ly=19; hs.analog_rx=23; hs.analog_ry=29;
    poll_pad_file(0,&data,&hs);
    write_command(path,"0x4000 40\\n");
    poll_pad_file(0,&data,&hs);
    if(fopen_calls!=7 || data.button[0]!=0x20 || data.button[1]!=0x40 ||
       hs.analog_lx!=17 || hs.analog_ly!=19 || hs.analog_rx!=23 || hs.analog_ry!=29)
        return 11;
    remove(path);
    return 0;
}}
'''


def compile_command(compiler: str, source: Path, exe: Path) -> list[str]:
    name = Path(compiler).name.lower()
    if name in ("clang-cl", "clang-cl.exe"):
        return [compiler, "/nologo", "/TC", "/std:c11", "/O2", "/W4",
                "/wd4100", "/D_WIN32", str(source), f"/Fe:{exe}",
                f"/Fo:{exe.with_suffix('.obj')}"]
    return [compiler, "-std=c11", "-O2", "-Wall", "-Wextra", "-D_WIN32",
            str(source), "-o", str(exe)]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--sdk", type=Path, default=ROOT / "external/ps3recomp")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "local/test-pad-file-detach")
    args = parser.parse_args()
    source_bytes = (args.sdk / "libs/input/cellPad.c").read_bytes()
    block = extract_block(source_bytes.decode("utf-8"))
    if 'printf("[cellPad] PAD_FILE detached\\n"); fflush(stdout);' not in block:
        raise RuntimeError("detach must acknowledge and flush after disabling PAD_FILE")
    if "held_mask = 0; held_n = 0;" not in block or \
       "memset(held_sticks, 0, sizeof held_sticks); stick_n = 0;" not in block:
        raise RuntimeError("detach must clear held button and stick state")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    fixture = args.output_dir / "pad_file_detach_fixture.c"
    fixture.write_text(fixture_source(block), encoding="utf-8")
    exe = args.output_dir / "pad_file_detach_fixture.exe"
    built = subprocess.run(compile_command(args.compiler, fixture, exe), capture_output=True, text=True)
    if built.returncode:
        raise RuntimeError(f"compile failed ({built.returncode})\\n{built.stdout}\\n{built.stderr}")
    with tempfile.TemporaryDirectory(prefix="haze-pad-file-detach-") as temp:
        path = Path(temp) / "pad.txt"
        result = subprocess.run([str(exe), str(path)], capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(f"fixture failed ({result.returncode})\\n{result.stdout}\\n{result.stderr}")
        if result.stdout.count("[cellPad] PAD_FILE detached\n") != 1:
            raise AssertionError(f"expected one flushed detach acknowledgement: {result.stdout!r}")
    print(f"SDK source sha256: {hashlib.sha256(source_bytes).hexdigest()}")
    print("PASS: exact detach parsing, ordinary command hold, no post-detach file opens, caller state preserved")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
