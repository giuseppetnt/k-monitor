
#!/usr/bin/env python3

import os
import sys
import time
import struct
import select
import termios
import zlib
from pathlib import Path
from datetime import datetime

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
MAGIC = b"KMSHOT01"


def read_more(fd, buffer, needed, timeout=120):
    deadline = time.monotonic() + timeout

    while len(buffer) < needed:
        remaining = deadline - time.monotonic()

        if remaining <= 0:
            raise TimeoutError("Timeout durante la ricezione")

        ready, _, _ = select.select([fd], [], [], remaining)

        if not ready:
            raise TimeoutError("Nessun dato ricevuto")

        data = os.read(fd, max(4096, needed - len(buffer)))

        if data:
            buffer.extend(data)

    return buffer


def png_chunk(tag, data):
    body = tag + data
    return (
        struct.pack(">I", len(data))
        + body
        + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)
    )


def save_png(filename, raw, width, height):
    pixels = bytearray()

    for y in range(height):
        pixels.append(0)  # PNG filter: None

        for x in range(width):
            i = (y * width + x) * 2

            pixel = raw[i] | (raw[i + 1] << 8)

            r5 = (pixel >> 11) & 0x1F
            g6 = (pixel >> 5) & 0x3F
            b5 = pixel & 0x1F

            r = (r5 << 3) | (r5 >> 2)
            g = (g6 << 2) | (g6 >> 4)
            b = (b5 << 3) | (b5 >> 2)

            pixels.extend((r, g, b))

    ihdr = struct.pack(
        ">IIBBBBB",
        width, height, 8, 2, 0, 0, 0
    )

    compressed = zlib.compress(bytes(pixels), 6)

    with open(filename, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(png_chunk(b"IHDR", ihdr))
        f.write(png_chunk(b"IDAT", compressed))
        f.write(png_chunk(b"IEND", b""))


def main():
    print(f"Connessione a {PORT}...")

    fd = os.open(PORT, os.O_RDWR | os.O_NOCTTY)

    try:
        attrs = termios.tcgetattr(fd)
        attrs[0] = 0
        attrs[1] = 0
        attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        attrs[3] = 0
        attrs[4] = termios.B115200
        attrs[5] = termios.B115200
        attrs[6][termios.VMIN] = 0
        attrs[6][termios.VTIME] = 0

        termios.tcsetattr(fd, termios.TCSANOW, attrs)
        termios.tcflush(fd, termios.TCIFLUSH)

        time.sleep(1)
        print("Richiedo screenshot alla ESP32...")

        os.write(fd, b"s")

        buffer = bytearray()
        deadline = time.monotonic() + 30

        while MAGIC not in buffer:
            if time.monotonic() > deadline:
                raise TimeoutError("Intestazione screenshot non ricevuta")

            read_more(fd, buffer, len(buffer) + 1, timeout=5)

            if len(buffer) > 65536:
                del buffer[:-len(MAGIC)]

        pos = buffer.index(MAGIC)
        buffer = buffer[pos + len(MAGIC):]

        read_more(fd, buffer, 8)

        width, height = struct.unpack("<II", buffer[:8])
        del buffer[:8]

        if width <= 0 or height <= 0 or width > 2048 or height > 1200:
            raise ValueError("Dimensioni immagine non valide")

        size = width * height * 2

        print(f"Ricezione: {width} x {height}")
        print(f"Dimensione: {size} byte")

        read_more(fd, buffer, size, timeout=120)
        raw = buffer[:size]

        output_dir = Path(__file__).resolve().parent.parent / "screenshots"
        output_dir.mkdir(exist_ok=True)

        filename = output_dir / (
            "display_" + datetime.now().strftime("%Y%m%d_%H%M%S") + ".png"
        )

        save_png(filename, raw, width, height)

        print()
        print("SCREENSHOT SALVATO!")
        print(filename)

    finally:
        os.close(fd)


if __name__ == "__main__":
    main()
