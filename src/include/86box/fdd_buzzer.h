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

/* Initialize the GPIO floppy buzzer system */
extern int  floppy_buzzer_init(void);

/* Cleanup the GPIO floppy buzzer system */
extern void floppy_buzzer_close(void);

/* Signal motor operations */
extern void floppy_buzzer_motor_on(int drive);
extern void floppy_buzzer_motor_off(int drive);

/* Signal seek operations */
extern void floppy_buzzer_seek(int drive, int steps);

/* Signal read/write activity */
extern void floppy_buzzer_activity(int drive);

/* Configuration functions */
extern void floppy_buzzer_set_pin(int pin);
extern void floppy_buzzer_enable(int enable);

/* Runtime configuration functions */
extern int  floppy_buzzer_get_gpio_enabled(void);
extern void floppy_buzzer_set_gpio_enabled(int enabled);
extern int  floppy_buzzer_get_fdd_buzzer_enabled(void);
extern void floppy_buzzer_set_fdd_buzzer_enabled(int enabled);
extern int  floppy_buzzer_get_pin(void);

#endif /* UNIX_FLOPPY_BUZZER_H */
