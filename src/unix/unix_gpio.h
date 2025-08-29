/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Linux/Raspberry Pi GPIO support for hardware indication.
 *
 *
 *
 * Authors: Your Name, <your.email@example.com>
 *
 *          Copyright 2025 Your Name.
 */
#ifndef UNIX_GPIO_H
#define UNIX_GPIO_H

#ifdef __linux__

/* Initialize GPIO support */
int unix_gpio_init(void);

/* Close GPIO support */
void unix_gpio_close(void);

/* HDD activity indication */
void unix_gpio_hdd_activity(int active);

/* HDD write indication */
void unix_gpio_hdd_write(int active);

/* HDD buzzer click sound */
void unix_gpio_hdd_click(void);

/* HDD seek sound (multiple clicks) */
void unix_gpio_hdd_seek(void);

#else

/* Non-Linux stub implementations */
static inline int unix_gpio_init(void) { return 0; }
static inline void unix_gpio_close(void) { }
static inline void unix_gpio_hdd_activity(int active) { (void)active; }
static inline void unix_gpio_hdd_write(int active) { (void)active; }
static inline void unix_gpio_hdd_click(void) { }
static inline void unix_gpio_hdd_seek(void) { }

#endif

#endif /* UNIX_GPIO_H */
#ifndef UNIX_GPIO_H
#define UNIX_GPIO_H

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize GPIO support */
extern int  unix_gpio_init(void);

/* Cleanup GPIO support */
extern void unix_gpio_close(void);

/* Set HDD activity LED state */
extern void unix_gpio_hdd_activity(int active);

/* Set HDD write LED state */
extern void unix_gpio_hdd_write(int active);

#ifdef __cplusplus
}
#endif

#endif /* UNIX_GPIO_H */
