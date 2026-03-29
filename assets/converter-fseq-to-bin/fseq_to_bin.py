import struct
import os

INPUT  = "teensies.fseq"
OUTPUT = "teensies.bin"

WIDTH  = 64
HEIGHT = 16
PIXELS = WIDTH * HEIGHT
OUT_CH = PIXELS * 3

OFFSET_PIX = 0
OFFSET_CH  = OFFSET_PIX * 3

def u16_le(b, off): return struct.unpack_from("<H", b, off)[0]
def u32_le(b, off): return struct.unpack_from("<I", b, off)[0]

with open(INPUT, "rb") as f:
    hdr = f.read(128)

    if hdr[0:4] != b"PSEQ":
        raise SystemExit("To nie jest FSEQ")

    data_off = u16_le(hdr, 4)      # <-- to jest poprawne dla tego pliku
    channels = u32_le(hdr, 10)     # <-- 32-bit, nie 16-bit

    print("Channels:", channels)
    print("Data offset:", data_off)

    file_size = os.path.getsize(INPUT)

    # w tym pliku na końcu są jeszcze 22 bajty, które nie należą do pełnych klatek
    total_data = file_size - data_off
    real_frames = total_data // channels

    print("Real frames:", real_frames)

    f.seek(data_off)

    with open(OUTPUT, "wb") as out:
        for fi in range(real_frames):
            frame = f.read(channels)
            if len(frame) < channels:
                break

            d = frame[:OUT_CH]
            if len(d) < OUT_CH:
                d += bytes(OUT_CH - len(d))

            if OFFSET_CH:
                d = d[OFFSET_CH:] + d[:OFFSET_CH]

            out.write(d)

print("OK ->", OUTPUT)