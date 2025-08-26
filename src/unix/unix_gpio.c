/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Linux/Raspberry Pi GPIO support for hardware indication.
 *
 *
 *
 * Authors: Your Name, <your.email@example.com>
 *
 *          Copyright 2025 Your Name.
 */
#ifdef __linux__
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <86box/86box.h>
#include <86box/config.h>
#include <86box/ini.h>
#include <86box/ui.h>
#include "unix_gpio.h"

#define GPIO_SYSFS_BASE "/sys/class/gpio"
#define GPIO_EXPORT_PATH "/sys/class/gpio/export"
#define GPIO_UNEXPORT_PATH "/sys/class/gpio/unexport"

/* GPIO configuration */
static int gpio_hdd_activity_pin = -1;
static int gpio_hdd_write_pin = -1;
static int gpio_enabled = 0;
static int gpio_exported[2] = {0, 0}; /* Track exported GPIOs */

/* GPIO file handles for performance */
static int gpio_hdd_activity_fd = -1;
static int gpio_hdd_write_fd = -1;

static void
gpio_list_exported_pins(void)
{
    char cmd[256];
    pclog("GPIO: Checking currently exported pins:\n");
    snprintf(cmd, sizeof(cmd), "ls -la %s/gpio* 2>/dev/null || echo 'No GPIO pins currently exported'", GPIO_SYSFS_BASE);
    system(cmd);
}

static int
gpio_export(int pin)
{
    char path[256];
    int fd;
    
    if (pin < 0)
        return 0;
    
    /* Check if already exported */
    snprintf(path, sizeof(path), "%s/gpio%d", GPIO_SYSFS_BASE, pin);
    if (access(path, F_OK) == 0) {
        pclog("GPIO: Pin %d already exported, checking if accessible\n", pin);
        return 1; /* Already exported */
    }
    
    pclog("GPIO: Attempting to export pin %d\n", pin);
    
    /* Export the GPIO */
    fd = open(GPIO_EXPORT_PATH, O_WRONLY);
    if (fd < 0) {
        pclog("GPIO: Failed to open export file: %s\n", strerror(errno));
        pclog("GPIO: Make sure you have GPIO permissions (try: sudo usermod -a -G gpio $USER)\n");
        return 0;
    }
    
    snprintf(path, sizeof(path), "%d", pin);
    if (write(fd, path, strlen(path)) < 0) {
        if (errno == EINVAL) {
            pclog("GPIO: Pin %d is invalid or already in use by another process\n", pin);
            pclog("GPIO: Try a different pin number or check: cat /sys/kernel/debug/gpio\n");
        } else if (errno == EBUSY) {
            pclog("GPIO: Pin %d is busy/already exported\n", pin);
            /* Check if it exists now */
            close(fd);
            snprintf(path, sizeof(path), "%s/gpio%d", GPIO_SYSFS_BASE, pin);
            if (access(path, F_OK) == 0) {
                pclog("GPIO: Pin %d became available, continuing\n", pin);
                return 1;
            }
        } else {
            pclog("GPIO: Failed to export pin %d: %s (errno=%d)\n", pin, strerror(errno), errno);
        }
        close(fd);
        return 0;
    }
    
    close(fd);
    
    /* Wait a bit for the export to complete */
    usleep(100000); /* 100ms */
    
    /* Verify the export worked */
    snprintf(path, sizeof(path), "%s/gpio%d", GPIO_SYSFS_BASE, pin);
    if (access(path, F_OK) != 0) {
        pclog("GPIO: Pin %d export failed - directory not created\n", pin);
        return 0;
    }
    
    pclog("GPIO: Pin %d exported successfully\n", pin);
    return 1;
}

static int
gpio_set_direction(int pin, const char *direction)
{
    char path[256];
    int fd;
    
    if (pin < 0)
        return 0;
    
    snprintf(path, sizeof(path), "%s/gpio%d/direction", GPIO_SYSFS_BASE, pin);
    fd = open(path, O_WRONLY);
    if (fd < 0) {
        pclog("GPIO: Failed to open direction for pin %d: %s\n", pin, strerror(errno));
        return 0;
    }
    
    if (write(fd, direction, strlen(direction)) < 0) {
        pclog("GPIO: Failed to set direction for pin %d: %s\n", pin, strerror(errno));
        close(fd);
        return 0;
    }
    
    close(fd);
    return 1;
}

static int
gpio_open_value_fd(int pin)
{
    char path[256];
    if (pin < 0) {
        pclog("GPIO: gpio_open_value_fd called with invalid pin %d\n", pin);
        return -1;
    }
    snprintf(path, sizeof(path), "%s/gpio%d/value", GPIO_SYSFS_BASE, pin);
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        pclog("GPIO: Failed to open value for pin %d: %s\n", pin, strerror(errno));
    } else {
        pclog("GPIO: Value fd for pin %d opened successfully\n", pin);
    }
    return fd;
}

static void
gpio_set_value_fd(int fd, int value)
{
    char val = value ? '1' : '0';
    
    if (fd >= 0) {
        lseek(fd, 0, SEEK_SET);
        write(fd, &val, 1);
    }
}

