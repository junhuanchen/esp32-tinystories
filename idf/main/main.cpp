#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "driver/usb_serial_jtag.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Minimal Arduino serial compatibility for the existing inference sketch. The
// sketch retains the model and tokenizer implementation; this adapter maps its
// USB I/O and delay calls to ESP-IDF and adds no Arduino component dependency.
class UsbSerialJtag {
 public:
  void begin(unsigned long) {}

  int available() {
    if (count_ == 0) {
      int read = usb_serial_jtag_read_bytes(
          buffer_, sizeof(buffer_), 0);
      if (read > 0) { head_ = 0; count_ = read; }
    }
    return count_;
  }

  int read() {
    if (!available()) return -1;
    int value = buffer_[head_++];
    --count_;
    return value;
  }

  int availableForWrite() const { return 1024; }

  size_t write(uint8_t byte) { return write(&byte, 1); }

  size_t write(const unsigned char *data, size_t length) {
    int written = usb_serial_jtag_write_bytes(
        reinterpret_cast<const char *>(data), length, portMAX_DELAY);
    return written > 0 ? static_cast<size_t>(written) : 0;
  }

  size_t print(const char *text) {
    return write(reinterpret_cast<const unsigned char *>(text), strlen(text));
  }

  size_t println() { return print("\n"); }

  size_t println(const char *text) { print(text); return println(); }

  int printf(const char *format, ...) {
    char buffer[384];
    va_list args;
    va_start(args, format);
    int count = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (count <= 0) return count;
    size_t length = static_cast<size_t>(count);
    if (length >= sizeof(buffer)) length = sizeof(buffer) - 1;
    write(reinterpret_cast<const unsigned char *>(buffer), length);
    return count;
  }

 private:
  uint8_t buffer_[512];
  int head_ = 0;
  int count_ = 0;
};

UsbSerialJtag Serial;

void delay(unsigned long milliseconds) {
  vTaskDelay(pdMS_TO_TICKS(milliseconds));
}

// The Arduino sketch is intentionally included rather than duplicated. Its
// relative runtime includes and generated tokenizer headers are provided by
// this component's include paths.
#include "../../firmware/esp32_tinystories/esp32_tinystories.ino"

static void tinystories_task(void *) {
  setup();
  for (;;) loop();
}

extern "C" void app_main(void) {
  usb_serial_jtag_driver_config_t config = {};
  config.tx_buffer_size = 1024;
  config.rx_buffer_size = 512;
  ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&config));

  // The inherited sketch pins its matvec worker to CPU0. Run its main task on
  // CPU1 so the two cores actually execute in parallel.
  configASSERT(xTaskCreatePinnedToCore(
      tinystories_task, "tinystories", 8192, nullptr, 5, nullptr, 1) == pdPASS);
}
