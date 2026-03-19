from PIL import Image
import sys
import os

def rgb888_to_rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)

def convert_image(input_path, output_path=None):
    img = Image.open(input_path).convert("RGB")
    width, height = img.size

    if output_path is None:
        base, _ = os.path.splitext(input_path)
        output_path = base + ".bin"

    with open(output_path, "wb") as f:
        for y in range(height):
            for x in range(width):
                r, g, b = img.getpixel((x, y))
                c = rgb888_to_rgb565(r, g, b)

                # zapis big-endian / swapped dla TFT
                f.write(bytes(((c >> 8) & 0xFF, c & 0xFF)))

    print(f"OK: {input_path} -> {output_path} ({width}x{height})")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Uzycie: py jpg_to_rgb565.py obraz.jpg [wyjscie.bin]")
        sys.exit(1)

    input_path = sys.argv[1]
    output_path = sys.argv[2] if len(sys.argv) >= 3 else None
    convert_image(input_path, output_path)