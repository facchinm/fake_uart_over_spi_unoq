#!/bin/bash
# systemd entry point: run the bridge once the MCU has been released from its boot
# animation, and restart it around every MCU upload staged in $UPLOAD_DIR.
set -u

DIR=$(cd "$(dirname "$0")" && pwd)
UPLOAD_DIR=${UPLOAD_DIR:-/tmp/remoteocd}
READY_CHIP=${READY_CHIP:-gpiochip1}
READY_LINE=${READY_LINE:-70}
OPENOCD=${OPENOCD:-/opt/openocd/bin/_openocd}
ROUTER_SOCK=${ROUTER_SOCK:-/var/run/arduino-router.sock}

log() { echo "spi-uart-service: $*"; }

start_bridge() {
    # Own session, so stopping it also stops socat and the bridge
    setsid "$DIR/run.sh" &
    BRIDGE=$!
    log "bridge started (pid $BRIDGE)"
}

stop_bridge() {
    kill -TERM -- "-$BRIDGE" 2>/dev/null
    wait "$BRIDGE" 2>/dev/null
    log "bridge stopped"
}

wait_for_flash() {
    # The files are staged first, then openocd runs
    for _ in $(seq 500); do
        if pgrep -f "$OPENOCD" >/dev/null; then
            # As soon as openocd runs, so the loader never plays the boot animation
            gpioset -c "$READY_CHIP" -t0 "$READY_LINE=1"
            break
        fi
        sleep 0.02
    done
    while pgrep -f "$OPENOCD" >/dev/null; do
        sleep 0.2
    done
}

trap 'stop_bridge; exit 0' TERM INT

# arduino-router drives the ready line high once ready, ending the boot animation;
# let its hook run before the bridge takes the line
for _ in $(seq 120); do
    [ -S "$ROUTER_SOCK" ] && break
    sleep 1
done
sleep 2

mkdir -p "$UPLOAD_DIR"
exec 3< <(inotifywait -m -q -e create,close_write,moved_to "$UPLOAD_DIR")
start_bridge

while read -r -u 3 _; do
    log "upload detected"
    stop_bridge
    wait_for_flash
    while read -r -u 3 -t 1 _; do :; done
    log "flash done"
    start_bridge
done
