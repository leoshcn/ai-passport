# Standby constraints

The hardware power button is a power latch. It is not wired to the MCU. Firmware only receives UP, DOWN, and OK from the GPIO0 ADC ladder (`docs/hardware-design/specifications.zh_CN.md`, `components/bsp/include/bsp_pins.h`). Holding that power button for about 2 seconds still shuts the device down in hardware.

`main/demo_low_power.c` states that key wakeup is unused because the repository has no board-level wake circuit. Light sleep and deep sleep stop the ADC button sampler. A later function-key click cannot be seen until the chip is already awake. Deep sleep also reboots the application. `bsp_display_prepare_deep_sleep()` turns the panel off and then releases SPI pins; the next step must be deep sleep or restart.

Standby for this task is therefore backlight off while the CPU, button sampler, Wi-Fi, and the current screen state keep running. Wake is an ordinary button event, same as the existing 30-second dim path in `main/main.c`.

`bsp_button` already reports `BSP_BTN_DOUBLE`. The dashboard currently handles only clicks and OK long-press.

Product decision, 2026-10-02: standby is entered only by an OK click while the view is the dashboard. That click no longer requests a fetch. Settings keep their current OK-click actions, including Refresh. Failure, provision, and pairing do not enter standby. The press that leaves standby only restores the backlight.
