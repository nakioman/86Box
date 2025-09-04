/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          GPIO-based floppy disk sound emulation for Raspberry Pi.
 *          This provides authentic floppy drive sounds by controlling
 *          a physical buzzer connected to GPIO pins.
 *
 *          Configuration (86box.cfg):
 *          [Unix]
 *          gpio_enabled = 1                 ; Enable GPIO functionality (0/1)
 *          fdd_buzzer_enabled = 1           ; Enable floppy disk buzzer (0/1)
 *          fdd_buzzer_gpio_pin = 18         ; GPIO pin for buzzer (0-53)
 *
 * Author:  nacho
 *
 *          Copyright 2025 nacho.
 */

#define HAVE_STDARG_H
#define _POSIX_C_SOURCE 200809L

#include <86box/fdd_buzzer.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <stdarg.h>
#include <math.h>
#include <gpiod.h>
#include <86box/86box.h>
#include <86box/timer.h>
#include <86box/thread.h>
#include <86box/ini.h>
#include <86box/config.h>
#include <errno.h>

#define ENABLE_FDD_BUZZER_LOG 1
#ifdef ENABLE_FDD_BUZZER_LOG
int fdd_buzzer_do_log = ENABLE_FDD_BUZZER_LOG;

static void
fdd_buzzer_log(const char *fmt, ...)
{
    va_list ap;

    if (fdd_buzzer_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
#else
#    define fdd_buzzer_log(fmt, ...)
#endif

/* Global speaker instance */
static floppy_speaker_t global_speaker;

/* Timing utilities */
#define MSEC_TO_NSEC(ms) ((ms) * 1000000L)
#define USEC_TO_NSEC(us) ((us) * 1000L)

/* Precise delay function */
static void delay_ns(uint64_t ns) {
    struct timespec req = {
        .tv_sec = ns / 1000000000UL,
        .tv_nsec = ns % 1000000000UL
    };
    nanosleep(&req, NULL);
}

static void delay_us(unsigned int us) {
    delay_ns(USEC_TO_NSEC(us));
}

/* Set GPIO pin state */
static int set_speaker_pin(bool state) {
    if (!global_speaker.initialized || !global_speaker.request) {
        return -1;
    }
    
    const enum gpiod_line_value values[1] = { state ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE };
    int ret = gpiod_line_request_set_values(global_speaker.request, values);
    return (ret < 0) ? ret : 0;
}

/* Initialize speaker system */
int fdd_buzzer_init(void) {
    memset(&global_speaker, 0, sizeof(global_speaker));

    /* Set default or user configuration */
        if (config_get_int("Unix", "gpio_enabled", 0) == 0) {
            fdd_buzzer_log("GPIO disabled in configuration, speaker not initialized\n");
            return -1;
        }

        if (config_get_int("Unix", "fdd_buzzer_enabled", 0) == 0) {
            fdd_buzzer_log("Floppy buzzer disabled in configuration, speaker not initialized\n");
            return -1;
        }

        global_speaker.config.step_volume = config_get_int("Unix", "fdd_buzzer_volume", DEFAULT_STEP_VOLUME);
        global_speaker.config.speaker_pin = config_get_int("Unix", "fdd_buzzer_gpio_pin", DEFAULT_SPEAKER_PIN);


    /* Open GPIO chip */
    global_speaker.chip = gpiod_chip_open(config_get_string("Unix", "fdd_buzzer_gpio_chip", DEFAULT_GPIO_CHIP));
    if (!global_speaker.chip) {
        fdd_buzzer_log("Failed to open GPIO chip: %s\n", strerror(errno));
        return -1;
    }

    /* Create line request builder */
    struct gpiod_line_settings *settings = gpiod_line_settings_new();
    if (!settings) {
        fdd_buzzer_log("Failed to create line settings: %s\n", strerror(errno));
        gpiod_chip_close(global_speaker.chip);
        return -1;
    }

    /* Set as output */
    gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_OUTPUT);

    /* Create line configuration */
    struct gpiod_line_config *line_cfg = gpiod_line_config_new();
    if (!line_cfg) {
        fdd_buzzer_log("Failed to create line config: %s\n", strerror(errno));
        gpiod_line_settings_free(settings);
        gpiod_chip_close(global_speaker.chip);
        return -1;
    }

    /* Add line configuration */
    if (gpiod_line_config_add_line_settings(line_cfg, &global_speaker.config.speaker_pin, 1, settings) < 0) {
        fdd_buzzer_log("Failed to add line settings: %s\n", strerror(errno));
        gpiod_line_config_free(line_cfg);
        gpiod_line_settings_free(settings);
        gpiod_chip_close(global_speaker.chip);
        return -1;
    }

    /* Request lines */
    struct gpiod_request_config *req_cfg = gpiod_request_config_new();
    if (req_cfg) {
        gpiod_request_config_set_consumer(req_cfg, "86Box Floppy Buzzer");
    }

    global_speaker.request = gpiod_chip_request_lines(global_speaker.chip, req_cfg, line_cfg);

    if (req_cfg) {
        gpiod_request_config_free(req_cfg);
    }
    gpiod_line_config_free(line_cfg);
    gpiod_line_settings_free(settings);

    if (!global_speaker.request) {
        fdd_buzzer_log("Failed to request GPIO lines: %s\n", strerror(errno));
        gpiod_chip_close(global_speaker.chip);
        return -1;
    }
    if (!global_speaker.request) {
        fdd_buzzer_log("Failed to request GPIO line as output: %s\n", strerror(errno));
        gpiod_chip_close(global_speaker.chip);
        return -1;
    }

    global_speaker.state = SPEAKER_STATE_IDLE;
    global_speaker.initialized = true;
    clock_gettime(CLOCK_MONOTONIC, &global_speaker.start_time);

    fdd_buzzer_log("Floppy speaker initialized on GPIO %u\n", global_speaker.config.speaker_pin);
    return 0;
}

/* Cleanup speaker resources */
void fdd_buzzer_cleanup(void) {
    if (!global_speaker.initialized) {
        return;
    }

    fdd_buzzer_log("Cleaning up floppy speaker\n");

    /* Ensure speaker is off */
    if (global_speaker.request) {
        set_speaker_pin(false);
        gpiod_line_request_release(global_speaker.request);
    }

    if (global_speaker.chip) {
        gpiod_chip_close(global_speaker.chip);
    }

    global_speaker.initialized = false;
    global_speaker.state = SPEAKER_STATE_IDLE;
    memset(&global_speaker, 0, sizeof(global_speaker));
    fdd_buzzer_log("Floppy speaker cleaned up\n");
}

/* Generate step pulse */
void fdd_buzzer_step_pulse(void) {
    if (!global_speaker.initialized || global_speaker.config.step_volume == 0) {
        return;
    }

    fdd_buzzer_log("Floppy speaker step pulse INIT\n");

    if (global_speaker.state != SPEAKER_STATE_IDLE) {
        return; /* Already active */
    }

    unsigned int volume = global_speaker.config.step_volume;
    
    global_speaker.state = SPEAKER_STATE_ACTIVE;
    clock_gettime(CLOCK_MONOTONIC, &global_speaker.start_time);
    
    /* Initial mechanical impact */
    set_speaker_pin(true);
    delay_us(80);  /* Sharp initial pulse */
    set_speaker_pin(false);
    delay_us(40);
    
    /* Primary resonance */
    for (int i = 0; i < 3; i++) {
        set_speaker_pin(true);
        delay_us(50 - (i * 10));  /* Decreasing pulse width */
        set_speaker_pin(false);
        delay_us(50 + (i * 10));  /* Increasing gap */
    }
    
    /* Secondary resonance (damped) */
    for (int i = 0; i < 2; i++) {
        set_speaker_pin(true);
        delay_us(20);  /* Short pulses */
        set_speaker_pin(false);
        delay_us(70 + (i * 20));  /* Longer gaps */
    }
    
    /* Scale timing based on volume */
    delay_us(1000 * (11 - volume));
    
    /* Minimum step cycle time (3ms standard) */
    delay_us(2000);
    
    global_speaker.state = SPEAKER_STATE_IDLE;
    fdd_buzzer_log("Floppy speaker step pulse DONE\n");
}

/* Generate seek sound (multiple steps) */
void fdd_buzzer_seek(unsigned int steps) {
    if (!global_speaker.initialized || global_speaker.config.step_volume == 0 || steps == 0) {
        return;
    }

    fdd_buzzer_log("Floppy speaker seek %u steps\n", steps);

    global_speaker.state = SPEAKER_STATE_ACTIVE;
    unsigned int volume = global_speaker.config.step_volume;

    /* Use faster timing for multi-track seeks */
    unsigned int step_delay_us = (steps > 1) ? 2000 : 3000;  /* Faster for multi-track */

    for (unsigned int i = 0; i < steps; i++) {
        /* Initial mechanical impact */
        set_speaker_pin(true);
        delay_us(60);  /* Shorter impact for seeks */
        set_speaker_pin(false);
        delay_us(30);

        /* Quick resonance (shorter for seeks) */
        for (int j = 0; j < 2; j++) {
            set_speaker_pin(true);
            delay_us(40 - (j * 10));
            set_speaker_pin(false);
            delay_us(40 + (j * 10));
        }

        /* Quick damping pulse */
        set_speaker_pin(true);
        delay_us(20);
        set_speaker_pin(false);

        /* Scale timing based on volume and wait for next step */
        delay_us(step_delay_us * (10 - (volume - 1)) / 10);
    }

    global_speaker.state = SPEAKER_STATE_IDLE;
    fdd_buzzer_log("Floppy speaker seek DONE\n");
}