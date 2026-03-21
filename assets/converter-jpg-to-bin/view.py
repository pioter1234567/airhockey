import os
import time
from PIL import Image
import numpy as np

w, h = 480, 320

folder = "view"

files = sorted([f for f in os.listdir(folder) if f.endswith(".bin")])

for file in files:
    with open(os.path.join(folder, file), "rb") as f:
        data = np.frombuffer(f.read(), dtype=np.uint16).byteswap()

    data = data.reshape((h, w))

    r = ((data >> 11) & 0x1F) << 3
    g = ((data >> 5) & 0x3F) << 2
    b = (data & 0x1F) << 3

    img = np.dstack((r, g, b)).astype('uint8')

    Image.fromarray(img).show()
    print(file)

    time.sleep(0.5)