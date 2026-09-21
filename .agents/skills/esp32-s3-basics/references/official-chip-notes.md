# ESP32-S3 Official Chip Notes

## Use This File For

- Official ESP32-S3 capability snapshots
- Reserved-pin and boot-pin caveats
- Memory-placement and DMA rules
- I2S hardware facts that affect bus design

## Chip Snapshot

- Espressif's product page describes ESP32-S3 as a dual-core Xtensa LX7 MCU up to 240 MHz with 512 KB internal SRAM, 45 programmable GPIOs, Wi-Fi, and Bluetooth LE 5.
- The same page highlights support for larger Octal SPI flash and PSRAM, 14 capacitive-touch GPIOs, a ULP core, and vector instructions used by signal-processing and neural-network libraries.
- Source:
  - https://www.espressif.com/en/products/socs/esp32-s3

## Pin Caveats

- ESP-IDF's GPIO guide says ESP32-S3 has 45 physical GPIO pads and uses IO MUX plus the GPIO matrix to route peripheral signals.
- Treat GPIO0, GPIO3, GPIO45, and GPIO46 as strapping pins. Keep their power-up state safe before reusing them at runtime.
- Avoid planning normal peripherals on GPIO26-32 when they are used by SPI flash or PSRAM. On boards with Octal Flash or Octal PSRAM, GPIO33-37 are also usually occupied.
- GPIO19 and GPIO20 are used by USB-JTAG by default; using them as plain GPIO disables USB-JTAG through drivers.
- Sources:
  - https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32s3/api-reference/peripherals/gpio.html
  - https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32s3/schematic-checklist.html

## Memory and DMA Caveats

- ESP-IDF's memory guide says unused internal SRAM not consumed by IRAM becomes DRAM for static data and heap. Large IRAM usage therefore reduces available DRAM and runtime heap.
- Interrupt handlers that use `ESP_INTR_FLAG_IRAM` must live in IRAM.
- Most DMA-capable peripherals expect buffers in DRAM and word-aligned memory. Prefer static DMA buffers with `DMA_ATTR` instead of stack allocation.
- DMA buffers on the stack are possible but discouraged, especially if the task stack may live in PSRAM.
- Source:
  - https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32s3/api-guides/memory-types.html

## I2S Hardware Notes

- ESP-IDF's ESP32-S3 I2S guide says the chip has two I2S peripherals.
- In I2S full-duplex mode, TX and RX channels on the same port share BCLK and WS, so both sides must be configured with matching clock and slot settings.
- PDM full-duplex is not supported because TX and RX clocks differ.
- Source:
  - https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32s3/api-reference/peripherals/i2s.html
