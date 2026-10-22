# netshutter-*.png (rsvg-convert로 SVG에서 크기별 렌더) → netshutter.ico
# 64px 이하는 32비트 BMP(DIB), 128·256은 PNG 그대로 담는다.
import struct, sys, os
from PIL import Image

SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]
here = os.path.dirname(os.path.abspath(__file__))

def dib(png_path, s):
    im = Image.open(png_path).convert("RGBA")
    assert im.size == (s, s)
    px = im.tobytes("raw", "BGRA")
    rows = [px[y * s * 4:(y + 1) * s * 4] for y in range(s)]
    xor = b"".join(reversed(rows))                     # 아래→위 순서
    mask_row = ((s + 31) // 32) * 4
    andm = b"\x00" * (mask_row * s)                     # 알파 채널 사용, AND 마스크는 0
    hdr = struct.pack("<IiiHHIIiiII", 40, s, s * 2, 1, 32, 0, len(xor) + len(andm), 0, 0, 0, 0)
    return hdr + xor + andm

entries = []
for s in SIZES:
    p = os.path.join(here, "png", f"netshutter-{s}.png")
    data = open(p, "rb").read() if s >= 128 else dib(p, s)
    entries.append((s, data))

out = struct.pack("<HHH", 0, 1, len(entries))
off = 6 + 16 * len(entries)
body = b""
for s, data in entries:
    out += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(data), off)
    off += len(data)
    body += data
open(os.path.join(here, "netshutter.ico"), "wb").write(out + body)
print("ok", [s for s, _ in entries])
