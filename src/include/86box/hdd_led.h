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
 *          Configuration (86box.cfg):
 *          [Unix]
 *          gpio_enabled = 1                 ; Enable GPIO functionality (0/1)
 *          hdd_led_enabled = 1              ; Enable HDD activity LED (0/1)
 *          hdd_led_gpio_pin = 21            ; GPIO pin for LED (0-53)
 *
 * Author:  nacho
 *
 *          Copyright 2025 nacho.
 */

#ifndef HDD_LED_H
#define HDD_LED_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

/* Configuration constants */
#define DEFAULT_HDD_LED_PIN 21     /* GPIO21 (Pin 40) */

/* HDD LED configuration structure */
typedef struct {
    uint32_t led_pin;           /* GPIO pin number for LED */
    uint32_t duration_ms;       /* LED on duration in milliseconds */
    bool enabled;               /* LED enabled flag */
} hdd_led_config_t;

/* HDD LED control structure */
typedef struct {
    int gpio_pin_id;            /* GPIO pin ID from gpio system */
    bool initialized;           /* Initialization flag */
    hdd_led_config_t config;    /* Configuration */
} hdd_led_t;

/* Function prototypes */

/**
 * Initialize the HDD LED system
 * @return 0 on success, -1 on error
 */
int hdd_led_init(void);

/**
 * Cleanup and release HDD LED resources
 */
void hdd_led_cleanup(void);

/**
 * Set HDD LED state manually
 * @param state LED state (true = on, false = off)
 */
void hdd_led_set_state(bool state);

#endif /* HDD_LED_H */
