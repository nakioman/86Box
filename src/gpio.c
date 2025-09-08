/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          GPIO management for Unix systems - Generic GPIO interface
 *          that supports multiple pins for different peripherals.
 *
 * Author:  nacho
 *
 *          Copyright 2025 nacho.
 */

#define HAVE_STDARG_H
#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <errno.h>
#include <gpiod.h>
#include <86box/ini.h>
#include <86box/config.h>
#include <86box/86box.h>
#include <86box/gpio.h>

#ifdef ENABLE_GPIO_LOG
int gpio_do_log = ENABLE_GPIO_LOG;

void
gpio_log(const char *fmt, ...)
{
    va_list ap;

    if (gpio_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
#else
#    define gpio_log(fmt, ...)
#endif

/* Global GPIO system instance */
static gpio_system_t gpio_system;

/* Default GPIO chip path */
#define DEFAULT_GPIO_CHIP "/dev/gpiochip0"

/**
 * Initialize the GPIO system
 */
int
gpio_init(void)
{
    memset(&gpio_system, 0, sizeof(gpio_system));

    /* Check if GPIO is enabled in configuration */
    if (config_get_int("Unix", "gpio_enabled", 0) == 0) {
        gpio_log("GPIO disabled in configuration\n");
        return -1;
    }

    /* Get GPIO chip path from configuration */
    const char *chip_path = config_get_string("Unix", "gpio_chip", DEFAULT_GPIO_CHIP);
    strncpy(gpio_system.chip_path, chip_path, sizeof(gpio_system.chip_path) - 1);

    /* Open GPIO chip */
    gpio_system.chip = gpiod_chip_open(chip_path);
    if (!gpio_system.chip) {
        gpio_log("Failed to open GPIO chip '%s': %s\n", chip_path, strerror(errno));
        return -1;
    }

    gpio_system.system_initialized = true;
    gpio_system.pin_count = 0;

    gpio_log("GPIO system initialized using chip '%s'\n", chip_path);
    return 0;
}

/**
 * Cleanup and release all GPIO resources
 */
void
gpio_cleanup(void)
{
    if (!gpio_system.system_initialized) {
        return;
    }

    gpio_log("Cleaning up GPIO system\n");

    /* Release all configured pins */
    for (uint32_t i = 0; i < gpio_system.pin_count; i++) {
        if (gpio_system.pins[i].initialized && gpio_system.pins[i].request) {
            /* Set output pins to inactive state before releasing */
            if (gpio_system.pins[i].type == GPIO_PIN_TYPE_OUTPUT) {
                const enum gpiod_line_value values[1] = { GPIOD_LINE_VALUE_INACTIVE };
                gpiod_line_request_set_values(gpio_system.pins[i].request, values);
            }
            
            gpiod_line_request_release(gpio_system.pins[i].request);
            gpio_system.pins[i].request = NULL;
            gpio_system.pins[i].initialized = false;
        }
    }

    /* Close GPIO chip */
    if (gpio_system.chip) {
        gpiod_chip_close(gpio_system.chip);
        gpio_system.chip = NULL;
    }

    gpio_system.system_initialized = false;
    gpio_system.pin_count = 0;
    memset(&gpio_system, 0, sizeof(gpio_system));

    gpio_log("GPIO system cleaned up\n");
}

/**
 * Configure and initialize a GPIO pin
 */
int
gpio_configure_pin(const gpio_pin_config_t *config)
{
    if (!gpio_system.system_initialized) {
        gpio_log("GPIO system not initialized\n");
        return -1;
    }

    if (!config) {
        gpio_log("Invalid pin configuration\n");
        return -1;
    }

    if (gpio_system.pin_count >= GPIO_MAX_PINS) {
        gpio_log("Maximum number of GPIO pins reached (%d)\n", GPIO_MAX_PINS);
        return -1;
    }

    /* Check if pin is already configured */
    for (uint32_t i = 0; i < gpio_system.pin_count; i++) {
        if (gpio_system.pins[i].initialized && 
            gpio_system.pins[i].pin_number == config->pin_number) {
            gpio_log("GPIO pin %u already configured\n", config->pin_number);
            return -1;
        }
    }

    /* Find next available slot */
    int pin_id = -1;
    for (uint32_t i = 0; i < GPIO_MAX_PINS; i++) {
        if (!gpio_system.pins[i].initialized) {
            pin_id = i;
            break;
        }
    }

    if (pin_id == -1) {
        gpio_log("No available GPIO pin slots\n");
        return -1;
    }

    gpio_pin_t *pin = &gpio_system.pins[pin_id];

    /* Create line settings */
    struct gpiod_line_settings *settings = gpiod_line_settings_new();
    if (!settings) {
        gpio_log("Failed to create line settings: %s\n", strerror(errno));
        return -1;
    }

    /* Configure direction based on pin type */
    switch (config->type) {
        case GPIO_PIN_TYPE_OUTPUT:
            gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_OUTPUT);
            break;
        case GPIO_PIN_TYPE_INPUT:
            gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_INPUT);
            break;
        case GPIO_PIN_TYPE_INPUT_PULLUP:
            gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_INPUT);
            gpiod_line_settings_set_bias(settings, GPIOD_LINE_BIAS_PULL_UP);
            break;
        case GPIO_PIN_TYPE_INPUT_PULLDOWN:
            gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_INPUT);
            gpiod_line_settings_set_bias(settings, GPIOD_LINE_BIAS_PULL_DOWN);
            break;
        default:
            gpio_log("Invalid GPIO pin type: %d\n", config->type);
            gpiod_line_settings_free(settings);
            return -1;
    }

    /* Create line configuration */
    struct gpiod_line_config *line_cfg = gpiod_line_config_new();
    if (!line_cfg) {
        gpio_log("Failed to create line config: %s\n", strerror(errno));
        gpiod_line_settings_free(settings);
        return -1;
    }

    /* Add line configuration */
    if (gpiod_line_config_add_line_settings(line_cfg, &config->pin_number, 1, settings) < 0) {
        gpio_log("Failed to add line settings for pin %u: %s\n", config->pin_number, strerror(errno));
        gpiod_line_config_free(line_cfg);
        gpiod_line_settings_free(settings);
        return -1;
    }

    /* Create request configuration */
    struct gpiod_request_config *req_cfg = gpiod_request_config_new();
    if (req_cfg && config->consumer_name) {
        gpiod_request_config_set_consumer(req_cfg, config->consumer_name);
    }

    /* Request GPIO line */
    pin->request = gpiod_chip_request_lines(gpio_system.chip, req_cfg, line_cfg);

    /* Cleanup temporary objects */
    if (req_cfg) {
        gpiod_request_config_free(req_cfg);
    }
    gpiod_line_config_free(line_cfg);
    gpiod_line_settings_free(settings);

    if (!pin->request) {
        gpio_log("Failed to request GPIO line %u: %s\n", config->pin_number, strerror(errno));
        return -1;
    }

    /* Initialize pin structure */
    pin->pin_number = config->pin_number;
    pin->type = config->type;
    pin->active_high = config->active_high;
    pin->initialized = true;
    strncpy(pin->consumer_name, config->consumer_name ? config->consumer_name : "Unknown", 
            sizeof(pin->consumer_name) - 1);

    /* Update pin count if this is a new slot */
    if (pin_id >= (int)gpio_system.pin_count) {
        gpio_system.pin_count = pin_id + 1;
    }

    gpio_log("GPIO pin %u configured as %s (%s) - assigned ID %d\n", 
             config->pin_number,
             (config->type == GPIO_PIN_TYPE_OUTPUT) ? "output" : "input",
             config->active_high ? "active-high" : "active-low",
             pin_id);

    return pin_id;
}

