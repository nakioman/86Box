/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          General GPIO subsystem header.
 *
 *
 *          Copyright 2026 Ignacio Castano.
 */
#ifndef EMU_GPIO_H
#define EMU_GPIO_H

#ifdef __cplusplus
extern "C" {
#endif

#ifdef USE_GPIO
extern void gpio_init(void);
extern void gpio_close(void);
extern void gpio_set_pin(int pin, int active);

/* Re-evaluates the hard disk activity in machine_status and drives the
   configured HDD activity pin accordingly. Frontends call this after
   updating machine_status.hdd[], so the aggregation lives in one place. */
extern void gpio_hdd_activity(void);
#else
static inline void gpio_init(void) { }
static inline void gpio_close(void) { }
static inline void gpio_set_pin(int pin, int active) { (void) pin; (void) active; }
static inline void gpio_hdd_activity(void) { }
#endif

#ifdef __cplusplus
}
#endif

#endif /* EMU_GPIO_H */
