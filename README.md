# Fake UART over SPI – Arduino UNO Q

```
 screen /tmp/ttyUNOQ0 ── socat PTY ── spi_uart_bridge.py ──SPI (/dev/spidev0.0)──> STM32U585 SPI3 (sketch or MicroPython)
                                              ^                                              |
                                              └──────────── GPIO70 (MCU PG13 "SPI RDY") ─────┘
```

- `linux/` – runs on the QRB2210 (Debian). socat creates the pty, the bridge forwards
  writes to SPI and, on a GPIO70 rising edge, clocks a frame to fetch the MCU data.
- MCU side, one of:
  - `sketch/` – Zephyr-based Arduino sketch. Reverses each received line and sends
    it back (`hello\n` → `olleh\r\n`).
  - MicroPython (Zephyr port) with its REPL running over the same link.
- `patches/zephyr/` – Zephyr SPI driver fix needed by MicroPython to run SPI3 as
  peripheral at 1 MHz.

Frame format (256 bytes, full duplex): `A5 TYPE LEN_LO LEN_HI payload...`
(`TYPE` 0x00 = poll, 0x01 = data).

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

Only one bridge can run at a time (it owns GPIO70); a second one fails with `EBUSY`.

## MCU, option A: Arduino sketch

On the board:

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

## MCU, option B: MicroPython

MicroPython's Zephyr port gets an SPI stdio driver (`CONFIG_MICROPY_GETCHAR_SPI`,
`ports/zephyr/src/zephyr_spi_stdio.c`) that replaces the UART console; the
`arduino_uno_q` board enables it on SPI3 with PG13 as ready line. Built on a host PC,
flashed through adb.

### 1. Zephyr workspace

Uses the Arduino Zephyr fork (`zephyr-arduino-v4.4.99`) through the ArduinoCore-zephyr
manifest. `bootstrap.sh` creates `venv/` with west, runs `west init -l` / `west update`
and installs the Zephyr SDK:

```sh
mkdir zephyr-ws && cd zephyr-ws
git clone -b arduino-next https://github.com/arduino/ArduinoCore-zephyr
cd ArduinoCore-zephyr && ./extra/bootstrap.sh && cd ..
```

The workspace root is `zephyr-ws/` (Zephyr in `zephyr-ws/zephyr`). Always activate
the venv and run `west` from inside the workspace:

```sh
source zephyr-ws/ArduinoCore-zephyr/venv/bin/activate
```

### 2. Zephyr SPI driver fix

In peripheral mode the STM32 SPI driver queues a single FIFO packet, so the TxFIFO
underruns above ~100 kHz. Apply the fix from this repo:

```sh
cd zephyr-ws/zephyr
git switch -c spi_stm32_peripheral_prefill
git am <this repo>/patches/zephyr/0001-drivers-spi-stm32-pre-fill-two-packets-in-peripheral.patch
```

### 3. MicroPython

```sh
git clone -b unoq_spi_shell https://github.com/facchinm/micropython
cd micropython && git submodule update --init lib/micropython-lib   # for the frozen manifest
```

### 4. Build

```sh
cd zephyr-ws
west build -p always -b arduino_uno_q -d build-mp-unoq <path to micropython>/ports/zephyr
```

### 5. Flash

The board's openocd drives the MCU SWD lines through GPIOs. This overwrites the
Arduino core:

```sh
adb push build-mp-unoq/zephyr/zephyr.elf /tmp/micropython.elf
adb shell 'cd /opt/openocd && ./bin/openocd -s . -s share/openocd/scripts -f openocd_gpiod.cfg \
  -c "reset_config srst_only srst_push_pull; init; reset halt; flash write_image erase /tmp/micropython.elf; reset run; shutdown"'
```

openocd prints `Translation from khz to adapter speed not implemented` errors; they
are harmless.

### 6. Use

Start the bridge as in [Linux](#linux) and open `/tmp/ttyUNOQ0`: the boot banner is
buffered until the bridge connects, then the `>>>` prompt is ready (Ctrl-C works).
No GPIO70 boot release is needed, that is an Arduino core loader feature. Kernel
messages (`printk`) still go to LPUART1 (`/dev/ttyHS1`).

To go back to the Arduino core, upload the sketch again (option A), which also
flashes the core image.