/**
 * Release a configured GPIO pin
 */
int
gpio_release_pin(int pin_id)
{
    if (!gpio_system.system_initialized) {
        gpio_log("GPIO system not initialized\n");
        return -1;
    }

    if (pin_id < 0 || pin_id >= GPIO_MAX_PINS) {
        gpio_log("Invalid GPIO pin ID: %d\n", pin_id);
        return -1;
    }

    gpio_pin_t *pin = &gpio_system.pins[pin_id];
    if (!pin->initialized) {
        gpio_log("GPIO pin ID %d not configured\n", pin_id);
        return -1;
    }

    /* Set output pin to inactive before releasing */
    if (pin->type == GPIO_PIN_TYPE_OUTPUT && pin->request) {
        const enum gpiod_line_value values[1] = { GPIOD_LINE_VALUE_INACTIVE };
        gpiod_line_request_set_values(pin->request, values);
    }

    /* Release GPIO line */
    if (pin->request) {
        gpiod_line_request_release(pin->request);
        pin->request = NULL;
    }

    gpio_log("Released GPIO pin %u (ID %d)\n", pin->pin_number, pin_id);

    /* Clear pin data */
    memset(pin, 0, sizeof(*pin));

    return 0;
}

/**
 * Set GPIO pin state (for output pins)
 */
