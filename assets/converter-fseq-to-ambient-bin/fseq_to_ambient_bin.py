import struct
import os
import glob

# Konwerter FSEQ -> surowy BIN dla ambientu pod stołem
# Założenie:
# - xLights Window Frame ma 192 pixele
# - LED 0 w xLights = fizyczny pierwszy LED
# - pasek startuje w górnym lewym rogu i idzie zgodnie z ruchem wskazówek zegara
# - wynik BIN: każda klatka = 192 * 3 bajty, kolejność RGB



# Ile LED-ów ma pasek ambiente
PIXELS = 192
OUT_CH = PIXELS * 3

# Korekty, gdyby animacja była przesunięta albo szła pod prąd
OFFSET_PIX = 0
REVERSE = False

# Jak chcesz konkretny wzorzec, zmień np. na "*_ambiente.fseq"
INPUT_PATTERN = "*.fseq"


def u16_le(b, off):
    return struct.unpack_from("<H", b, off)[0]


def u32_le(b, off):
    return struct.unpack_from("<I", b, off)[0]


def apply_pixel_offset(data):
    if OFFSET_PIX == 0:
        return data

    offset = OFFSET_PIX % PIXELS
    offset_ch = offset * 3
    return data[offset_ch:] + data[:offset_ch]


def apply_reverse(data):
    if not REVERSE:
        return data

    pixels = [data[i:i+3] for i in range(0, len(data), 3)]
    pixels.reverse()
    return b"".join(pixels)


def convert_fseq_to_bin(input_path):
    base, _ = os.path.splitext(input_path)
    output_path = base + ".bin"

    with open(input_path, "rb") as f:
        hdr = f.read(128)

        if hdr[0:4] != b"PSEQ":
            print(f"[SKIP] {input_path} - to nie jest FSEQ")
            return

        data_off = u16_le(hdr, 4)
        channels = u32_le(hdr, 10)

        if channels <= 0:
            print(f"[SKIP] {input_path} - channels = 0")
            return

        file_size = os.path.getsize(input_path)
        total_data = file_size - data_off
        real_frames = total_data // channels
        trailing = total_data % channels

        print()
        print(f"Input:  {input_path}")
        print(f"Output: {output_path}")
        print(f"Channels: {channels}")
        print(f"Data offset: {data_off}")
        print(f"Frames: {real_frames}")
        print(f"Trailing bytes ignored: {trailing}")

        f.seek(data_off)

        with open(output_path, "wb") as out:
            for fi in range(real_frames):
                frame = f.read(channels)

                if len(frame) < channels:
                    break

                # Bierzemy tylko pierwsze 192 LED * 3 kanały
                d = frame[:OUT_CH]

                # Gdyby kanałów było mniej, dopełniamy czarnym
                if len(d) < OUT_CH:
                    d += bytes(OUT_CH - len(d))

                d = apply_pixel_offset(d)
                d = apply_reverse(d)

                out.write(d)

    print(f"OK -> {output_path}")


def main():
    files = sorted(glob.glob(INPUT_PATTERN))

    if not files:
        print(f"Nie znaleziono plików: {INPUT_PATTERN}")
        return

    print(f"Znaleziono {len(files)} plików FSEQ")

    for path in files:
        convert_fseq_to_bin(path)

    print()
    print("Gotowe.")


if __name__ == "__main__":
    main()