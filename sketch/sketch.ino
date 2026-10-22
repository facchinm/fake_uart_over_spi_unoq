// UNO Q MCU side: SPI3 peripheral that receives bytes from Linux (/dev/spidev0.0),
// reverses each line and hands it back, raising PG13 (Linux GPIO70) when data is pending.
//
// Frame (fixed FRAME_SIZE bytes, both directions, full duplex):
//   [0] MAGIC  [1] TYPE  [2] LEN_LO  [3] LEN_HI  [4..] payload
// Linux sends TYPE_DATA with payload or TYPE_POLL (len 0); the MCU always answers
// in the same transaction with whatever it has queued (TYPE_DATA, len may be 0).

#include <Arduino.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>

#define FRAME_SIZE  256
#define HDR_SIZE    4
#define MAX_PAYLOAD (FRAME_SIZE - HDR_SIZE)
#define MAGIC       0xA5
#define TYPE_POLL   0x00
#define TYPE_DATA   0x01

#define SPI_PERIPHERAL_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(zephyr_spi_slave)

static const struct device *const spi_dev = DEVICE_DT_GET(DT_BUS(SPI_PERIPHERAL_NODE));
// "Internal SPI RDY" line, wired to Linux GPIO70
static const struct device *const rdy_port = DEVICE_DT_GET(DT_NODELABEL(gpiog));
#define RDY_PIN 13

static struct spi_config spi_cfg;

static uint8_t tx_frame[FRAME_SIZE];
static uint8_t rx_frame[FRAME_SIZE];
static struct spi_buf tx_buf = { .buf = tx_frame, .len = FRAME_SIZE };
static struct spi_buf rx_buf = { .buf = rx_frame, .len = FRAME_SIZE };
static const struct spi_buf_set tx_set = { .buffers = &tx_buf, .count = 1 };
static const struct spi_buf_set rx_set = { .buffers = &rx_buf, .count = 1 };

static uint8_t line[MAX_PAYLOAD];
static size_t line_len;

// Output waiting to be clocked out by Linux, larger than one frame
#define RING_SIZE 8192
static uint8_t ring[RING_SIZE];
static size_t ring_head, ring_count;

static void tx_append(const uint8_t *data, size_t len) {
  for (size_t i = 0; i < len && ring_count < RING_SIZE; i++) {
    ring[(ring_head + ring_count) % RING_SIZE] = data[i];
    ring_count++;
  }
}

// Load the next frame from the ring and signal Linux if it carries data
static void tx_prepare() {
  size_t len = ring_count < MAX_PAYLOAD ? ring_count : MAX_PAYLOAD;
  memset(tx_frame, 0, sizeof(tx_frame));
  tx_frame[0] = MAGIC;
  tx_frame[1] = TYPE_DATA;
  tx_frame[2] = len & 0xFF;
  tx_frame[3] = len >> 8;
  for (size_t i = 0; i < len; i++) {
    tx_frame[HDR_SIZE + i] = ring[ring_head];
    ring_head = (ring_head + 1) % RING_SIZE;
  }
  ring_count -= len;
  gpio_pin_set(rdy_port, RDY_PIN, len > 0);
}

static void flush_line() {
  uint8_t out[MAX_PAYLOAD + 2];
  for (size_t i = 0; i < line_len; i++) {
    out[i] = line[line_len - 1 - i];
  }
  out[line_len] = '\r';
  out[line_len + 1] = '\n';
  tx_append(out, line_len + 2);
  line_len = 0;
}

static void handle_rx(const uint8_t *data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    uint8_t c = data[i];
    if (c == '\r' || c == '\n') {
      if (line_len > 0) {
        flush_line();
      }
    } else {
      line[line_len++] = c;
      if (line_len == sizeof(line) - 2) {
        flush_line();
      }
    }
  }
}

void setup() {
  spi_cfg.frequency = 2000000;
  spi_cfg.operation = SPI_WORD_SET(8) | SPI_OP_MODE_SLAVE;

  device_init(spi_dev);  // spi3 is zephyr,deferred-init
  tx_prepare();
}

void loop() {
  memset(rx_frame, 0, sizeof(rx_frame));

  // Blocks until Linux clocks a full frame
  int ret = spi_transceive(spi_dev, &spi_cfg, &tx_set, &rx_set);
  if (ret < 0) {
    return;  // frame not delivered, resend it
  }

  // Linux holds GPIO70 as output-high to release the MCU at boot; drive it only once the bridge talks to us
  static bool rdy_configured;
  if (!rdy_configured) {
    gpio_pin_configure(rdy_port, RDY_PIN, GPIO_OUTPUT_INACTIVE);
    rdy_configured = true;
  }

  if (rx_frame[0] == MAGIC && rx_frame[1] == TYPE_DATA) {
    uint16_t len = (uint16_t)rx_frame[2] | ((uint16_t)rx_frame[3] << 8);
    if (len <= MAX_PAYLOAD) {
      handle_rx(&rx_frame[HDR_SIZE], len);
    }
  }
  tx_prepare();
}