int
gpio_set_pin(int pin_id, bool state)
{
    if (!gpio_system.system_initialized) {
        return -1;
    }

    if (pin_id < 0 || pin_id >= GPIO_MAX_PINS) {
        return -1;
    }

    gpio_pin_t *pin = &gpio_system.pins[pin_id];
    if (!pin->initialized || !pin->request) {
        return -1;
    }

    if (pin->type != GPIO_PIN_TYPE_OUTPUT) {
        gpio_log("Attempt to set state on non-output pin %u\n", pin->pin_number);
        return -1;
    }

    /* Apply polarity logic */
    bool physical_state = pin->active_high ? state : !state;
    const enum gpiod_line_value values[1] = { 
        physical_state ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE 
    };
    
    int ret = gpiod_line_request_set_values(pin->request, values);
    return (ret < 0) ? -1 : 0;
}

/**
 * Get GPIO pin state (for input pins)
 */
int
gpio_get_pin(int pin_id, bool *state)
{
    if (!gpio_system.system_initialized || !state) {
        return -1;
    }

    if (pin_id < 0 || pin_id >= GPIO_MAX_PINS) {
        return -1;
    }

    gpio_pin_t *pin = &gpio_system.pins[pin_id];
    if (!pin->initialized || !pin->request) {
        return -1;
    }

    if (pin->type == GPIO_PIN_TYPE_OUTPUT) {
        gpio_log("Attempt to read state from output pin %u\n", pin->pin_number);
        return -1;
    }

    enum gpiod_line_value values[1];
    int ret = gpiod_line_request_get_values(pin->request, values);
    if (ret < 0) {
        return -1;
    }

    /* Apply polarity logic */
    bool physical_state = (values[0] == GPIOD_LINE_VALUE_ACTIVE);
    *state = pin->active_high ? physical_state : !physical_state;
    
    return 0;
}

/**
 * Toggle GPIO pin state (for output pins)
 */
int
gpio_toggle_pin(int pin_id)
{
    if (!gpio_system.system_initialized) {
        return -1;
    }

    if (pin_id < 0 || pin_id >= GPIO_MAX_PINS) {
        return -1;
    }

    gpio_pin_t *pin = &gpio_system.pins[pin_id];
    if (!pin->initialized || !pin->request) {
        return -1;
    }

    if (pin->type != GPIO_PIN_TYPE_OUTPUT) {
        return -1;
    }

    /* For toggle, we need to read current state first */
    enum gpiod_line_value current_values[1];
    int ret = gpiod_line_request_get_values(pin->request, current_values);
    if (ret < 0) {
        return -1;
    }

    /* Toggle the physical state */
    const enum gpiod_line_value new_values[1] = { 
        (current_values[0] == GPIOD_LINE_VALUE_ACTIVE) ? 
        GPIOD_LINE_VALUE_INACTIVE : GPIOD_LINE_VALUE_ACTIVE 
    };
    
    ret = gpiod_line_request_set_values(pin->request, new_values);
    return (ret < 0) ? -1 : 0;
}

/**
 * Check if GPIO system is initialized
 */
bool
gpio_is_initialized(void)
{
    return gpio_system.system_initialized;
}

/**
 * Check if a specific pin is configured
 */
bool
gpio_pin_is_configured(int pin_id)
{
    if (pin_id < 0 || pin_id >= GPIO_MAX_PINS) {
        return false;
    }
    
    return gpio_system.pins[pin_id].initialized;
}

/**
 * Get pin configuration information
 */
int
gpio_get_pin_config(int pin_id, gpio_pin_config_t *config)
{
    if (!gpio_system.system_initialized || !config) {
        return -1;
    }

    if (pin_id < 0 || pin_id >= GPIO_MAX_PINS) {
        return -1;
    }

    gpio_pin_t *pin = &gpio_system.pins[pin_id];
    if (!pin->initialized) {
        return -1;
    }

    config->pin_number = pin->pin_number;
    config->type = pin->type;
    config->active_high = pin->active_high;
    config->consumer_name = pin->consumer_name;
    config->initialized = pin->initialized;

    return 0;
}