# 86Box GPIO Support for Raspberry Pi

This document describes the GPIO support feature in 86Box that allows you to control physical LEDs on a Raspberry Pi to indicate hard disk activity.

## Overview

The GPIO support feature enables 86Box to control GPIO pins on a Raspberry Pi (or other Linux systems with GPIO sysfs support) to drive physical LEDs that indicate when the emulated hard disk drives are active. This provides a more realistic experience by giving you visual feedback of disk activity, similar to how real retro computers had HDD activity LEDs on their cases.

## Features

- **HDD Activity LED**: Lights up when any hard disk read or write operation occurs
- **HDD Write LED**: Lights up specifically when write operations occur
- **HDD Buzzer Clicks**: Produces realistic HDD clicking sounds ONLY on track changes (head seeks)
- **Authentic Behavior**: Buzzer silent during reads/writes within same track, just like real HDDs
- **Configurable GPIO pins**: Choose which GPIO pins to use for each LED and buzzer
- **Safe GPIO handling**: Properly exports, configures, and cleans up GPIO pins
- **Performance optimized**: Uses file descriptors for fast GPIO updates

## Requirements

- Linux system with GPIO sysfs support (typically `/sys/class/gpio/`)
- Raspberry Pi is the most common platform, but any Linux system with GPIO should work
- Appropriate permissions to access GPIO (usually requires running as root or being in the `gpio` group)
- LEDs and appropriate resistors connected to the chosen GPIO pins
- Optional: Active buzzer for HDD clicking sounds (recommended: 5V active buzzer with built-in oscillator)

## Hardware Setup

### Basic LED Circuit

For each LED you want to control:

1. Connect the long leg (anode) of the LED to the chosen GPIO pin
2. Connect a 220-330 ohm resistor between the short leg (cathode) of the LED and ground (GND)
3. Common GPIO pins on Raspberry Pi:
   - GPIO 18 (Physical pin 12) - commonly used for HDD activity LED
   - GPIO 19 (Physical pin 35) - commonly used for HDD write LED
   - GPIO 20 (Physical pin 38) - commonly used for HDD buzzer
   - Ground pins: Physical pins 6, 9, 14, 20, 25, 30, 34, 39

### Wiring Example

```
Raspberry Pi GPIO Header:
┌─────┬─────┐
│ 3V3 │ 5V  │ Pins 1-2
├─────┼─────┤
│ GP2 │ 5V  │ Pins 3-4
├─────┼─────┤
│ GP3 │ GND │ Pins 5-6  ← Connect LED cathode (via resistor)
├─────┼─────┤
│ GP4 │GP14 │ Pins 7-8
├─────┼─────┤
│ GND │GP15 │ Pins 9-10
├─────┼─────┤
│GP17 │GP18 │ Pins 11-12 ← Connect HDD Activity LED anode to Pin 12
├─────┼─────┤
│GP27 │GND  │ Pins 13-14
├─────┼─────┤
│GP22 │GP23 │ Pins 15-16
├─────┼─────┤
│3V3  │GP24 │ Pins 17-18
├─────┼─────┤
│GP10 │GND  │ Pins 19-20 ← Connect buzzer ground
├─────┼─────┤
│GP9  │GP25 │ Pins 21-22
├─────┼─────┤
│GP11 │GP8  │ Pins 23-24
├─────┼─────┤
│GND  │GP7  │ Pins 25-26
├─────┼─────┤
│ID_SD│ID_SC│ Pins 27-28
├─────┼─────┤
│GP5  │GND  │ Pins 29-30
├─────┼─────┤
│GP6  │GP12 │ Pins 31-32
├─────┼─────┤
│GP13 │GND  │ Pins 33-34
├─────┼─────┤
│GP19 │GP16 │ Pins 35-36 ← Pin 35 = HDD Write LED
├─────┼─────┤
│GP26 │GP20 │ Pins 37-38 ← Pin 38 = HDD Buzzer
├─────┼─────┤
│GND  │GP21 │ Pins 39-40 ← Connect buzzer ground here too
└─────┴─────┘

LED Connection:
GPIO 18 (Pin 12) ──── LED Anode (long leg)
                     LED Cathode (short leg) ──── 330Ω Resistor ──── GND (Pin 6)

Buzzer Connection:
GPIO 20 (Pin 38) ──── Buzzer Positive (+)
                     Buzzer Negative (-) ──── GND (Pin 39)
```

