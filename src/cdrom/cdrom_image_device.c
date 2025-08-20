/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          CD-ROM device (physical drive) handling module.
 *          Linux-specific implementation for reading from /dev/cdrom and similar devices.
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
#include <limits.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/cdrom.h>
#include <linux/fs.h>

#include <86box/86box.h>
#include <86box/log.h>
#include <86box/cdrom.h>
#include <86box/cdrom_image.h>
#include <86box/cdrom_image_device.h>

/* Forward declarations */
static int check_disc_ready(int fd, void *log);
static uint64_t get_device_size(int fd, const char *device_path, void *log);

#ifdef ENABLE_DEVICE_LOG
#define device_log(tf, fmt, ...) \
    do { \
        if ((tf) && (tf)->log) { \
            log_out((tf)->log, fmt, ##__VA_ARGS__); \
        } \
    } while (0)
#else
#define device_log(tf, fmt, ...)
#endif

/* Standard CD-ROM sector size */
#define CD_SECTOR_SIZE 2048
#define CD_RAW_SECTOR_SIZE 2352

/* Device track file functions */
static int
device_read(void *priv, uint8_t *buffer, uint64_t seek, size_t count)
{
    track_file_t *tf = (track_file_t *)priv;
    device_track_file_t *dev_tf = (device_track_file_t *)tf->priv;
    
    if (!dev_tf || dev_tf->fd < 0) {
        device_log(tf->log, "Device read: invalid device track file\n");
        return 0;
    }

    device_log(tf->log, "Device read: device=%s, seek=%" PRIu64 ", count=%zu\n", 
               dev_tf->device_path, seek, count);

    /* Check if disc is ready (only for block devices) */
    struct stat st;
    if (fstat(dev_tf->fd, &st) == 0 && S_ISBLK(st.st_mode)) {
        if (!check_disc_ready(dev_tf->fd, tf->log)) {
            device_log(tf->log, "Device read: disc not ready\n");
            return 0;
        }
    }

    /* Check bounds */
    if (seek >= dev_tf->device_size) {
        device_log(tf->log, "Device read: seek beyond device size\n");
        return 0;
    }

    /* Adjust count if it would read past end */
    if (seek + count > dev_tf->device_size) {
        count = dev_tf->device_size - seek;
        device_log(tf->log, "Device read: adjusted count to %zu\n", count);
    }

    /* Seek to position */
    if (lseek64(dev_tf->fd, seek, SEEK_SET) == -1) {
        device_log(tf->log, "Device read: lseek failed: %s\n", strerror(errno));
        return 0;
    }

    /* Read data */
    ssize_t bytes_read = read(dev_tf->fd, buffer, count);
    if (bytes_read < 0) {
        device_log(tf->log, "Device read: read failed: %s\n", strerror(errno));
        return 0;
    }

    if ((size_t)bytes_read != count) {
        device_log(tf->log, "Device read: partial read: got %zd, expected %zu\n", 
                   bytes_read, count);
        /* For device reads, partial reads might be acceptable in some cases */
        /* but we'll be strict for now */
        return 0;
    }

    device_log(tf->log, "Device read: successfully read %zd bytes\n", bytes_read);
    return 1;
}

static uint64_t
device_get_length(void *priv)
{
    track_file_t *tf = (track_file_t *)priv;
    device_track_file_t *dev_tf = (device_track_file_t *)tf->priv;
    
    if (!dev_tf) {
        device_log(tf->log, "Device get_length: invalid device track file\n");
        return 0;
    }

    /* Check for disc changes before returning length */
    /* No need to check for disc changes here - monitor handles this */
    device_log(tf->log, "Device get_length: %" PRIu64 "\n", dev_tf->device_size);
    return dev_tf->device_size;
}

static void
device_close(void *priv)
{
    track_file_t *tf = (track_file_t *)priv;
    device_track_file_t *dev_tf;
    
    if (!tf) {
        return;
    }

    dev_tf = (device_track_file_t *)tf->priv;
    
    if (dev_tf) {
        if (dev_tf->fd >= 0) {
            close(dev_tf->fd);
            dev_tf->fd = -1;
        }
        if (dev_tf->device_path) {
            free(dev_tf->device_path);
            dev_tf->device_path = NULL;
        }
        free(dev_tf);
        tf->priv = NULL;
    }

    if (tf->log) {
        log_close(tf->log);
        tf->log = NULL;
    }

    memset(tf->fn, 0x00, sizeof(tf->fn));
    free(tf);
}

/* Helper function to check if a disc is present and ready */
static int
check_disc_ready(int fd, void *log)
{
    int status = ioctl(fd, CDROM_DRIVE_STATUS, CDSL_CURRENT);
    
    switch (status) {
        case CDS_DISC_OK:
            device_log(log, "Disc is present and ready\n");
            return 1;
            
        case CDS_NO_DISC:
            device_log(log, "No disc in drive\n");
            return 0;
            
        case CDS_TRAY_OPEN:
            device_log(log, "Drive tray is open\n");
            return 0;
            
        case CDS_DRIVE_NOT_READY:
            device_log(log, "Drive is not ready\n");
            return 0;
            
        case CDS_NO_INFO:
            device_log(log, "Cannot determine drive status\n");
            /* Assume ready and let read operations fail if needed */
            return 1;
            
        default:
            device_log(log, "Unknown drive status: %d\n", status);
            return 1;  /* Assume ready */
    }
}

/* Helper function to detect if a device is a CD-ROM */
static int
is_cdrom_device(int fd, void *log)
{
    int drive_type;
    
    /* Try to get the drive type using CDROM_GET_CAPABILITY */
    if (ioctl(fd, CDROM_GET_CAPABILITY, 0) >= 0) {
        device_log(log, "Device supports CD-ROM ioctls\n");
        return 1;
    }
    
    /* Try CDROM_DRIVE_STATUS */
    drive_type = ioctl(fd, CDROM_DRIVE_STATUS, CDSL_CURRENT);
    if (drive_type >= 0) {
        device_log(log, "CD-ROM drive status: %d\n", drive_type);
        return 1;
    }
    
    device_log(log, "Device does not appear to be a CD-ROM drive\n");
    return 0;
}

/* Helper function to get the size of the device/disc */
static uint64_t
get_device_size(int fd, const char *device_path, void *log)
{
    struct stat st;
    uint64_t size = 0;
    
    if (fstat(fd, &st) == 0) {
        if (S_ISBLK(st.st_mode)) {
            /* Block device - try to get actual size */
            unsigned long long blk_size;
            
            if (ioctl(fd, BLKGETSIZE64, &blk_size) == 0) {
                size = blk_size;
                device_log(log, "Block device size via BLKGETSIZE64: %" PRIu64 " bytes\n", size);
            } else {
                /* Fallback: try BLKGETSIZE (returns sectors) */
                unsigned long sectors;
                if (ioctl(fd, BLKGETSIZE, &sectors) == 0) {
                    size = sectors * 512;  /* Usually 512-byte sectors */
                    device_log(log, "Block device size via BLKGETSIZE: %lu sectors = %" PRIu64 " bytes\n", 
                               sectors, size);
                }
            }
            
            /* For CD-ROM devices, try to get the disc size */
            if (size == 0) {
                struct cdrom_tochdr toc_header;
                if (ioctl(fd, CDROMREADTOCHDR, &toc_header) == 0) {
                    /* Get the last track to estimate size */
                    struct cdrom_tocentry toc_entry;
                    toc_entry.cdte_track = toc_header.cdth_trk1;  /* Last track */
                    toc_entry.cdte_format = CDROM_LBA;
                    
                    if (ioctl(fd, CDROMREADTOCENTRY, &toc_entry) == 0) {
                        /* Approximate size based on last track position */
                        size = (uint64_t)toc_entry.cdte_addr.lba * CD_SECTOR_SIZE;
                        device_log(log, "CD-ROM disc size estimated from TOC: %" PRIu64 " bytes\n", size);
                    }
                }
            }
        } else if (S_ISREG(st.st_mode)) {
            /* Regular file */
            size = st.st_size;
            device_log(log, "Regular file size: %" PRIu64 " bytes\n", size);
        }
    }
    
    /* If all else fails, try seeking to the end */
    if (size == 0) {
        off64_t end_pos = lseek64(fd, 0, SEEK_END);
        if (end_pos > 0) {
            size = end_pos;
            device_log(log, "Device size via seek: %" PRIu64 " bytes\n", size);
        }
        lseek64(fd, 0, SEEK_SET);  /* Reset position */
    }
    
    return size;
}

track_file_t *
cdrom_device_init(const uint8_t id, const char *device_path, int *error)
{
    track_file_t *tf = NULL;
    device_track_file_t *dev_tf = NULL;
    char log_name[1024];
    struct stat st;

    *error = 1;  /* Assume error initially */

    if (!device_path || strlen(device_path) == 0) {
        return NULL;
    }

    /* Check if the path looks like a device file */
    if (strncmp(device_path, "/dev/", 5) != 0) {
        return NULL;  /* Not a device path */
    }

    /* Check if device exists */
    if (stat(device_path, &st) != 0) {
        return NULL;
    }

    tf = calloc(1, sizeof(track_file_t));
    if (!tf) {
        return NULL;
    }

    dev_tf = calloc(1, sizeof(device_track_file_t));
    if (!dev_tf) {
        free(tf);
        return NULL;
    }

    /* Set up logging */
    snprintf(log_name, sizeof(log_name), "CD-ROM %i Device", id + 1);
    tf->log = log_open(log_name);

    /* Copy device path */
    dev_tf->device_path = strdup(device_path);
    if (!dev_tf->device_path) {
        device_close(tf);
        return NULL;
    }

    /* Open device */
    dev_tf->fd = open(device_path, O_RDONLY | O_NONBLOCK);
    if (dev_tf->fd < 0) {
        device_log(tf->log, "Failed to open device %s: %s\n", device_path, strerror(errno));
        device_close(tf);
        return NULL;
    }

    /* Remove O_NONBLOCK flag for actual reading */
    int flags = fcntl(dev_tf->fd, F_GETFL);
    if (flags >= 0) {
        fcntl(dev_tf->fd, F_SETFL, flags & ~O_NONBLOCK);
    }

    /* Verify it's a CD-ROM device (for block devices) */
    if (S_ISBLK(st.st_mode) && !is_cdrom_device(dev_tf->fd, tf->log)) {
        device_log(tf->log, "Device %s does not appear to be a CD-ROM drive\n", device_path);
        device_close(tf);
        return NULL;
    }

    /* Get device size */
    dev_tf->device_size = get_device_size(dev_tf->fd, device_path, tf->log);
    if (dev_tf->device_size == 0) {
        device_log(tf->log, "Failed to determine size of device %s\n", device_path);
        device_close(tf);
        return NULL;
    }

    dev_tf->sector_size = CD_SECTOR_SIZE;
    
    /* Set up track file structure */
    tf->priv = dev_tf;
    tf->fp = NULL;  /* Not used for devices */
    strncpy(tf->fn, device_path, sizeof(tf->fn) - 1);
    tf->fn[sizeof(tf->fn) - 1] = '\0';

    /* Set function pointers */
    tf->read = device_read;
    tf->get_length = device_get_length;
    tf->close = device_close;

    device_log(tf->log, "Device CD-ROM initialized: device=%s, size=%" PRIu64 "\n", 
               device_path, dev_tf->device_size);

    *error = 0;  /* Success */
    return tf;
}

#endif /* !_WIN32 */
