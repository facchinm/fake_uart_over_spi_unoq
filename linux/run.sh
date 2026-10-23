#!/bin/sh
# Expose a fake serial port at $TTY_LINK backed by the SPI bridge.
# Env passthrough: SPI_DEV, SPI_SPEED, GPIO_CHIP, GPIO_LINE
set -eu

gpioset -c1 -t0 70=1

TTY_LINK=${TTY_LINK:-/tmp/ttyUNOQ0}
DIR=$(cd "$(dirname "$0")" && pwd)

# socat exits when the last client closes the port; restart it
while true; do
    socat -d PTY,link="$TTY_LINK",raw,echo=0 EXEC:"python3 -u $DIR/spi_uart_bridge.py" || true
    sleep 0.5
done
