/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          General GPIO subsystem using libgpiod 2.0.
 *
 *
 *          Copyright 2026 Ignacio Castano.
 */
#include <gpiod.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <86box/86box.h>
#include <86box/plat.h>
#include <86box/timer.h>
#include <86box/device.h>
#include <86box/fdd.h>
#include <86box/scsi.h>
#include <86box/scsi_device.h>
#include <86box/cdrom.h>
#include <86box/rdisk.h>
#include <86box/mo.h>
#include <86box/scsi_tape.h>
#include <86box/hdd.h>
#include <86box/thread.h>
#include <86box/network.h>
#include <86box/machine_status.h>
#include <86box/gpio.h>

#define GPIO_MAX_PINS 8

static struct gpiod_chip *chip = NULL;

typedef struct {
    int                        pin;
    int                        current_value;
    int                        active_low;
    struct gpiod_line_request *request;
} gpio_pin_state_t;

static gpio_pin_state_t pins[GPIO_MAX_PINS];
static int              num_pins = 0;

void
gpio_init(void)
{
    if (!gpio_enabled || !gpio_device[0])
        return;

    chip = gpiod_chip_open(gpio_device);
    if (!chip)
        pclog("GPIO: failed to open %s\n", gpio_device);

    num_pins = 0;
}

void
gpio_close(void)
{
    for (int i = 0; i < num_pins; i++) {
        if (!pins[i].request)
            continue;

        /* Do not leave the LED lit behind us. */
        gpiod_line_request_set_value(pins[i].request, (unsigned int) pins[i].pin,
                                     GPIOD_LINE_VALUE_INACTIVE);
        gpiod_line_request_release(pins[i].request);
        pins[i].request = NULL;
    }

    if (chip)
        gpiod_chip_close(chip);

    chip     = NULL;
    num_pins = 0;
}

/* Requests the line as an output with the currently configured polarity.
   Returns 0 and leaves ps->request NULL if the kernel refuses the line. */
static int
gpio_request_pin(gpio_pin_state_t *ps)
{
    struct gpiod_line_settings *settings = gpiod_line_settings_new();
    gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_OUTPUT);
    gpiod_line_settings_set_active_low(settings, ps->active_low ? true : false);

    struct gpiod_line_config *line_cfg = gpiod_line_config_new();
    unsigned int              offset   = (unsigned int) ps->pin;
    gpiod_line_config_add_line_settings(line_cfg, &offset, 1, settings);

    struct gpiod_request_config *req_cfg = gpiod_request_config_new();
    gpiod_request_config_set_consumer(req_cfg, "86box");

    ps->request       = gpiod_chip_request_lines(chip, req_cfg, line_cfg);
    ps->current_value = -1;

    gpiod_request_config_free(req_cfg);
    gpiod_line_config_free(line_cfg);
    gpiod_line_settings_free(settings);

    if (!ps->request) {
        pclog("GPIO: failed to request pin %d\n", ps->pin);
        return 0;
    }

    return 1;
}

void
gpio_set_pin(int pin, int active)
{
    const int         active_low = gpio_active_low ? 1 : 0;
    gpio_pin_state_t *ps         = NULL;

    if (!chip || pin < 0)
        return;

    /* Find existing pin state. */
    for (int i = 0; i < num_pins; i++) {
        if (pins[i].pin == pin) {
            ps = &pins[i];
            break;
        }
    }

    if (!ps) {
        /* Lazy request on first use. A line we failed to request keeps its
           slot with a NULL request, so we do not retry (and log) forever. */
        if (num_pins >= GPIO_MAX_PINS)
            return;

        ps             = &pins[num_pins++];
        ps->pin        = pin;
        ps->active_low = active_low;
        ps->request    = NULL;
        gpio_request_pin(ps);
    } else if (ps->request && (ps->active_low != active_low)) {
        /* Polarity is baked into the line request, so a change to the
           setting only takes effect once the line is requested again. */
        gpiod_line_request_release(ps->request);
        ps->request    = NULL;
        ps->active_low = active_low;
        gpio_request_pin(ps);
    }

    if (!ps->request)
        return;

    /* Only write if value changed. */
    if (ps->current_value != active) {
        ps->current_value = active;
        gpiod_line_request_set_value(ps->request, (unsigned int) pin,
                                     active ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE);
    }
}

void
gpio_hdd_activity(void)
{
    if (!chip || (gpio_hdd_pin < 0))
        return;

    int any_active = 0;
    for (int i = 0; i < HDD_BUS_USB; i++) {
        if (machine_status.hdd[i].active || machine_status.hdd[i].write_active) {
            any_active = 1;
            break;
        }
    }

    gpio_set_pin(gpio_hdd_pin, any_active);
}