### Hardware Requirements

**For LEDs:**
- Standard 5mm LEDs (any color)
- 330Ω current-limiting resistors
- Breadboard or PCB for connections

**For Buzzer:**
- 5V Active Buzzer (recommended) - has built-in oscillator, produces tone when powered
- OR 3.3V Active Buzzer - works directly with Pi GPIO voltage
- Do NOT use passive buzzers - they require PWM signal generation

**Recommended Active Buzzers:**
- 5V Active Buzzer: More reliable, louder sound
- 3.3V Active Buzzer: Direct GPIO compatibility
- Look for "Active Buzzer" or "Buzzer with Oscillator" in specifications

## Software Configuration

Add the following settings to your 86Box configuration file (usually `86box.cfg`) under the `[Unix]` section:

**For Raspberry Pi 1-4:**
```ini
[Unix]
# Enable GPIO support (0 = disabled, 1 = enabled)
gpio_enabled = 1

# GPIO pin for HDD activity LED (any read or write operation)
# Use -1 to disable this LED
gpio_hdd_activity_pin = 18

# GPIO pin for HDD write LED (write operations only)
# Use -1 to disable this LED
gpio_hdd_write_pin = 19

# GPIO pin for HDD buzzer (clicking sounds)
# Use -1 to disable buzzer
gpio_hdd_buzzer_pin = 20

# Enable/disable buzzer sounds (0 = disabled, 1 = enabled)
gpio_buzzer_enabled = 1
```

**For Raspberry Pi 5:**
```ini
[Unix]
# Enable GPIO support (0 = disabled, 1 = enabled)
gpio_enabled = 1

# GPIO pin for HDD activity LED (any read or write operation)
# Use -1 to disable this LED
gpio_hdd_activity_pin = 589

# GPIO pin for HDD write LED (write operations only)
# Use -1 to disable this LED
gpio_hdd_write_pin = 590

# GPIO pin for HDD buzzer (clicking sounds)
# Use -1 to disable buzzer
gpio_hdd_buzzer_pin = 591

# Enable/disable buzzer sounds (0 = disabled, 1 = enabled)
gpio_buzzer_enabled = 1
```

### Configuration Options

| Setting | Description | Default | Valid Values |
|---------|-------------|---------|--------------|
| `gpio_enabled` | Enable or disable GPIO support | `0` | `0` (disabled), `1` (enabled) |
| `gpio_hdd_activity_pin` | GPIO pin for HDD activity LED | `-1` | `-1` (disabled) or valid GPIO pin number (see below) |
| `gpio_hdd_write_pin` | GPIO pin for HDD write LED | `-1` | `-1` (disabled) or valid GPIO pin number (see below) |
| `gpio_hdd_buzzer_pin` | GPIO pin for HDD buzzer | `-1` | `-1` (disabled) or valid GPIO pin number (see below) |
| `gpio_buzzer_enabled` | Enable or disable buzzer sounds | `0` | `0` (disabled), `1` (enabled) |

### GPIO Pin Mapping

**Raspberry Pi 1-4:**
- Physical pin 12 (GPIO18) = sysfs pin 18
- Physical pin 35 (GPIO19) = sysfs pin 19
- Physical pin 38 (GPIO20) = sysfs pin 20

**Raspberry Pi 5:**
- Physical pin 12 (GPIO18) = sysfs pin 589
- Physical pin 35 (GPIO19) = sysfs pin 590
- Physical pin 38 (GPIO20) = sysfs pin 591

To find the correct pin numbers for your system:
```bash
sudo cat /sys/kernel/debug/gpio | grep GPIO
```

