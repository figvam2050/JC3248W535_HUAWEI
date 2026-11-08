#!/usr/bin/env python3
"""
Скрипт для витягування графічних іконок LVGL з дампу прошивки JC3248W535C.
"""

import os
from PIL import Image

OUT_DIR = "extracted_assets"
os.makedirs(OUT_DIR, exist_ok=True)

FIRMWARE_FILE = "backup/jc3248w535c_factory_full_16MB.bin"

with open(FIRMWARE_FILE, "rb") as f:
    f.seek(0x10000)  # Початок розділу app0
    data = f.read(0x200000)

icons = [
    ("icon_1_music", 0x1e1bc, 94, 94),
    ("icon_2_photo", 0x24954, 94, 94),
    ("icon_3_video", 0x2b0ec, 94, 94),
    ("icon_4_settings", 0x31884, 94, 94),
    ("icon_5_timer", 0x3801c, 94, 94),
    ("icon_6_sound", 0x3e7b4, 94, 94),
    ("icon_7_app7", 0x44f4c, 94, 94),
    ("icon_8_app8", 0x4b6e4, 94, 94),
]

for name, desc_offset, w, h in icons:
    data_offset = desc_offset + 12
    raw_pixels = data[data_offset : data_offset + w * h * 3]
    
    img = Image.new("RGBA", (w, h))
    pixels = img.load()
    
    idx = 0
    for y in range(h):
        for x in range(w):
            c16 = int.from_bytes(raw_pixels[idx:idx+2], "little")
            alpha = raw_pixels[idx+2]
            idx += 3
            
            # RGB565 to RGB888
            r = ((c16 >> 11) & 0x1F) * 255 // 31
            g = ((c16 >> 5) & 0x3F) * 255 // 63
            b = (c16 & 0x1F) * 255 // 31
            pixels[x, y] = (r, g, b, alpha)
            
    out_path = os.path.join(OUT_DIR, f"{name}.png")
    img.save(out_path)
    print(f"Збережено: {out_path} ({w}x{h})")

print(f"\nУсі іконки успішно витягнуто в папку '{OUT_DIR}/'.")

