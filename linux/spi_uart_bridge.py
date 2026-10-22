#!/usr/bin/env python3
"""Fake UART <-> SPI bridge for the Arduino UNO Q (Linux side).

Meant to be spawned by socat (see run.sh): stdin/stdout are the fake serial port.
Bytes written to the port are sent to the MCU over SPI; when the MCU raises
GPIO70 it has data pending, which is fetched with an SPI transaction and written
back to the port. Both directions are logged to stderr.
"""
import argparse
import glob
import os
import select
import sys
import time

import gpiod
import spidev
from gpiod.line import Direction, Edge, Value

FRAME_SIZE = 256
HDR_SIZE = 4
MAX_PAYLOAD = FRAME_SIZE - HDR_SIZE
MAGIC = 0xA5
TYPE_POLL = 0x00
TYPE_DATA = 0x01

# The MCU must re-arm its SPI peripheral between transactions
MIN_GAP_S = 0.002


def log(msg):
    print(msg, file=sys.stderr, flush=True)


def find_gpiochip(line):
    for path in sorted(glob.glob("/dev/gpiochip*")):
        with gpiod.Chip(path) as chip:
            info = chip.get_info()
            if info.num_lines > line and ("pinctrl" in info.label or "tlmm" in info.label):
                return path
    sys.exit(f"no SoC gpiochip with line {line} found, use --gpio-chip")


class Bridge:
    def __init__(self, spi, out_fd):
        self.spi = spi
        self.out_fd = out_fd
        self.last = 0.0

    def transfer(self, frame_type, payload=b""):
        frame = bytearray(FRAME_SIZE)
        frame[0] = MAGIC
        frame[1] = frame_type
        frame[2] = len(payload) & 0xFF
        frame[3] = len(payload) >> 8
        frame[HDR_SIZE:HDR_SIZE + len(payload)] = payload

        wait = self.last + MIN_GAP_S - time.monotonic()
        if wait > 0:
            time.sleep(wait)
        rx = bytes(self.spi.xfer2(list(frame)))
        self.last = time.monotonic()

        if rx[0] != MAGIC or rx[1] != TYPE_DATA:
            return
        length = rx[2] | (rx[3] << 8)
        if 0 < length <= MAX_PAYLOAD:
            data = rx[HDR_SIZE:HDR_SIZE + length]
            log(f"MCU   -> {data!r}")
            os.write(self.out_fd, data)

    def send(self, data):
        log(f"Linux -> {data!r}")
        for i in range(0, len(data), MAX_PAYLOAD):
            self.transfer(TYPE_DATA, data[i:i + MAX_PAYLOAD])


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--spi-dev", default=os.environ.get("SPI_DEV", "/dev/spidev0.0"))
    p.add_argument("--spi-speed", type=int, default=int(os.environ.get("SPI_SPEED", "1000000")))
    p.add_argument("--gpio-chip", default=os.environ.get("GPIO_CHIP"))
    p.add_argument("--gpio-line", type=int, default=int(os.environ.get("GPIO_LINE", "70")))
    args = p.parse_args()

    bus, dev = (int(x) for x in args.spi_dev.removeprefix("/dev/spidev").split("."))
    spi = spidev.SpiDev()
    spi.open(bus, dev)
    spi.max_speed_hz = args.spi_speed
    spi.mode = 0
    spi.bits_per_word = 8

    chip = args.gpio_chip or find_gpiochip(args.gpio_line)
    req = gpiod.request_lines(
        chip,
        consumer="spi-uart-bridge",
        config={args.gpio_line: gpiod.LineSettings(direction=Direction.INPUT, edge_detection=Edge.RISING)},
    )
    log(f"bridge up: {args.spi_dev} @ {args.spi_speed} Hz, ready line {chip}:{args.gpio_line}")

    bridge = Bridge(spi, sys.stdout.fileno())
    in_fd = sys.stdin.fileno()

    def drain_mcu():
        # Bounded so a stuck-high line can't starve the input side; returns True if still pending
        for _ in range(16):
            if req.get_value(args.gpio_line) != Value.ACTIVE:
                return False
            bridge.transfer(TYPE_POLL)
        return req.get_value(args.gpio_line) == Value.ACTIVE

    pending = False
    try:
        while True:
            ready, _, _ = select.select([in_fd, req.fd], [], [], 0 if pending else 1.0)
            if req.fd in ready:
                req.read_edge_events()
            if in_fd in ready:
                data = os.read(in_fd, 4096)
                if not data:
                    break
                bridge.send(data)
            pending = drain_mcu()
    finally:
        req.release()
        spi.close()


if __name__ == "__main__":
    main()