## Permissions Setup

### Method 1: GPIO Group (Recommended)

1. Add your user to the gpio group:
   ```bash
   sudo usermod -a -G gpio $USER
   ```

2. Log out and log back in, or use:
   ```bash
   newgrp gpio
   ```

3. Create udev rules for GPIO access (create `/etc/udev/rules.d/99-gpio.rules`):
   ```
   SUBSYSTEM=="gpio", KERNEL=="gpiochip*", ACTION=="add", RUN+="/bin/chown root:gpio /sys/class/gpio/export /sys/class/gpio/unexport"
   SUBSYSTEM=="gpio", KERNEL=="gpio*", ACTION=="add", RUN+="/bin/chown root:gpio /sys%p/active_low /sys%p/direction /sys%p/edge /sys%p/value"
   ```

4. Reload udev rules:
   ```bash
   sudo udevadm control --reload-rules
   sudo udevadm trigger
   ```

### Method 2: Running as Root (Not Recommended)

You can run 86Box as root, but this is not recommended for security reasons:
```bash
sudo ./86Box
```

## Testing

1. Start 86Box with GPIO enabled
2. Check the log output for GPIO initialization messages:
   ```
   GPIO: Initializing GPIO support
   GPIO: Configuring HDD activity pin 18
   GPIO: HDD activity pin 18 configured successfully
   GPIO: Configuring HDD write pin 24
   GPIO: HDD write pin 24 configured successfully
   GPIO: GPIO initialization complete
   ```

3. Boot a guest OS and perform disk operations - the LEDs should light up during disk activity

## Troubleshooting

### Common Issues

**"GPIO sysfs not available"**
- Make sure you're running on a system with GPIO support
- Check if `/sys/class/gpio/` exists

**"Failed to open export for pin X"**
- Check permissions (see Permissions Setup above)
- Make sure you're not trying to use an invalid GPIO pin
- Some pins may be reserved by the system

**"Failed to export pin X"**
- The pin may already be in use by another application
- Try using a different GPIO pin
- Check `dmesg` for more detailed error messages

**LEDs don't light up**
- Verify hardware connections
- Check that the GPIO pins are correctly configured in the config file
- Test the LEDs manually:
  ```bash
  # Test GPIO 18
  echo 18 > /sys/class/gpio/export
  echo out > /sys/class/gpio/gpio18/direction
  echo 1 > /sys/class/gpio/gpio18/value  # LED should turn on
  echo 0 > /sys/class/gpio/gpio18/value  # LED should turn off
  echo 18 > /sys/class/gpio/unexport
  ```

### Debug Information

Enable more verbose logging by running 86Box with debug output to see GPIO-related messages.

## Advanced Usage

### Multiple Activity LEDs

You can use the same GPIO pin for both settings if you want a single LED that lights up for any disk activity:

```ini
[Unix]
gpio_enabled = 1
gpio_hdd_activity_pin = 18
gpio_hdd_write_pin = 18
```

### Different LED Colors

Use different colored LEDs for different activities:
- Green LED for general disk activity (`gpio_hdd_activity_pin`)
- Red LED for write activity (`gpio_hdd_write_pin`)

## Supported Systems

This feature is designed for and tested on:
- Raspberry Pi (all models with 40-pin GPIO header)
- Other ARM single-board computers with GPIO sysfs support
- x86 Linux systems with GPIO controllers

## Technical Details

- Uses Linux GPIO sysfs interface (`/sys/class/gpio/`)
- GPIO pins are exported on initialization and unexported on shutdown
- File descriptors are kept open for performance during operation
- GPIO operations are non-blocking and optimized for real-time response
- Supports both active-high and active-low LEDs (currently configured for active-high)

## Contributing

If you'd like to contribute to this feature:
- Source code is in `src/unix/unix_gpio.c` and `src/unix/unix_gpio.h`
- Integration is in `src/unix/unix.c`
- Follow the existing 86Box coding style and conventions

## License

This GPIO support feature is part of 86Box and is licensed under the same terms as the main project.
