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

#define ENABLE_FLOPPY_BUZZER_LOG 1
#ifdef ENABLE_FLOPPY_BUZZER_LOG
int floppy_buzzer_do_log = ENABLE_FLOPPY_BUZZER_LOG;

static void
floppy_buzzer_log(const char *fmt, ...)
{
    va_list ap;

    if (floppy_buzzer_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
#else
#    define floppy_buzzer_log(fmt, ...)
#endif

/* Default GPIO pin for buzzer (GPIO 18, physical pin 12) */
#define DEFAULT_BUZZER_PIN 18
#define DEFAULT_BUZZER_CHIP "/dev/gpiochip0"

/* Sound patterns timing (in microseconds) */
#define MOTOR_STARTUP_DURATION 2000000    /* 2 seconds */
#define MOTOR_RUNNING_PERIOD   100000     /* 100ms between pulses */
#define SEEK_STEP_DURATION     50000      /* 50ms per step */
#define READ_WRITE_PULSE       5000       /* 5ms pulse for R/W activity */

typedef struct {
    struct gpiod_chip *chip;
    struct gpiod_line_request *buzzer_request;
    int                buzzer_pin;
    char               buzzer_chip[256];   /* GPIO chip path */
    int                enabled;
    int                gpio_enabled;       /* From config: gpio_enabled */
    int                fdd_buzzer_enabled; /* From config: fdd_buzzer_enabled */
    pthread_t          thread;
    pthread_mutex_t    mutex;
    volatile int       should_stop;
    
    /* Current state */
    volatile int motor_running[4];
    volatile int seeking[4];
    volatile int activity[4];
} floppy_buzzer_t;

static floppy_buzzer_t buzzer = { 0 };

/* Load configuration settings */
static void
floppy_buzzer_load_config(void)
{
    const char *chip_path;
    
    /* Load settings from [Unix] section */
    buzzer.gpio_enabled = config_get_int("Unix", "gpio_enabled", 1);
    buzzer.fdd_buzzer_enabled = config_get_int("Unix", "fdd_buzzer_enabled", 1);
    buzzer.buzzer_pin = config_get_int("Unix", "fdd_buzzer_gpio_pin", DEFAULT_BUZZER_PIN);
    
    /* Load GPIO chip path */
    chip_path = config_get_string("Unix", "fdd_buzzer_gpio_chip", DEFAULT_BUZZER_CHIP);
    strncpy(buzzer.buzzer_chip, chip_path, sizeof(buzzer.buzzer_chip) - 1);
    buzzer.buzzer_chip[sizeof(buzzer.buzzer_chip) - 1] = '\0';
    
    floppy_buzzer_log("Config loaded: gpio_enabled=%d, fdd_buzzer_enabled=%d, gpio_pin=%d, gpio_chip=%s\n",
                      buzzer.gpio_enabled, buzzer.fdd_buzzer_enabled, buzzer.buzzer_pin, buzzer.buzzer_chip);
}

/* Save configuration settings */
static void
floppy_buzzer_save_config(void)
{
    /* Save settings to [Unix] section */
    config_set_int("Unix", "gpio_enabled", buzzer.gpio_enabled);
    config_set_int("Unix", "fdd_buzzer_enabled", buzzer.fdd_buzzer_enabled);
    config_set_int("Unix", "fdd_buzzer_gpio_pin", buzzer.buzzer_pin);
    config_set_string("Unix", "fdd_buzzer_gpio_chip", buzzer.buzzer_chip);
}

/* Set GPIO pin high */
static void
gpio_set_high(int pin)
{
    if (!buzzer.buzzer_request)
        return;

    gpiod_line_request_set_value(buzzer.buzzer_request, buzzer.buzzer_pin, GPIOD_LINE_VALUE_ACTIVE);
}

/* Set GPIO pin low */
static void
gpio_set_low(int pin)
{
    if (!buzzer.buzzer_request)
        return;

    gpiod_line_request_set_value(buzzer.buzzer_request, buzzer.buzzer_pin, GPIOD_LINE_VALUE_INACTIVE);
}

/* Generate a tone for specified duration and frequency */
static void
generate_tone(int frequency_hz, int duration_us)
{
    if (!buzzer.enabled || frequency_hz <= 0)
        return;

    int period_us = 1000000 / frequency_hz;
    int half_period_us = period_us / 2;
    int cycles = duration_us / period_us;

    for (int i = 0; i < cycles && !buzzer.should_stop; i++) {
        gpio_set_high(buzzer.buzzer_pin);
        usleep(half_period_us);
        gpio_set_low(buzzer.buzzer_pin);
        usleep(half_period_us);
    }
}

/* Generate motor startup sound */
static void
motor_startup_sound(void)
{
    floppy_buzzer_log("Playing motor startup sound\n");
    
    /* Ramp up from low to high frequency to simulate motor spin-up */
    for (int freq = 20; freq <= 200 && !buzzer.should_stop; freq += 5) {
        generate_tone(freq, 10000);
    }
    
    /* Brief high frequency burst */
    generate_tone(800, 100000);
}

/* Generate motor running sound */
static void
motor_running_sound(void)
{
    /* Low frequency hum with occasional variations */
    generate_tone(120, 80000);
    generate_tone(100, 20000);
}

/* Generate seek step sound */
static void
seek_step_sound(void)
{
    floppy_buzzer_log("Playing seek step sound\n");
    
    /* Sharp click sound */
    generate_tone(2000, 3000);
    usleep(10000);
    generate_tone(1500, 2000);
}

/* Generate read/write activity sound */
static void
activity_sound(void)
{
    /* Quick high-frequency chirp */
    generate_tone(3000, 2000);
    usleep(1000);
    generate_tone(3500, 1500);
}

/* Main buzzer thread */
static void *
buzzer_thread(void *arg)
{
    (void)arg;
    
    floppy_buzzer_log("Floppy buzzer thread started\n");
    
    while (!buzzer.should_stop) {
        pthread_mutex_lock(&buzzer.mutex);
        
        /* Check for motor activity */
        int any_motor_running = 0;
        for (int drive = 0; drive < 4; drive++) {
            if (buzzer.motor_running[drive]) {
                any_motor_running = 1;
                break;
            }
        }
        
        /* Check for seeking activity */
        int any_seeking = 0;
        for (int drive = 0; drive < 4; drive++) {
            if (buzzer.seeking[drive]) {
                any_seeking = 1;
                buzzer.seeking[drive] = 0;  /* Reset after processing */
                break;
            }
        }
        
        /* Check for read/write activity */
        int any_activity = 0;
        for (int drive = 0; drive < 4; drive++) {
            if (buzzer.activity[drive]) {
                any_activity = 1;
                buzzer.activity[drive] = 0;  /* Reset after processing */
                break;
            }
        }
        
        pthread_mutex_unlock(&buzzer.mutex);
        
        /* Generate appropriate sounds */
        if (any_seeking) {
            seek_step_sound();
        } else if (any_activity) {
            activity_sound();
        } else if (any_motor_running) {
            motor_running_sound();
        } else {
            /* No activity, just sleep */
            usleep(50000);  /* 50ms */
        }
    }
    
    floppy_buzzer_log("Floppy buzzer thread stopped\n");
    return NULL;
}

/* Initialize the GPIO floppy buzzer */
int
floppy_buzzer_init(void)
{
    struct gpiod_line_settings *line_settings;
    struct gpiod_request_config *request_config;
    struct gpiod_line_config *line_config;
    unsigned int offsets[1];
    
    /* Load configuration settings first */
    floppy_buzzer_load_config();
    
    /* Check if GPIO is enabled in config */
    if (!buzzer.gpio_enabled) {
        floppy_buzzer_log("GPIO disabled in configuration (gpio_enabled=0), buzzer disabled\n");
        return 0;
    }
    
    /* Check if floppy buzzer is enabled in config */
    if (!buzzer.fdd_buzzer_enabled) {
        floppy_buzzer_log("Floppy buzzer disabled in configuration (fdd_buzzer_enabled=0), buzzer disabled\n");
        return 0;
    }
    
    /* Validate GPIO pin range */
    if (buzzer.buzzer_pin < 0 || buzzer.buzzer_pin > 53) {
        floppy_buzzer_log("Invalid GPIO pin %d in configuration, using default pin %d\n",
                          buzzer.buzzer_pin, DEFAULT_BUZZER_PIN);
        buzzer.buzzer_pin = DEFAULT_BUZZER_PIN;
        floppy_buzzer_save_config();
    }
    
    /* Validate GPIO chip path */
    if (strlen(buzzer.buzzer_chip) == 0) {
        floppy_buzzer_log("Empty GPIO chip path in configuration, using default %s\n", DEFAULT_BUZZER_CHIP);
        strncpy(buzzer.buzzer_chip, DEFAULT_BUZZER_CHIP, sizeof(buzzer.buzzer_chip) - 1);
        buzzer.buzzer_chip[sizeof(buzzer.buzzer_chip) - 1] = '\0';
        floppy_buzzer_save_config();
    }
    
    /* Open GPIO chip */
    buzzer.chip = gpiod_chip_open(buzzer.buzzer_chip);
    if (!buzzer.chip) {
        floppy_buzzer_log("Failed to open GPIO chip %s, buzzer disabled\n", buzzer.buzzer_chip);
        return 0;
    }
    
    floppy_buzzer_log("Successfully opened GPIO chip: %s\n", buzzer.buzzer_chip);
    
    /* Set up offsets array */
    offsets[0] = buzzer.buzzer_pin;
    
    /* Create line settings */
    line_settings = gpiod_line_settings_new();
    if (!line_settings) {
        floppy_buzzer_log("Failed to create line settings, buzzer disabled\n");
        gpiod_chip_close(buzzer.chip);
        buzzer.chip = NULL;
        return 0;
    }
    
    /* Configure line as output with initial value low */
    gpiod_line_settings_set_direction(line_settings, GPIOD_LINE_DIRECTION_OUTPUT);
    gpiod_line_settings_set_output_value(line_settings, GPIOD_LINE_VALUE_INACTIVE);
    
    /* Create line configuration */
    line_config = gpiod_line_config_new();
    if (!line_config) {
        floppy_buzzer_log("Failed to create line config, buzzer disabled\n");
        gpiod_line_settings_free(line_settings);
        gpiod_chip_close(buzzer.chip);
        buzzer.chip = NULL;
        return 0;
    }
    
    /* Add line settings to config */
    if (gpiod_line_config_add_line_settings(line_config, offsets, 1, line_settings) < 0) {
        floppy_buzzer_log("Failed to add line settings, buzzer disabled\n");
        gpiod_line_config_free(line_config);
        gpiod_line_settings_free(line_settings);
        gpiod_chip_close(buzzer.chip);
        buzzer.chip = NULL;
        return 0;
    }
    
    /* Create request configuration */
    request_config = gpiod_request_config_new();
    if (!request_config) {
        floppy_buzzer_log("Failed to create request config, buzzer disabled\n");
        gpiod_line_config_free(line_config);
        gpiod_line_settings_free(line_settings);
        gpiod_chip_close(buzzer.chip);
        buzzer.chip = NULL;
        return 0;
    }
    
    gpiod_request_config_set_consumer(request_config, "86box-floppy-buzzer");
    
    /* Request the GPIO line */
    buzzer.buzzer_request = gpiod_chip_request_lines(buzzer.chip, request_config, line_config);
    
    /* Clean up configuration objects */
    gpiod_request_config_free(request_config);
    gpiod_line_config_free(line_config);
    gpiod_line_settings_free(line_settings);
    
    if (!buzzer.buzzer_request) {
        floppy_buzzer_log("Failed to request GPIO line %d, buzzer disabled\n", buzzer.buzzer_pin);
        gpiod_chip_close(buzzer.chip);
        buzzer.chip = NULL;
        return 0;
    }
    
    /* Initialize buzzer settings */
    buzzer.enabled = 1;
    buzzer.should_stop = 0;
    
    /* Initialize mutex */
    if (pthread_mutex_init(&buzzer.mutex, NULL) != 0) {
        floppy_buzzer_log("Failed to initialize mutex, buzzer disabled\n");
        gpiod_line_request_release(buzzer.buzzer_request);
        gpiod_chip_close(buzzer.chip);
        buzzer.buzzer_request = NULL;
        buzzer.chip = NULL;
        return 0;
    }
    
    /* Create buzzer thread */
    if (pthread_create(&buzzer.thread, NULL, buzzer_thread, NULL) != 0) {
        floppy_buzzer_log("Failed to create buzzer thread, buzzer disabled\n");
        pthread_mutex_destroy(&buzzer.mutex);
        gpiod_line_request_release(buzzer.buzzer_request);
        gpiod_chip_close(buzzer.chip);
        buzzer.buzzer_request = NULL;
        buzzer.chip = NULL;
        return 0;
    }
    
    floppy_buzzer_log("Floppy buzzer initialized on GPIO chip %s, pin %d\n", buzzer.buzzer_chip, buzzer.buzzer_pin);
    return 1;
}

/* Cleanup the GPIO floppy buzzer */
void
floppy_buzzer_close(void)
{
    if (!buzzer.enabled)
        return;
    
    floppy_buzzer_log("Closing floppy buzzer\n");
    
    /* Stop the thread */
    buzzer.should_stop = 1;
    if (buzzer.thread) {
        pthread_join(buzzer.thread, NULL);
    }
    
    /* Clean up GPIO */
    if (buzzer.buzzer_request) {
        gpio_set_low(buzzer.buzzer_pin);
        gpiod_line_request_release(buzzer.buzzer_request);
        buzzer.buzzer_request = NULL;
    }
    
    if (buzzer.chip) {
        gpiod_chip_close(buzzer.chip);
        buzzer.chip = NULL;
    }
    
    /* Clean up mutex */
    pthread_mutex_destroy(&buzzer.mutex);
    
    buzzer.enabled = 0;
}

/* Signal motor start */
void
floppy_buzzer_motor_on(int drive)
{
    if (!buzzer.enabled || !buzzer.fdd_buzzer_enabled || drive < 0 || drive >= 4)
        return;
    
    pthread_mutex_lock(&buzzer.mutex);
    
    if (!buzzer.motor_running[drive]) {
        floppy_buzzer_log("Motor ON for drive %d\n", drive);
        buzzer.motor_running[drive] = 1;
        
        /* Play startup sound immediately */
        motor_startup_sound();
    }
    
    pthread_mutex_unlock(&buzzer.mutex);
}

/* Signal motor stop */
void
floppy_buzzer_motor_off(int drive)
{
    if (!buzzer.enabled || !buzzer.fdd_buzzer_enabled || drive < 0 || drive >= 4)
        return;
    
    pthread_mutex_lock(&buzzer.mutex);
    
    if (buzzer.motor_running[drive]) {
        floppy_buzzer_log("Motor OFF for drive %d\n", drive);
        buzzer.motor_running[drive] = 0;
    }
    
    pthread_mutex_unlock(&buzzer.mutex);
}

/* Signal seek operation */
void
floppy_buzzer_seek(int drive, int steps)
{
    if (!buzzer.enabled || !buzzer.fdd_buzzer_enabled || drive < 0 || drive >= 4 || steps == 0)
        return;
    
    pthread_mutex_lock(&buzzer.mutex);
    
    floppy_buzzer_log("Seek operation for drive %d, steps: %d\n", drive, steps);
    buzzer.seeking[drive] = abs(steps);  /* Store number of steps */
    
    pthread_mutex_unlock(&buzzer.mutex);
}

/* Signal read/write activity */
void
floppy_buzzer_activity(int drive)
{
    if (!buzzer.enabled || !buzzer.fdd_buzzer_enabled || drive < 0 || drive >= 4)
        return;
    
    pthread_mutex_lock(&buzzer.mutex);
    buzzer.activity[drive] = 1;
    pthread_mutex_unlock(&buzzer.mutex);
}

/* Set buzzer GPIO pin (for configuration) */
void
floppy_buzzer_set_pin(int pin)
{
    if (pin >= 0 && pin <= 53) {
        buzzer.buzzer_pin = pin;
        floppy_buzzer_log("Buzzer pin set to GPIO %d\n", pin);
        floppy_buzzer_save_config();
    }
}

/* Set buzzer GPIO chip (for configuration) */
void
floppy_buzzer_set_chip(const char *chip_path)
{
    if (chip_path && strlen(chip_path) > 0 && strlen(chip_path) < sizeof(buzzer.buzzer_chip)) {
        strncpy(buzzer.buzzer_chip, chip_path, sizeof(buzzer.buzzer_chip) - 1);
        buzzer.buzzer_chip[sizeof(buzzer.buzzer_chip) - 1] = '\0';
        floppy_buzzer_log("Buzzer chip set to %s\n", chip_path);
        floppy_buzzer_save_config();
    }
}

/* Enable/disable buzzer */
void
floppy_buzzer_enable(int enable)
{
    buzzer.fdd_buzzer_enabled = enable ? 1 : 0;
    floppy_buzzer_log("Buzzer %s\n", enable ? "enabled" : "disabled");
    floppy_buzzer_save_config();
}

/* Runtime configuration functions */
int
floppy_buzzer_get_gpio_enabled(void)
{
    return buzzer.gpio_enabled;
}

void
floppy_buzzer_set_gpio_enabled(int enabled)
{
    buzzer.gpio_enabled = enabled ? 1 : 0;
    floppy_buzzer_log("GPIO %s\n", enabled ? "enabled" : "disabled");
    floppy_buzzer_save_config();
}

int
floppy_buzzer_get_fdd_buzzer_enabled(void)
{
    return buzzer.fdd_buzzer_enabled;
}

void
floppy_buzzer_set_fdd_buzzer_enabled(int enabled)
{
    buzzer.fdd_buzzer_enabled = enabled ? 1 : 0;
    floppy_buzzer_log("Floppy buzzer %s\n", enabled ? "enabled" : "disabled");
    floppy_buzzer_save_config();
}

int
floppy_buzzer_get_pin(void)
{
    return buzzer.buzzer_pin;
}

const char *
floppy_buzzer_get_chip(void)
{
    return buzzer.buzzer_chip;
}
