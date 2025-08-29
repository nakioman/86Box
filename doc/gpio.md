# GPIO Support for 86Box

This document describes the GPIO support feature that allows 86Box to control external LEDs on a Raspberry Pi to indicate HDD activity.

## Overview

The GPIO feature enables 86Box to turn on/off LEDs connected to Raspberry Pi GPIO pins whenever hard disk read/write operations occur in the emulated system. This provides a physical indication of virtual disk activity.

## Requirements

- Raspberry Pi (any model with GPIO pins)
- LEDs and appropriate resistors (typically 330Ω for 3.3V GPIO)
- 86Box compiled with Unix/Linux support

## Hardware Setup

### Basic LED Circuit

Connect LEDs to your Raspberry Pi GPIO pins:

```
GPIO Pin → LED Anode → LED → Resistor (330Ω) → Ground
```

### Recommended GPIO Pins

- **GPIO 18** (Pin 12) - HDD Read Activity LED
- **GPIO 19** (Pin 35) - HDD Write Activity LED
- **Ground** (Pin 6, 9, 14, 20, 25, 30, 34, or 39)

### Wiring Diagram

```
Raspberry Pi GPIO Header (viewed from above)
┌─────────────────────────────────────────┐
│ 1  2  3  4  5  6  7  8  9 10 11 12 13 14│
│ 15 16 17 18 19 20 21 22 23 24 25 26 27 28│
│ 29 30 31 32 33 34 35 36 37 38 39 40      │
└─────────────────────────────────────────┘

Pin 12 (GPIO 18) → HDD Read LED → 330Ω → Pin 14 (Ground)
Pin 35 (GPIO 19) → HDD Write LED → 330Ω → Pin 39 (Ground)
```

## Configuration

Add the following settings to your 86Box configuration file (`86box.cfg`):

**For Raspberry Pi 1-4:**
```ini
[Unix]
gpio_enabled = 1
gpio_hdd_activity_pin = 18
gpio_hdd_write_pin = 19
```

**For Raspberry Pi 5:**
```ini
[Unix]
gpio_enabled = 1
gpio_hdd_activity_pin = 589
gpio_hdd_write_pin = 590
```

### Configuration Options

| Option | Description | Default | Valid Range |
|--------|-------------|---------|-------------|
| `gpio_enabled` | Enable/disable GPIO functionality | 0 (disabled) | 0 or 1 |
| `gpio_hdd_activity_pin` | GPIO pin for HDD activity LED | -1 (disabled) | See GPIO Pin Numbers below |
| `gpio_hdd_write_pin` | GPIO pin for HDD write activity LED | -1 (disabled) | See GPIO Pin Numbers below |

### GPIO Pin Numbers

**Raspberry Pi 1-4 (Legacy numbering):**
- GPIO 18 (Physical pin 12) = sysfs pin 18
- GPIO 19 (Physical pin 35) = sysfs pin 19
- GPIO 24 (Physical pin 18) = sysfs pin 24

**Raspberry Pi 5 (New numbering):**
- GPIO 18 (Physical pin 12) = sysfs pin 589
- GPIO 19 (Physical pin 35) = sysfs pin 590
- GPIO 24 (Physical pin 18) = sysfs pin 595

To find the correct pin numbers for your system, run:
```bash
sudo cat /sys/kernel/debug/gpio | grep "GPIO18\|GPIO19\|GPIO24"
```

## Usage

1. **Setup Hardware**: Connect LEDs to the configured GPIO pins
2. **Configure 86Box**: Add GPIO settings to configuration file
3. **Run with Permissions**: Ensure 86Box has GPIO access permissions
4. **Test**: Boot a virtual machine and observe LED activity during disk operations

### Running with GPIO Permissions

To allow 86Box to access GPIO without running as root:

```bash
# Add user to gpio group
sudo usermod -a -G gpio $USER

# Log out and back in, then run 86Box
./86Box
```

Alternatively, run as root (not recommended for regular use):

```bash
sudo ./86Box
```

## LED Behavior

- **HDD Read LED**: Lights up during virtual hard disk read operations
- **HDD Write LED**: Lights up during virtual hard disk write operations
- **Duration**: LEDs remain on for approximately 100ms per operation
- **Multiple Drives**: Activity on any configured HDD will trigger the LEDs

## Troubleshooting

### Common Issues

**LEDs don't light up:**
- Check GPIO permissions (`ls -l /sys/class/gpio/`)
- Verify wiring and LED polarity
- Confirm GPIO pins are not in use by other processes
- Check configuration file syntax
- **For Raspberry Pi 5**: Make sure you're using the correct pin numbers (589, 590, etc.)
- **For older Pi models**: Use traditional pin numbers (18, 19, etc.)

**"Failed to export pin X: Invalid argument":**
- Wrong pin number for your Pi model (see GPIO Pin Numbers section)
- Pin may be reserved or in use by another process
- Check available pins: `sudo cat /sys/kernel/debug/gpio`

**Permission denied errors:**
- Run with `sudo` or add user to `gpio` group
- Check `/sys/class/gpio` permissions

**Configuration not recognized:**
- Verify `[GPIO]` section exists in config file
- Check spelling of configuration options
- Ensure config file is in correct location

### Debugging

Enable debug logging to troubleshoot GPIO issues:

```bash
# Check if GPIO pins are exported
cat /sys/class/gpio/gpio18/direction
cat /sys/class/gpio/gpio19/direction

# Monitor GPIO sysfs activity
ls -la /sys/class/gpio/
```

## Technical Details

### Implementation

The GPIO support is implemented in:
- `src/unix/unix_gpio.c` - Core GPIO functionality
- `src/unix/unix_gpio.h` - GPIO interface definitions
- `src/qt/qt_ui.cpp` - Qt UI integration
- `src/unix/unix.c` - Unix platform integration

### GPIO Control Method

86Box uses the Linux sysfs GPIO interface:
- Export pins via `/sys/class/gpio/export`
- Set direction via `/sys/class/gpio/gpioN/direction`
- Control state via `/sys/class/gpio/gpioN/value`

### Performance Impact

GPIO operations are designed to be lightweight:
- Non-blocking file operations
- Minimal CPU overhead
- No impact on emulation performance

## Safety Notes

- Use appropriate current-limiting resistors with LEDs
- Don't exceed GPIO pin current limits (16mA per pin)
- Avoid short circuits between GPIO pins and ground/power
- GPIO pins output 3.3V, not 5V

## Building from Source

To build 86Box with GPIO support:

```bash
cd 86Box
mkdir build && cd build
cmake ..
make -j$(nproc)
```

GPIO support is automatically included in Unix/Linux builds.

## License

GPIO support is provided under the same license as 86Box.
