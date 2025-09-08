/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          GPIO-based HDD activity LED for Raspberry Pi.
 *          This provides visual HDD activity indication by controlling
 *          an LED connected to a GPIO pin.
 *
 * Author:  nacho
 *
 *          Copyright 2025 nacho.
 */

#define HAVE_STDARG_H
#define _POSIX_C_SOURCE 200809L

#include <86box/hdd_led.h>
#include <86box/gpio.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <stdarg.h>
#include <86box/86box.h>
#include <86box/timer.h>
#include <86box/thread.h>
#include <86box/ini.h>
#include <86box/config.h>
#include <errno.h>

#ifdef ENABLE_HDD_LED_LOG
int hdd_led_do_log = ENABLE_HDD_LED_LOG;

static void
hdd_led_log(const char *fmt, ...)
{
    va_list ap;

    if (hdd_led_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
#else
#    define hdd_led_log(fmt, ...)
#endif

/* Global HDD LED instance */
static hdd_led_t global_hdd_led;

/* Set LED state */
static int set_led_pin(bool state) {
    if (!global_hdd_led.initialized || global_hdd_led.gpio_pin_id < 0) {
        return -1;
    }
    
    return gpio_set_pin(global_hdd_led.gpio_pin_id, state);
}

/* Initialize HDD LED system */
int hdd_led_init(void) {
    memset(&global_hdd_led, 0, sizeof(global_hdd_led));
    global_hdd_led.gpio_pin_id = -1;

    /* Check configuration settings */
    if (config_get_int("Unix", "gpio_enabled", 0) == 0) {
        hdd_led_log("GPIO disabled in configuration, HDD LED not initialized\n");
        return -1;
    }

    if (config_get_int("Unix", "hdd_led_enabled", 0) == 0) {
        hdd_led_log("HDD LED disabled in configuration, LED not initialized\n");
        return -1;
    }

    global_hdd_led.config.led_pin = config_get_int("Unix", "hdd_led_gpio_pin", DEFAULT_HDD_LED_PIN);
    global_hdd_led.config.enabled = true;

    /* Initialize GPIO system if not already initialized */
    if (!gpio_is_initialized()) {
        if (gpio_init() != 0) {
            hdd_led_log("Failed to initialize GPIO system\n");
            return -1;
        }
    }

    /* Configure GPIO pin for LED */
    gpio_pin_config_t pin_config = {
        .pin_number = global_hdd_led.config.led_pin,
        .type = GPIO_PIN_TYPE_OUTPUT,
        .active_high = true,  /* Assuming active high for LED */
        .consumer_name = "86Box HDD Activity LED",
        .initialized = false
    };

    global_hdd_led.gpio_pin_id = gpio_configure_pin(&pin_config);
    if (global_hdd_led.gpio_pin_id < 0) {
        hdd_led_log("Failed to configure GPIO pin %u for HDD LED\n", 
                   global_hdd_led.config.led_pin);
        return -1;
    }

    global_hdd_led.initialized = true;

    /* Ensure LED starts in off state */
    set_led_pin(false);

    hdd_led_log("HDD LED initialized on GPIO %u (pin ID %d)\n", 
               global_hdd_led.config.led_pin, global_hdd_led.gpio_pin_id);
    return 0;
}

/* Cleanup HDD LED resources */
void hdd_led_cleanup(void) {
    if (!global_hdd_led.initialized) {
        return;
    }

    hdd_led_log("Cleaning up HDD LED\n");

    /* Ensure LED is off and release GPIO pin */
    if (global_hdd_led.gpio_pin_id >= 0) {
        set_led_pin(false);
        gpio_release_pin(global_hdd_led.gpio_pin_id);
        global_hdd_led.gpio_pin_id = -1;
    }

    global_hdd_led.initialized = false;
    memset(&global_hdd_led, 0, sizeof(global_hdd_led));
    hdd_led_log("HDD LED cleaned up\n");
}

/* Set HDD LED state manually */
void hdd_led_set_state(bool state) {
    if (!global_hdd_led.initialized || !global_hdd_led.config.enabled) {
        return;
    }

    set_led_pin(state);

    hdd_led_log("HDD LED set to %s\n", state ? "ON" : "OFF");
}
