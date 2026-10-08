#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The RG Nano's screen to a PNG, over adb (plorpos-ggv).

    tools/nano-shot.py out.png

/dev/fb0 there (fb_st7789v) reads back as one 240x240 RGB565 page whatever
fbset says about 240x720, and it is what the panel shows: hello's colour bars
came back as exactly the values drawn. FunKey's adbd has no exec-out, so the
frame is copied to /tmp on the device and pulled. Plain zlib, no PIL."""
import os, struct, subprocess, sys, tempfile, zlib

W = H = 240

def main(out):
    subprocess.run(['adb', 'shell', 'cp /dev/fb0 /tmp/nanoshot.raw'], check=True, timeout=15)
    with tempfile.TemporaryDirectory() as d:
        raw = os.path.join(d, 'fb.raw')
        subprocess.run(['adb', 'pull', '/tmp/nanoshot.raw', raw], check=True, timeout=15,
                       stdout=subprocess.DEVNULL)
        b = open(raw, 'rb').read()[:W * H * 2]
    rows = []
    for y in range(H):
        r = bytearray(b'\0')
        for x in range(W):
            v = b[(y * W + x) * 2] | b[(y * W + x) * 2 + 1] << 8
            r += bytes(((v >> 11 & 31) * 255 // 31, (v >> 5 & 63) * 255 // 63, (v & 31) * 255 // 31))
        rows.append(bytes(r))

    def chunk(t, data):
        c = t + data
        return struct.pack('>I', len(data)) + c + struct.pack('>I', zlib.crc32(c))
    with open(out, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0))
                + chunk(b'IDAT', zlib.compress(b''.join(rows))) + chunk(b'IEND', b''))

if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit('usage: nano-shot.py out.png')
    main(sys.argv[1])
