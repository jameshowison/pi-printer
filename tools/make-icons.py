#!/usr/bin/env python3
"""Convert a JPEG to three PNG icon sizes using GhostScript with inline PS."""

import os
import subprocess
import sys
import struct
import tempfile

def jpeg_dimensions(path):
    """Read JPEG width/height from SOF marker."""
    with open(path, 'rb') as f:
        data = f.read()
    i = 0
    while i < len(data) - 1:
        if data[i] != 0xFF:
            break
        marker = data[i+1]
        if marker in (0xC0, 0xC1, 0xC2):  # SOF0, SOF1, SOF2
            h = struct.unpack('>H', data[i+4:i+6])[0]
            w = struct.unpack('>H', data[i+6:i+8])[0]
            return w, h
        if marker in (0xD8, 0xD9):
            i += 2
        else:
            length = struct.unpack('>H', data[i+2:i+4])[0]
            i += 2 + length
    return None, None

def make_icon(src_jpg, out_png, size):
    src_w, src_h = jpeg_dimensions(src_jpg)
    if src_w is None:
        print(f"ERROR: could not read JPEG dimensions from {src_jpg}", file=sys.stderr)
        sys.exit(1)

    with open(src_jpg, 'rb') as f:
        jpg_data = f.read()

    # Build PostScript that embeds the JPEG inline as DCTDecode
    hex_data = jpg_data.hex()

    ps = f"""%!PS-Adobe-3.0
<< /PageSize [{size} {size}] >> setpagedevice

% Scale to fill the square, centred
/src_w {src_w} def
/src_h {src_h} def
/out {size} def

% Scale: fit largest dimension
/scale_w out src_w div def
/scale_h out src_h div def
/scale scale_w scale_h lt {{ scale_w }} {{ scale_h }} ifelse def

/draw_w src_w scale mul def
/draw_h src_h scale mul def
/off_x out draw_w sub 2 div def
/off_y out draw_h sub 2 div def

% White background
1 setgray
0 0 out out rectfill

off_x off_y translate
draw_w draw_h scale

% Inline JPEG via DCTDecode
/DeviceRGB setcolorspace
<<
  /ImageType 1
  /Width {src_w}
  /Height {src_h}
  /BitsPerComponent 8
  /Decode [0 1 0 1 0 1]
  /ImageMatrix [{src_w} 0 0 -{src_h} 0 {src_h}]
  /DataSource
  currentfile /ASCIIHexDecode filter /DCTDecode filter
>> image
{hex_data}>
showpage
"""

    with tempfile.NamedTemporaryFile(suffix='.ps', mode='w', delete=False) as tf:
        tf.write(ps)
        ps_path = tf.name

    try:
        result = subprocess.run(
            ['gs', '-dNOPAUSE', '-dBATCH', '-sDEVICE=png16m',
             '-dFIXEDMEDIA', f'-g{size}x{size}',
             f'-sOutputFile={out_png}', ps_path],
            capture_output=True, text=True
        )
        if result.returncode != 0 or not os.path.exists(out_png):
            print(f"GS error for {size}x{size}:\n{result.stderr}", file=sys.stderr)
            sys.exit(1)
        print(f"  icon-{size}.png  ({os.path.getsize(out_png)} bytes)")
    finally:
        os.unlink(ps_path)

if __name__ == '__main__':
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    src = os.path.join(repo, 'Brother-HL-5170dn-image.jpg')
    out_dir = os.path.join(repo, 'icons')
    os.makedirs(out_dir, exist_ok=True)

    print(f"Source: {src}")
    for size in [48, 128, 512]:
        make_icon(src, os.path.join(out_dir, f'icon-{size}.png'), size)
    print("Done.")
