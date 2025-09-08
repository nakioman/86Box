/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          GPIO-based hard disk drive sound emulation for Raspberry Pi.
 *          This provides authentic HDD activity sounds by controlling
 *          a physical buzzer connected to GPIO pins.
 *
 *          Configuration (86box.cfg):
 *          [Unix]
 *          gpio_enabled = 1                 ; Enable GPIO functionality (0/1)
 *          hdd_buzzer_enabled = 1           ; Enable HDD buzzer (0/1)
 *          hdd_buzzer_gpio_pin = 19         ; GPIO pin for HDD buzzer (0-53)
 *
 * Author:  nacho
 *
 *          Copyright 2025 nacho.
 */

#ifndef HDD_BUZZER_H
#define HDD_BUZZER_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

/* Configuration constants */
#define DEFAULT_HDD_BUZZER_PIN 19     /* GPIO19 (Pin 35) */
#define DEFAULT_HDD_BUZZER_VOLUME 3   /* Volume level 1-10 */

/* HDD buzzer states */
typedef enum {
    HDD_BUZZER_STATE_IDLE = 0,
    HDD_BUZZER_STATE_SEEKING,
    HDD_BUZZER_STATE_READING,
    HDD_BUZZER_STATE_WRITING
} hdd_buzzer_state_t;

/* HDD buzzer configuration structure */
typedef struct {
    uint8_t volume;         /* Buzzer volume (0=off, 1-10) */
    uint32_t buzzer_pin;    /* GPIO pin number for buzzer */
    bool enabled;           /* Buzzer enabled flag */
} hdd_buzzer_config_t;

/* HDD buzzer control structure */
typedef struct {
    int gpio_pin_id;                /* GPIO pin ID from gpio system */
    hdd_buzzer_state_t state;       /* Current buzzer state */
    struct timespec start_time;     /* Start time for activity timing */
    bool initialized;               /* Initialization flag */
    hdd_buzzer_config_t config;     /* Configuration */
} hdd_buzzer_t;

/* Function prototypes */

int hdd_buzzer_init(void);
void hdd_buzzer_cleanup(void);
void hdd_buzzer_click(void);

#endif /* HDD_BUZZER_H */
