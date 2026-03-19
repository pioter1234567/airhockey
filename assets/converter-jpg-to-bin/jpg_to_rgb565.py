from PIL import Image
import os

def rgb888_to_rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)

def convert_image(input_path, output_path):
    img = Image.open(input_path).convert("RGB")
    width, height = img.size

    with open(output_path, "wb") as f:
        for y in range(height):
            for x in range(width):
                r, g, b = img.getpixel((x, y))
                c = rgb888_to_rgb565(r, g, b)

                # big-endian (TFT)
                f.write(bytes(((c >> 8) & 0xFF, c & 0xFF)))

    print(f"OK: {input_path} -> {output_path} ({width}x{height})")


def convert_all_in_folder(folder="."):
    for filename in os.listdir(folder):
        if filename.lower().endswith(".jpg"):
            input_path = os.path.join(folder, filename)
            output_path = os.path.join(
                folder,
                os.path.splitext(filename)[0] + ".bin"
            )
            convert_image(input_path, output_path)


if __name__ == "__main__":
    convert_all_in_folder()