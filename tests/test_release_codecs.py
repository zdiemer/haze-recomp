"""Validate the actual release DLL exports before compiling or packaging the game."""
import ctypes
import os
from pathlib import Path
import sys

root = Path(sys.argv[1]).resolve() / 'bin'
with os.add_dll_directory(str(root)):
    library = ctypes.CDLL(str(next(root.glob('avcodec-*.dll'))))
    library.avcodec_find_decoder_by_name.argtypes = [ctypes.c_char_p]
    library.avcodec_find_decoder_by_name.restype = ctypes.c_void_p
    for codec in (b'mpeg2video', b'atrac3plus'):
        if not library.avcodec_find_decoder_by_name(codec):
            raise SystemExit('Release DLL lacks required movie decoder: ' + codec.decode())
    library.avcodec_license.restype = ctypes.c_char_p
    if b'LGPL' not in library.avcodec_license():
        raise SystemExit('Unexpected FFmpeg binary license')
print('Release DLLs expose both MPEG-2 and ATRAC3+ decoders under LGPL.')
