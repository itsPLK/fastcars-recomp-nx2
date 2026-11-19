#!/usr/bin/env python3
"""NRO icon from the player's own disc: the title thumbnail of Disc 1's nxeart package (an Xbox 360
STFS/PIRS file: PNG size at 0x1716, the image at 0x571A; the package thumbnail at 0x1712/0x171A as
a fallback), scaled to the 256x256 JPEG an NRO carries. Needs Pillow.

usage: make_icon.py NXEART OUT.jpg     (exits 1 and writes nothing when there is no usable image;
                                        without Pillow it keeps an existing OUT.jpg and exits 0)
"""
import io
import struct
import sys

try:
    from PIL import Image
except ImportError:  # e.g. the dev image: keep the icon made earlier (or none)
    Image = None


def thumbnail(data):
    for size_at, image_at in ((0x1716, 0x571A), (0x1712, 0x171A)):
        if len(data) < image_at + 8:
            continue
        size = struct.unpack(">I", data[size_at:size_at + 4])[0]
        png = data[image_at:image_at + size]
        if 0 < size <= 0x4000 and png[:8] == b"\x89PNG\r\n\x1a\n":
            return png
    return None


def main():
    src, out = sys.argv[1], sys.argv[2]
    if Image is None:
        print(f"make_icon: no Pillow here, {out} left as it is")
        return
    with open(src, "rb") as f:
        data = f.read(0x9800)
    if data[:4] not in (b"PIRS", b"LIVE", b"CON "):
        sys.exit(f"make_icon: {src} is not an Xbox 360 content package")
    png = thumbnail(data)
    if not png:
        sys.exit(f"make_icon: no thumbnail in {src}")
    image = Image.open(io.BytesIO(png)).convert("RGBA")
    canvas = Image.new("RGBA", image.size, (0, 0, 0, 255))
    canvas.alpha_composite(image)
    canvas.convert("RGB").resize((256, 256), Image.LANCZOS).save(out, "JPEG", quality=95)
    print(f"make_icon: {out} from {src} ({image.size[0]}x{image.size[1]})")


if __name__ == "__main__":
    main()
