# MicroPython REPL over SPI – Arduino UNO Q

Run a MicroPython REPL on the UNO Q microcontroller (STM32U585) next to your Arduino
sketch, and reach it from Linux (QRB2210) as a regular serial port.

```
 screen /tmp/ttyUNOQ0 ── socat PTY ── spi_uart_bridge.py ──SPI (/dev/spidev0.0)──> STM32U585 SPI3 (MicroPython)
                                              ^                                              |
                                              └──────────── GPIO70 (MCU PG13 "SPI RDY") ─────┘
```

- **MCU**: the Arduino Zephyr core loader, built with MicroPython in its own thread.
  MicroPython starts as soon as Linux is up (right after the boot animation) and its
  REPL runs over SPI3, as SPI peripheral of the QRB2210.
- **Linux** (`linux/` in this repo): `socat` exposes a pty, `spi_uart_bridge.py`
  forwards what you type to `/dev/spidev0.0` and, whenever the MCU raises GPIO70,
  clocks a frame to fetch its output.

Frame format (256 bytes, full duplex): `A5 TYPE LEN_LO LEN_HI payload...`
(`TYPE` 0x00 = poll, 0x01 = data).

The history of this repository shows how the setup was developed (a plain sketch
first, then standalone MicroPython, then the loader integration).

## 1. Build the core with MicroPython (host PC)

The pieces live in these branches:

| Repository | Branch | Content |
|---|---|---|
| [facchinm/ArduinoCore-zephyr](https://github.com/facchinm/ArduinoCore-zephyr) | `loader_micropython` | west project, loader thread, `build.sh --micropython` |
| [facchinm/micropython](https://github.com/facchinm/micropython) | `unoq_spi_shell` | SPI stdio driver, embedding support (fetched by west) |
| [arduino/zephyr](https://github.com/arduino/zephyr) | `zephyr-arduino-v4.4.99` | STM32 SPI peripheral fix (fetched by west) |

Set up the workspace. `bootstrap.sh` creates `venv/` with west, runs `west init` /
`west update` and installs the Zephyr SDK:

```sh
mkdir zephyr-ws && cd zephyr-ws
git clone -b loader_micropython https://github.com/facchinm/ArduinoCore-zephyr
cd ArduinoCore-zephyr
./extra/bootstrap.sh
```

MicroPython is an optional west project, fetch it once:

```sh
source venv/bin/activate
west config manifest.group-filter +micropython
west update micropython
```

Build the loader:

```sh
./extra/build.sh unoq --micropython
```

The result is `firmwares/zephyr-arduino_uno_q_stm32u585xx.elf`.

## 2. Flash the loader

The board's openocd drives the MCU SWD lines through GPIOs. From the PC, with the
board connected over USB (or use `scp` / `ssh` instead of adb):

```sh
adb push firmwares/zephyr-arduino_uno_q_stm32u585xx.elf /tmp/loader.elf
adb shell 'cd /opt/openocd && ./bin/openocd -s . -s share/openocd/scripts -f openocd_gpiod.cfg \
  -c "reset_config srst_only srst_push_pull; init; reset halt; flash write_image erase /tmp/loader.elf; reset run; shutdown"'
```

openocd prints `Translation from khz to adapter speed not implemented` errors; they
are harmless. After flashing, the matrix shows the boot animation until Linux drives
GPIO70 high; the bridge does that when it starts (on a normal boot `arduino-router`
does it too).

Sketches keep working, but they must be compiled and uploaded with **this** core:
uploading with the stock core (App Lab, or `arduino-cli` with the released
`arduino:zephyr` platform) flashes its own loader and removes MicroPython. To use the
core from the Arduino IDE / CLI, link it into your sketchbook as described in the
ArduinoCore-zephyr README (`~/Arduino/hardware/arduino-git/zephyr`).

## 3. Linux side (on the board)

Clone this repository on the board, then the one-time setup (Debian trixie):

```sh
sudo apt install -y socat python3-spidev python3-libgpiod inotify-tools   # libgpiod >= 2
```

### Start at boot (recommended)

Install the systemd service, which:

- starts the bridge once `arduino-router` has released the MCU from its boot
  animation;
- watches `/tmp/remoteocd`, where sketch/core uploads are staged: on activity it stops
  the bridge, drives GPIO70 high, waits for openocd to finish flashing and starts the
  bridge again, so the REPL comes back by itself after every upload.

```sh
sudo mkdir -p /opt/spi-uart-bridge
sudo cp linux/run.sh linux/spi_uart_bridge.py linux/spi_uart_service.sh /opt/spi-uart-bridge/
sudo cp linux/spi-uart-bridge.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now spi-uart-bridge
```

The service runs as `arduino` and gives the `gpiod` group access to
`/dev/spidev0.0` at start. Logs: `journalctl -u spi-uart-bridge -f`.

### Manual start

`/dev/spidev0.0` is root-only by default (reset at every boot):

```sh
sudo chmod 666 /dev/spidev0.0
```

The `arduino` user is already in the `gpiod` group, so `/dev/gpiochip*` needs no change.

```sh
./linux/run.sh
```

### Use

Open the fake serial port:

```sh
screen /tmp/ttyUNOQ0
```

```
MicroPython v1.30.0-preview... on 2026-10-23; zephyr-arduino_uno_q with stm32u585xx
>>> from machine import ADC
>>> ADC(("adc1", 9)).read_u16()
```

The boot banner is buffered until the bridge connects; Ctrl-C, paste mode and raw
REPL work as usual. When it starts, the bridge keeps GPIO70 high until MicroPython
answers over SPI, so a freshly flashed loader never stays stuck in the boot animation.

Env overrides for `run.sh`: `TTY_LINK`, `SPI_DEV`, `SPI_SPEED`, `GPIO_CHIP`,
`GPIO_LINE`. Only one bridge can run at a time (it owns GPIO70); a second one fails
with `EBUSY`.

## Notes

- MicroPython runs at the sketch priority with time slicing, so a busy Python loop
  slows the sketch down but doesn't stop it (and vice versa).
- SPI3 and PG13 are taken by the REPL: sketches must not use SPI3.
- Peripherals are looked up by devicetree node label (`"adc1"`, `"i2c4"`, ...) and
  initialised on first use, like the Arduino core does.
- `/flash` is not mounted automatically: MicroPython's default `_boot.py` would
  format the `storage` partition shared with sketches.
- Kernel messages (`printk`) go to the Zephyr console, not to the REPL.
