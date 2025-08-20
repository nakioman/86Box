/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          CD-ROM device monitoring module header.
 *
 * Authors: GitHub Copilot Assistant
 *
 *          Copyright 2025 86Box contributors.
 */
#ifndef CDROM_MONITOR_H
#define CDROM_MONITOR_H

#ifndef _WIN32

/* Opaque structure for device monitor */
typedef struct cdrom_device_monitor_t cdrom_device_monitor_t;

/* Initialize device monitoring for a CD-ROM drive */
extern cdrom_device_monitor_t *cdrom_monitor_init(uint8_t id, const char *device_path);

/* Check if disc state has changed (ejected or inserted) */
extern int cdrom_monitor_check_changes(cdrom_device_monitor_t *monitor, int *disc_inserted, int *disc_ejected);

/* Get current disc presence state */
extern int cdrom_monitor_has_disc(cdrom_device_monitor_t *monitor);

/* Close device monitor */
extern void cdrom_monitor_close(cdrom_device_monitor_t *monitor);

#endif /* !_WIN32 */

#endif /* CDROM_MONITOR_H */