static void
gpio_unexport(int pin)
{
    char path[256];
    int fd;
    
    if (pin < 0)
        return;
    
    fd = open(GPIO_UNEXPORT_PATH, O_WRONLY);
    if (fd < 0)
        return;
    
    snprintf(path, sizeof(path), "%d", pin);
    write(fd, path, strlen(path));
    close(fd);
}

int
unix_gpio_init(void)
{
    /* Check if we're running on Linux and GPIO is available */
    if (access(GPIO_SYSFS_BASE, F_OK) != 0) {
        pclog("GPIO: GPIO sysfs not available, GPIO support disabled\n");
        return 0;
    }
    
    /* Debug: list currently exported pins */
    gpio_list_exported_pins();
    
    /* Get configuration */
    gpio_enabled = config_get_int("Unix", "gpio_enabled", 0);
    if (!gpio_enabled) {
        pclog("GPIO: GPIO support disabled in configuration\n");
        return 0;
    }
    
    gpio_hdd_activity_pin = config_get_int("Unix", "gpio_hdd_activity_pin", -1);
    gpio_hdd_write_pin = config_get_int("Unix", "gpio_hdd_write_pin", -1);
    
    if (gpio_hdd_activity_pin < 0 && gpio_hdd_write_pin < 0) {
        pclog("GPIO: No GPIO pins configured\n");
        return 0;
    }
    
    pclog("GPIO: Initializing GPIO support\n");
    
    /* Initialize HDD activity pin */
    if (gpio_hdd_activity_pin >= 0) {
        pclog("GPIO: Configuring HDD activity pin %d\n", gpio_hdd_activity_pin);
        if (gpio_export(gpio_hdd_activity_pin) &&
            gpio_set_direction(gpio_hdd_activity_pin, "out")) {
            gpio_hdd_activity_fd = gpio_open_value_fd(gpio_hdd_activity_pin);
            if (gpio_hdd_activity_fd >= 0) {
                gpio_exported[0] = 1;
                gpio_set_value_fd(gpio_hdd_activity_fd, 0); /* Start with LED off */
                pclog("GPIO: HDD activity pin %d configured successfully\n", gpio_hdd_activity_pin);
            } else {
                pclog("GPIO: Failed to open value file for HDD activity pin %d\n", gpio_hdd_activity_pin);
            }
        }
    }
    
    /* Initialize HDD write pin */
    if (gpio_hdd_write_pin >= 0) {
        pclog("GPIO: Configuring HDD write pin %d\n", gpio_hdd_write_pin);
        if (gpio_export(gpio_hdd_write_pin) &&
            gpio_set_direction(gpio_hdd_write_pin, "out")) {
            gpio_hdd_write_fd = gpio_open_value_fd(gpio_hdd_write_pin);
            if (gpio_hdd_write_fd >= 0) {
                gpio_exported[1] = 1;
                gpio_set_value_fd(gpio_hdd_write_fd, 0); /* Start with LED off */
                pclog("GPIO: HDD write pin %d configured successfully\n", gpio_hdd_write_pin);
            } else {
                pclog("GPIO: Failed to open value file for HDD write pin %d\n", gpio_hdd_write_pin);
            }
        }
    }
    
    pclog("GPIO: GPIO initialization complete\n");
    return 1;
}

void
unix_gpio_close(void)
{
    if (!gpio_enabled)
        return;
    
    pclog("GPIO: Shutting down GPIO support\n");
    
    /* Turn off LEDs */
    if (gpio_hdd_activity_fd >= 0) {
        gpio_set_value_fd(gpio_hdd_activity_fd, 0);
        close(gpio_hdd_activity_fd);
        gpio_hdd_activity_fd = -1;
    }
    
    if (gpio_hdd_write_fd >= 0) {
        gpio_set_value_fd(gpio_hdd_write_fd, 0);
        close(gpio_hdd_write_fd);
        gpio_hdd_write_fd = -1;
    }
    
    /* Unexport GPIOs */
    if (gpio_exported[0] && gpio_hdd_activity_pin >= 0) {
        gpio_unexport(gpio_hdd_activity_pin);
        gpio_exported[0] = 0;
    }
    
    if (gpio_exported[1] && gpio_hdd_write_pin >= 0) {
        gpio_unexport(gpio_hdd_write_pin);
        gpio_exported[1] = 0;
    }
    
    gpio_enabled = 0;
}

void
unix_gpio_hdd_activity(int active)
{
    if (!gpio_enabled || gpio_hdd_activity_fd < 0)
        return;
    
    gpio_set_value_fd(gpio_hdd_activity_fd, active);
}

void
unix_gpio_hdd_write(int active)
{
    if (!gpio_enabled || gpio_hdd_write_fd < 0)
        return;
    
    gpio_set_value_fd(gpio_hdd_write_fd, active);
}

#else
/* Non-Linux stub implementations */
int
unix_gpio_init(void)
{
    return 0;
}

void
unix_gpio_close(void)
{
}

void
unix_gpio_hdd_activity(int active)
{
}

void
unix_gpio_hdd_write(int active)
{
}
#endif
