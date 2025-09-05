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

#ifndef GPIO_H
#define GPIO_H

#include <stdint.h>
#include <stdbool.h>
#include <gpiod.h>

/* Maximum number of GPIO pins that can be managed */
#define GPIO_MAX_PINS 32

/* GPIO pin types/purposes */
typedef enum {
    GPIO_PIN_TYPE_OUTPUT = 0,
    GPIO_PIN_TYPE_INPUT,
    GPIO_PIN_TYPE_INPUT_PULLUP,
    GPIO_PIN_TYPE_INPUT_PULLDOWN
} gpio_pin_type_t;

/* GPIO pin configuration */
typedef struct {
    uint32_t pin_number;           /* GPIO pin number */
    gpio_pin_type_t type;          /* Pin type (input/output) */
    bool active_high;              /* true = active high, false = active low */
    const char *consumer_name;     /* Name for this pin usage */
    bool initialized;              /* Whether this pin is initialized */
} gpio_pin_config_t;

/* GPIO pin handle */
typedef struct {
    uint32_t pin_number;           /* GPIO pin number */
    gpio_pin_type_t type;          /* Pin type */
    bool active_high;              /* Pin polarity */
    struct gpiod_line_request *request; /* GPIO line request handle */
    bool initialized;              /* Initialization status */
    char consumer_name[64];        /* Consumer name for debugging */
} gpio_pin_t;

/* GPIO system state */
typedef struct {
    struct gpiod_chip *chip;       /* GPIO chip handle */
    gpio_pin_t pins[GPIO_MAX_PINS]; /* Pin management array */
    uint32_t pin_count;            /* Number of configured pins */
    bool system_initialized;       /* System initialization status */
    char chip_path[256];           /* GPIO chip path */
} gpio_system_t;

/**
 * Initialize the GPIO system
 * @return 0 on success, -1 on error
 */
int gpio_init(void);

/**
 * Cleanup and release all GPIO resources
 */
void gpio_cleanup(void);

/**
 * Configure and initialize a GPIO pin
 * @param config Pin configuration structure
 * @return Pin handle ID (>=0) on success, -1 on error
 */
int gpio_configure_pin(const gpio_pin_config_t *config);

/**
 * Release a configured GPIO pin
 * @param pin_id Pin handle ID returned by gpio_configure_pin
 * @return 0 on success, -1 on error
 */
int gpio_release_pin(int pin_id);

/**
 * Set GPIO pin state (for output pins)
 * @param pin_id Pin handle ID
 * @param state Pin state (true = high/active, false = low/inactive)
 * @return 0 on success, -1 on error
 */
int gpio_set_pin(int pin_id, bool state);

/**
 * Get GPIO pin state (for input pins)
 * @param pin_id Pin handle ID
 * @param state Pointer to store pin state
 * @return 0 on success, -1 on error
 */
int gpio_get_pin(int pin_id, bool *state);

/**
 * Toggle GPIO pin state (for output pins)
 * @param pin_id Pin handle ID
 * @return 0 on success, -1 on error
 */
int gpio_toggle_pin(int pin_id);

/**
 * Check if GPIO system is initialized
 * @return true if initialized, false otherwise
 */
bool gpio_is_initialized(void);

/**
 * Check if a specific pin is configured
 * @param pin_id Pin handle ID
 * @return true if configured, false otherwise
 */
bool gpio_pin_is_configured(int pin_id);

/**
 * Get pin configuration information
 * @param pin_id Pin handle ID
 * @param config Pointer to store configuration
 * @return 0 on success, -1 on error
 */
int gpio_get_pin_config(int pin_id, gpio_pin_config_t *config);

/* Logging support */
#ifdef ENABLE_GPIO_LOG
int gpio_do_log;
void gpio_log(const char *fmt, ...);
#else
#define gpio_log(fmt, ...)
#endif

#endif /* GPIO_H */
