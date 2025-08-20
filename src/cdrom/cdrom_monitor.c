/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          CD-ROM device monitoring module for automatic disc change handling.
 *          Linux-specific implementation for monitoring /dev/cdrom and similar devices.
 *
 * Authors: GitHub Copilot Assistant
 *
 *          Copyright 2025 86Box contributors.
 */
#ifndef _WIN32

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/cdrom.h>
#include <limits.h>
#include <time.h>

#include <86box/86box.h>
#include <86box/log.h>
#include <86box/cdrom.h>
#include <86box/cdrom_image.h>
#include <86box/cdrom_monitor.h>

/* Device monitor state */
typedef struct cdrom_device_monitor_t {
    int fd;                    /* File descriptor for device */
    char device_path[1024];    /* Path to device */
    int last_status;           /* Last known drive status */
    int has_disc;              /* Current disc presence state */
    time_t last_check;         /* Last time we checked status */
    void *log;                 /* Logging handle */
} cdrom_device_monitor_t;

#ifdef ENABLE_MONITOR_LOG
#define monitor_log(monitor, fmt, ...) \
    do { \
        if ((monitor) && (monitor)->log) { \
            log_out((monitor)->log, fmt, ##__VA_ARGS__); \
        } \
    } while (0)
#else
#define monitor_log(monitor, fmt, ...)
#endif

/* Check device status and return current state */
static int
check_device_status(cdrom_device_monitor_t *monitor)
{
    int status;
    
    if (!monitor || monitor->fd < 0) {
        return CDS_NO_INFO;
    }
    
    status = ioctl(monitor->fd, CDROM_DRIVE_STATUS, CDSL_CURRENT);
    monitor_log(monitor, "Device status check: %d\n", status);
    
    return status;
}

/* Initialize device monitoring for a CD-ROM drive */
cdrom_device_monitor_t *
cdrom_monitor_init(uint8_t id, const char *device_path)
{
    cdrom_device_monitor_t *monitor;
    struct stat st;
    char log_name[1024];
    
    if (!device_path || strncmp(device_path, "/dev/", 5) != 0) {
        return NULL;  /* Not a device path */
    }
    
    /* Check if device exists */
    if (stat(device_path, &st) != 0) {
        return NULL;
    }
    
    monitor = calloc(1, sizeof(cdrom_device_monitor_t));
    if (!monitor) {
        return NULL;
    }
    
    /* Set up logging */
    snprintf(log_name, sizeof(log_name), "CD-ROM %i Monitor", id + 1);
    monitor->log = log_open(log_name);
    
    /* Copy device path */
    strncpy(monitor->device_path, device_path, sizeof(monitor->device_path) - 1);
    monitor->device_path[sizeof(monitor->device_path) - 1] = '\0';
    
    /* Open device for monitoring (non-blocking) */
    monitor->fd = open(device_path, O_RDONLY | O_NONBLOCK);
    if (monitor->fd < 0) {
        monitor_log(monitor, "Failed to open device %s: %s\n", device_path, strerror(errno));
        log_close(monitor->log);
        free(monitor);
        return NULL;
    }
    
    /* Initialize state */
    monitor->last_status = check_device_status(monitor);
    monitor->has_disc = (monitor->last_status == CDS_DISC_OK);
    monitor->last_check = time(NULL);
    
    monitor_log(monitor, "Device monitor initialized: device=%s, initial_status=%d, has_disc=%d\n", 
                device_path, monitor->last_status, monitor->has_disc);
    
    return monitor;
}

/* Check if disc state has changed (ejected or inserted) */
int
cdrom_monitor_check_changes(cdrom_device_monitor_t *monitor, int *disc_inserted, int *disc_ejected)
{
    int current_status;
    int current_has_disc;
    time_t now;
    
    *disc_inserted = 0;
    *disc_ejected = 0;
    
    if (!monitor) {
        return 0;
    }
    
    now = time(NULL);
    
    /* Don't check too frequently - limit to once per second */
    if (now - monitor->last_check < 1) {
        return 0;
    }
    
    monitor->last_check = now;
    
    current_status = check_device_status(monitor);
    current_has_disc = (current_status == CDS_DISC_OK);
    
    /* Check for state changes */
    if (current_has_disc != monitor->has_disc) {
        if (current_has_disc && !monitor->has_disc) {
            /* Disc inserted */
            *disc_inserted = 1;
            monitor_log(monitor, "Disc insertion detected\n");
        } else if (!current_has_disc && monitor->has_disc) {
            /* Disc ejected */
            *disc_ejected = 1;
            monitor_log(monitor, "Disc ejection detected\n");
        }
        
        monitor->has_disc = current_has_disc;
        monitor->last_status = current_status;
        return 1;  /* State changed */
    }
    
    /* Also check for media change events */
    if (monitor->has_disc) {
        int media_changed = ioctl(monitor->fd, CDROM_MEDIA_CHANGED, CDSL_CURRENT);
        if (media_changed > 0) {
            monitor_log(monitor, "Media change detected while disc present\n");
            *disc_ejected = 1;  /* Treat as eject followed by insert */
            *disc_inserted = 1;
            return 1;
        }
    }
    
    return 0;  /* No change */
}

/* Get current disc presence state */
int
cdrom_monitor_has_disc(cdrom_device_monitor_t *monitor)
{
    if (!monitor) {
        return 0;
    }
    
    return monitor->has_disc;
}

/* Close device monitor */
void
cdrom_monitor_close(cdrom_device_monitor_t *monitor)
{
    if (!monitor) {
        return;
    }
    
    if (monitor->fd >= 0) {
        close(monitor->fd);
        monitor->fd = -1;
    }
    
    if (monitor->log) {
        monitor_log(monitor, "Device monitor closed\n");
        log_close(monitor->log);
        monitor->log = NULL;
    }
    
    free(monitor);
}

#endif /* !_WIN32 */
