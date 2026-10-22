# Fake UART over SPI – Arduino UNO Q

```
 screen /tmp/ttyUNOQ0 ── socat PTY ── spi_uart_bridge.py ──SPI (/dev/spidev0.0)──> STM32U585 SPI3 (sketch)
                                              ^                                              |
                                              └──────────── GPIO70 (MCU PG13 "SPI RDY") ─────┘
```

- `linux/` – runs on the QRB2210 (Debian). socat creates the pty, the bridge forwards
  writes to SPI and, on a GPIO70 rising edge, clocks a frame to fetch the MCU data.
- `sketch/` – Zephyr-based Arduino sketch for the STM32U585. Reverses each received
  line and sends it back (`hello\n` → `olleh\r\n`).

Frame format (256 bytes, full duplex): `A5 TYPE LEN_LO LEN_HI payload...`
(`TYPE` 0x00 = poll, 0x01 = data).

## MCU

```sh
arduino-cli compile --upload --profile default sketch
```
(or open `sketch/` in App Lab / Arduino IDE with board *Arduino UNO Q*).

After flashing, the MCU shows the boot animation until Linux drives GPIO70 high
(the same line is later used as the MCU "data ready" signal). Release it with:

```sh
timeout 5 gpioset -c gpiochip1 70=1
```

The sketch only starts driving PG13 after the first SPI frame from the bridge, so
the two sides never drive the line at the same time.

## Linux

One-time setup on the board (Debian trixie):

```sh
sudo apt install -y socat python3-spidev python3-libgpiod   # libgpiod >= 2
sudo chmod 666 /dev/spidev0.0   # root-only by default, resets on reboot (or run.sh as root)
```

The `arduino` user is already in the `gpiod` group, so `/dev/gpiochip*` needs no change.

```sh
./linux/run.sh
```

Then, in another shell:

```sh
screen /tmp/ttyUNOQ0           # or: echo hello > /tmp/ttyUNOQ0; cat /tmp/ttyUNOQ0
```

Env overrides: `TTY_LINK`, `SPI_DEV`, `SPI_SPEED`, `GPIO_CHIP`, `GPIO_LINE`.
