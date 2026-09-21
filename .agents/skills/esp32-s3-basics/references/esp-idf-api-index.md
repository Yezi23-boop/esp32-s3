# ESP-IDF v5.5.3 API Index

Use this index to locate the authoritative documentation for any ESP-IDF API. Keep function signatures, component availability, Kconfig options, and examples aligned with the project's installed ESP-IDF version.

## Complete Documentation

- Programming Guide home: https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32s3/
- API Reference index: https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32s3/api-reference/index.html
- API Guides index: https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32s3/api-guides/index.html
- Component source and public headers: https://github.com/espressif/esp-idf/tree/v5.5.3/components

## API Areas

Locate a component from the API Reference index first. It covers every public ESP-IDF component, including:

- Peripherals and drivers: GPIO, I2C, I2S, SPI, UART, ADC, LEDC, RMT, PCNT, timers, SD/MMC, USB, TWAI, Ethernet and MCPWM.
- System and runtime: FreeRTOS, heap capabilities, power management, sleep, event loop, logging, partitions, NVS, OTA, watchdogs, system, bootloader, console and VFS.
- Connectivity: Wi-Fi, Bluetooth LE, ESP-NETIF, networking protocols, HTTP client/server, MQTT, mDNS, provisioning and ESP-NOW.
- Storage, security and diagnostics: FATFS, SPIFFS, LittleFS, wear levelling, flash encryption, secure boot, mbedTLS, coredump, panic handler and application tracing.
- Platform features: ESP-DL, camera, LCD, touch, USB device/host, DSP, deep-sleep wake sources and SoC-specific capabilities.

## Lookup Rules

1. Use the installed ESP-IDF version, not `latest`, unless the user explicitly asks to target another version.
2. Confirm a public header and the component's Kconfig requirements before proposing an API call.
3. Prefer the component's official examples when initialization order, buffer ownership, callback context, or teardown behavior matters.
4. Treat HAL, LL, ROM and register headers as implementation details unless a documented exception requires them.
