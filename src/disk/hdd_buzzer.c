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
 * Author:  nacho
 *
 *          Copyright 2025 nacho.
 */

#define HAVE_STDARG_H
#define _POSIX_C_SOURCE 200809L

#include <86box/hdd_buzzer.h>
#include <86box/gpio.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <stdarg.h>
#include <math.h>
#include <86box/86box.h>
#include <86box/timer.h>
#include <86box/thread.h>
#include <86box/ini.h>
#include <86box/config.h>
#include <errno.h>

#ifdef ENABLE_HDD_BUZZER_LOG
int hdd_buzzer_do_log = ENABLE_HDD_BUZZER_LOG;

static void
hdd_buzzer_log(const char *fmt, ...)
{
    va_list ap;

    if (hdd_buzzer_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
#else
#    define hdd_buzzer_log(fmt, ...)
#endif

/* Global HDD buzzer instance */
static hdd_buzzer_t global_hdd_buzzer;

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

/* Set HDD buzzer pin state */
static int set_hdd_buzzer_pin(bool state) {
    if (!global_hdd_buzzer.initialized || global_hdd_buzzer.gpio_pin_id < 0) {
        return -1;
    }
    
    return gpio_set_pin(global_hdd_buzzer.gpio_pin_id, state);
}

/* Initialize HDD buzzer system */
int hdd_buzzer_init(void) {
    memset(&global_hdd_buzzer, 0, sizeof(global_hdd_buzzer));
    global_hdd_buzzer.gpio_pin_id = -1;

    /* Check configuration settings */
    if (config_get_int("Unix", "gpio_enabled", 0) == 0) {
        hdd_buzzer_log("GPIO disabled in configuration, HDD buzzer not initialized\n");
        return -1;
    }

    if (config_get_int("Unix", "hdd_buzzer_enabled", 0) == 0) {
        hdd_buzzer_log("HDD buzzer disabled in configuration, buzzer not initialized\n");
        return -1;
    }

    global_hdd_buzzer.config.volume = config_get_int("Unix", "hdd_buzzer_volume", DEFAULT_HDD_BUZZER_VOLUME);
    global_hdd_buzzer.config.buzzer_pin = config_get_int("Unix", "hdd_buzzer_gpio_pin", DEFAULT_HDD_BUZZER_PIN);
    global_hdd_buzzer.config.enabled = true;

    /* Initialize GPIO system if not already initialized */
    if (!gpio_is_initialized()) {
        if (gpio_init() != 0) {
            hdd_buzzer_log("Failed to initialize GPIO system\n");
            return -1;
        }
    }

    /* Configure GPIO pin for HDD buzzer */
    gpio_pin_config_t pin_config = {
        .pin_number = global_hdd_buzzer.config.buzzer_pin,
        .type = GPIO_PIN_TYPE_OUTPUT,
        .active_high = true,  /* Assuming active high for buzzer */
        .consumer_name = "86Box HDD Buzzer",
        .initialized = false
    };

    global_hdd_buzzer.gpio_pin_id = gpio_configure_pin(&pin_config);
    if (global_hdd_buzzer.gpio_pin_id < 0) {
        hdd_buzzer_log("Failed to configure GPIO pin %u for HDD buzzer\n", 
                       global_hdd_buzzer.config.buzzer_pin);
        return -1;
    }

    global_hdd_buzzer.state = HDD_BUZZER_STATE_IDLE;
    global_hdd_buzzer.initialized = true;
    clock_gettime(CLOCK_MONOTONIC, &global_hdd_buzzer.start_time);

    /* Ensure buzzer starts in off state */
    set_hdd_buzzer_pin(false);

    hdd_buzzer_log("HDD buzzer initialized on GPIO %u (pin ID %d)\n", 
                   global_hdd_buzzer.config.buzzer_pin, global_hdd_buzzer.gpio_pin_id);
    return 0;
}

/* Cleanup HDD buzzer resources */
void hdd_buzzer_cleanup(void) {
    if (!global_hdd_buzzer.initialized) {
        return;
    }

    hdd_buzzer_log("Cleaning up HDD buzzer\n");

    /* Ensure buzzer is off and release GPIO pin */
    if (global_hdd_buzzer.gpio_pin_id >= 0) {
        set_hdd_buzzer_pin(false);
        gpio_release_pin(global_hdd_buzzer.gpio_pin_id);
        global_hdd_buzzer.gpio_pin_id = -1;
    }

    global_hdd_buzzer.initialized = false;
    global_hdd_buzzer.state = HDD_BUZZER_STATE_IDLE;
    memset(&global_hdd_buzzer, 0, sizeof(global_hdd_buzzer));
    hdd_buzzer_log("HDD buzzer cleaned up\n");
}

void
hdd_buzzer_click(void)
{
    if (!global_hdd_buzzer.initialized || !global_hdd_buzzer.config.enabled || 
        global_hdd_buzzer.config.volume == 0) {
        return;
    }

    /* Volume control: 1=50us (quietest), 2=100us, 3=200us, 4=400us, 5=800us (loudest) */
    unsigned int volume_duration = 25 * (1 << global_hdd_buzzer.config.volume); /* Exponential scale */

    set_hdd_buzzer_pin(true);
    delay_us(volume_duration);
    set_hdd_buzzer_pin(false);
}
