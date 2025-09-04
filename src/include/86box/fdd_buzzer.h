/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          GPIO-based floppy disk sound emulation for Raspberry Pi.
 *
 * Author:  nacho
 *
 *          Copyright 2025 nacho.
 */

#ifndef FDD_BUZZER_H
#define FDD_BUZZER_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <gpiod.h>

/* Configuration constants */
#define DEFAULT_SPEAKER_PIN 20  /* GPIO18 (Pin 12) */
#define DEFAULT_STEP_VOLUME 1   /* Volume level 0-100 */
#define DEFAULT_GPIO_CHIP "/dev/gpiochip0"

/* Notification volume flags */
#define NOTIFY_VOLUME_MASK 0x7F /* 7 bits for 0-100 volume */
#define NOTIFY_SLOTNR      0x80

/* Speaker states */
typedef enum {
    SPEAKER_STATE_IDLE = 0,
    SPEAKER_STATE_ACTIVE,
    SPEAKER_STATE_MASKED
} speaker_state_t;

/* Speaker configuration structure */
typedef struct {
    uint8_t step_volume;    /* Step sound volume (0=off, 1-10) */
    uint8_t notify_volume;  /* Insert/eject notification volume with flags */
    uint32_t speaker_pin;   /* GPIO pin number for speaker */
} floppy_speaker_config_t;

/* Speaker control structure */
typedef struct {
    struct gpiod_chip *chip;        /* GPIO chip */
    struct gpiod_line_request *request; /* GPIO line request */
    speaker_state_t state;          /* Current speaker state */
    struct timespec start_time;     /* Start time for pulse timing */
    bool initialized;               /* Initialization flag */
    floppy_speaker_config_t config; /* Configuration */
} floppy_speaker_t;

/* Function prototypes */

/**
 * Initialize the floppy speaker system
 * @param config Pointer to configuration (NULL for defaults)
 * @return 0 on success, -1 on error
 */
int fdd_buzzer_init(void);

/**
 * Cleanup and release GPIO resources
 */
void fdd_buzzer_cleanup(void);

/**
 * Generate a step sound pulse (head movement simulation)
 */
void fdd_buzzer_step_pulse(void);

/**
 * Generate motor spin-up sound
 */
void fdd_buzzer_motor_on(void);

/**
 * Generate motor spin-down sound  
 */
void fdd_buzzer_motor_off(void);

/**
 * Generate seek sound (multiple steps)
 * @param steps Number of steps to simulate
 */
void fdd_buzzer_seek(unsigned int steps);

#endif /* UNIX_FLOPPY_BUZZER_H */
