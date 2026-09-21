# ESP32-S3 Peripherals and Debug Reference

## Use This File For

- GPIO, I2C, SPI, UART, and I2S basics
- ESP32-S3 memory and RTOS tradeoff discussions
- Peripheral tradeoff discussions
- Shared-bus reasoning
- FreeRTOS basics for simple firmware debugging
- First-pass fault isolation

## Memory Basics

- Keep internal memory constraints visible in explanations:
  - IRAM for time-critical code paths
  - DRAM for internal data and DMA-sensitive buffers
  - PSRAM for capacity, not every hot path
- For runtime problems, ask whether the failing buffer or task stack must stay in internal memory.

## GPIO

- Check strapping and boot-sensitive pins before assigning runtime functions.
- Confirm direction, pull-up/pull-down, drive strength, and active level.
- Separate logical meaning from electrical level in explanations.

## I2C

- Check pull-ups, device address, bus speed, and voltage domain first.
- Use a bus scan only as a rough health check; it does not prove full protocol compatibility.
- If one device works and another does not, compare address, clock stretching, and init order.

## SPI

- Check mode, frequency, chip select, and DMA usage.
- Separate bus-wide settings from device-specific settings.
- When multiple devices share the bus, verify CS handling and timing margins.

## UART

- Distinguish bootloader logs, app logs, and application protocol traffic.
- Confirm baud rate and which UART instance is used for console versus device traffic.
- Flash issues often come from wiring, power, or boot mode rather than software.

## I2S and Audio

- Always confirm:
  - Master or slave ownership
  - Sample rate
  - Bit width
  - Slot or channel mode
  - Whether playback and capture share one bus
- If playback and capture share one I2S bus, prefer one fixed bus format when both sides may stay alive.
- If the bus format must change, ensure the inactive side is fully stopped and reconfigured before switching clocks.
- Do not assume that stopping only upper-layer reads means the I2S pipeline is inactive.

## FreeRTOS Basics

- Check task priority, stack size, blocking behavior, and queue depth before blaming hardware.
- Watch for starvation, watchdog resets, and hidden producer-consumer mismatches.
- Prefer logs around task transitions and peripheral state changes for timing bugs.

## Fault-Isolation Order

1. Reproduce with the smallest possible path.
2. Confirm build and config are sane.
3. Confirm pin and bus ownership.
4. Confirm clocks and formats.
5. Confirm tasking and timing.
6. Only then optimize.

## Common Debug Patterns

### Peripheral Does Not Respond

- Verify pins, power, address or mode, and init order.
- Compare expected versus actual clocking.

### Audio Acts Strange

- Check shared bus ownership first.
- Check sample rate, bit width, and slot mapping consistency.
- Check whether one path is still alive while another path changes the I2S clock.

### Runtime or Memory Failure

- Check task stack size, queue depth, blocking path, and allocation site.
- Distinguish stack overflow, watchdog starvation, and heap exhaustion before suggesting fixes.
