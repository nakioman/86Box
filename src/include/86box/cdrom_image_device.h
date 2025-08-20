/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          CD-ROM device (physical drive) handling module header.
 *
 * Authors: GitHub Copilot Assistant
 *
 *          Copyright 2025 86Box contributors.
 */
#ifndef CDROM_IMAGE_DEVICE_H
#define CDROM_IMAGE_DEVICE_H

#include <86box/cdrom_image.h>

/* Device track file struct for Linux physical CD-ROM devices */
typedef struct device_track_file_t {
    char *device_path;
    int fd;  /* File descriptor for device */
    uint64_t device_size;
    int sector_size;  /* Usually 2048 for CD-ROM */
    void *log;
} device_track_file_t;

extern track_file_t *cdrom_device_init(const uint8_t id, const char *device_path, int *error);

#endif /*CDROM_IMAGE_DEVICE_H*/
