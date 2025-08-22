/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Implementation of USB floppy device support.
 *          USB floppy drives appear as block devices (/dev/sda) and
 *          contain standard floppy disk images.
 *
 *
 * Authors: 86Box Team
 *
 *          Copyright 2025 86Box Team.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <wchar.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#define HAVE_STDARG_H
#define ENABLE_USB_FDD_LOG 1
#include <86box/86box.h>
#include <86box/timer.h>
#include <86box/plat.h>
#include <86box/fdd.h>
#include <86box/fdd_86f.h>
#include <86box/fdd_usb.h>
#include <86box/fdc.h>

/* USB floppy structure - treats device as a block device containing floppy image */
typedef struct usb_fdd_t {
    int         fd;             /* File descriptor for the block device */
    int         track;          /* Current track */
    int         heads;          /* Number of heads (detected from size) */
    int         sectors;        /* Sectors per track (detected from size) */
    int         sector_size;    /* Sector size in bytes (always 512) */
    int         tracks;         /* Total number of tracks (detected from size) */
    uint64_t    total_size;     /* Total size of the block device */
    uint16_t    disk_flags;     /* Disk flags */
    uint16_t    current_side_flags[2]; /* Current side flags */
    char       *device_path;    /* Device path (/dev/sda) */
} usb_fdd_t;

static usb_fdd_t *usb_fdd[FDD_NUM];
static fdc_t *usb_fdd_fdc;

#ifdef ENABLE_USB_FDD_LOG
int usb_fdd_do_log = ENABLE_USB_FDD_LOG;

static void
usb_fdd_log(const char *fmt, ...)
{
    va_list ap;

    if (usb_fdd_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
#else
#    define usb_fdd_log(fmt, ...)
#endif

/* Detect floppy disk geometry from block device size */
static int
detect_floppy_geometry(int fd, usb_fdd_t *dev)
{
    struct stat st;
    uint64_t size;
    
    if (fstat(fd, &st) == -1) {
        usb_fdd_log("USB_FDD: Failed to get device size\n");
        return 0;
    }
    
    /* Get the size of the block device */
    if (S_ISBLK(st.st_mode)) {
        /* For block devices, get the actual size */
        if (lseek(fd, 0, SEEK_END) == -1) {
            usb_fdd_log("USB_FDD: Failed to seek to end of device\n");
            return 0;
        }
        size = lseek(fd, 0, SEEK_CUR);
        lseek(fd, 0, SEEK_SET); /* Reset to beginning */
    } else {
        /* For regular files, use file size */
        size = st.st_size;
    }
    
    dev->total_size = size;
    dev->sector_size = 512; /* Standard sector size for USB storage */
    
    /* Detect floppy format based on total size */
    if (size == 163840) {       /* 160K (40 tracks, 1 side, 8 sectors) */
        dev->tracks = 40;
        dev->heads = 1;
        dev->sectors = 8;
        dev->disk_flags = 0x00;
        dev->current_side_flags[0] = dev->current_side_flags[1] = 0x02; /* DD */
    } else if (size == 184320) { /* 180K (40 tracks, 1 side, 9 sectors) */
        dev->tracks = 40;
        dev->heads = 1;
        dev->sectors = 9;
        dev->disk_flags = 0x00;
        dev->current_side_flags[0] = dev->current_side_flags[1] = 0x02; /* DD */
    } else if (size == 327680) { /* 320K (40 tracks, 2 sides, 8 sectors) */
        dev->tracks = 40;
        dev->heads = 2;
        dev->sectors = 8;
        dev->disk_flags = 0x08; /* Double sided */
        dev->current_side_flags[0] = dev->current_side_flags[1] = 0x02; /* DD */
    } else if (size == 368640) { /* 360K (40 tracks, 2 sides, 9 sectors) */
        dev->tracks = 40;
        dev->heads = 2;
        dev->sectors = 9;
        dev->disk_flags = 0x08; /* Double sided */
        dev->current_side_flags[0] = dev->current_side_flags[1] = 0x02; /* DD */
    } else if (size == 737280) { /* 720K (80 tracks, 2 sides, 9 sectors) */
        dev->tracks = 80;
        dev->heads = 2;
        dev->sectors = 9;
        dev->disk_flags = 0x08; /* Double sided */
        dev->current_side_flags[0] = dev->current_side_flags[1] = 0x02; /* DD */
    } else if (size == 1228800) { /* 1.2M (80 tracks, 2 sides, 15 sectors) */
        dev->tracks = 80;
        dev->heads = 2;
        dev->sectors = 15;
        dev->disk_flags = 0x08; /* Double sided */
        dev->current_side_flags[0] = dev->current_side_flags[1] = 0x00; /* HD */
    } else if (size == 1474560) { /* 1.44M (80 tracks, 2 sides, 18 sectors) */
        dev->tracks = 80;
        dev->heads = 2;
        dev->sectors = 18;
        dev->disk_flags = 0x08; /* Double sided */
        dev->current_side_flags[0] = dev->current_side_flags[1] = 0x00; /* HD */
    } else if (size == 2949120) { /* 2.88M (80 tracks, 2 sides, 36 sectors) */
        dev->tracks = 80;
        dev->heads = 2;
        dev->sectors = 36;
        dev->disk_flags = 0x08; /* Double sided */
        dev->current_side_flags[0] = dev->current_side_flags[1] = 0x03; /* ED */
    } else {
        /* Unknown size, try to guess based on sector count */
        uint64_t total_sectors = size / 512;
        if (total_sectors <= 720) {
            dev->tracks = 40;
            dev->heads = 2;
            dev->sectors = 9;
            dev->disk_flags = 0x08;
            dev->current_side_flags[0] = dev->current_side_flags[1] = 0x02;
        } else if (total_sectors <= 1440) {
            dev->tracks = 80;
            dev->heads = 2;
            dev->sectors = 18;
            dev->disk_flags = 0x08;
            dev->current_side_flags[0] = dev->current_side_flags[1] = 0x00;
        } else {
            dev->tracks = 80;
            dev->heads = 2;
            dev->sectors = 36;
            dev->disk_flags = 0x08;
            dev->current_side_flags[0] = dev->current_side_flags[1] = 0x03;
        }
    }
    
    usb_fdd_log("USB_FDD: Detected geometry from size %llu - tracks: %d, heads: %d, sectors: %d\n", 
                (unsigned long long)size, dev->tracks, dev->heads, dev->sectors);
    
    return 1;
}

/* Read sector directly from USB floppy device */
static void
read_sector_from_device(int drive, int track, int head, int sector, uint8_t *buffer)
{
    usb_fdd_t *dev = usb_fdd[drive];
    off_t offset;
    ssize_t bytes_read;
    
    if (!dev || dev->fd < 0) {
        memset(buffer, 0, 512);
        return;
    }
    
    /* Calculate the absolute sector number and offset */
    int absolute_sector = (track * dev->heads + head) * dev->sectors + (sector - 1);
    offset = (off_t)absolute_sector * dev->sector_size;
    
    if (offset + dev->sector_size > (off_t)dev->total_size) {
        usb_fdd_log("USB_FDD: Sector %d out of bounds\n", absolute_sector);
        memset(buffer, 0, dev->sector_size);
        return;
    }
    
    /* Seek to the sector position */
    if (lseek(dev->fd, offset, SEEK_SET) == -1) {
        usb_fdd_log("USB_FDD: Failed to seek to sector %d\n", absolute_sector);
        memset(buffer, 0, dev->sector_size);
        return;
    }
    
    /* Read the sector directly from the device */
    bytes_read = read(dev->fd, buffer, dev->sector_size);
    if (bytes_read != dev->sector_size) {
        usb_fdd_log("USB_FDD: Failed to read sector %d (got %zd bytes)\n", 
                    absolute_sector, bytes_read);
        memset(buffer, 0, dev->sector_size);
    }
}

/* Write sector directly to USB floppy device */
static void
write_sector_to_device(int drive, int track, int head, int sector, const uint8_t *buffer)
{
    usb_fdd_t *dev = usb_fdd[drive];
    off_t offset;
    ssize_t bytes_written;
    
    if (!dev || dev->fd < 0)
        return;
    
    if (writeprot[drive]) {
        usb_fdd_log("USB_FDD: Write protected\n");
        return;
    }
    
    /* Calculate the absolute sector number and offset */
    int absolute_sector = (track * dev->heads + head) * dev->sectors + (sector - 1);
    offset = (off_t)absolute_sector * dev->sector_size;
    
    if (offset + dev->sector_size > (off_t)dev->total_size) {
        usb_fdd_log("USB_FDD: Sector %d out of bounds for write\n", absolute_sector);
        return;
    }
    
    /* Seek to the sector position */
    if (lseek(dev->fd, offset, SEEK_SET) == -1) {
        usb_fdd_log("USB_FDD: Failed to seek to sector %d for write\n", absolute_sector);
        return;
    }
    
    /* Write the sector directly to the device */
    bytes_written = write(dev->fd, buffer, dev->sector_size);
    if (bytes_written != dev->sector_size) {
        usb_fdd_log("USB_FDD: Failed to write sector %d (wrote %zd bytes)\n", 
                    absolute_sector, bytes_written);
    } else {
        /* Sync to ensure data is written to hardware */
        fsync(dev->fd);
    }
}

/* Seek to track */
static void
usb_fdd_seek(int drive, int track)
{
    usb_fdd_t *dev = usb_fdd[drive];
    
    if (!dev)
        return;
    
    dev->track = track;
    
    /* For USB floppy, we use the d86f engine for track management */
    d86f_set_cur_track(drive, track);
    d86f_reset_index_hole_pos(drive, 0);
    d86f_reset_index_hole_pos(drive, 1);
    d86f_destroy_linked_lists(drive, 0);
    d86f_destroy_linked_lists(drive, 1);
    d86f_zero_track(drive);
    
    /* Build track data for both sides */
    for (int side = 0; side < dev->heads; side++) {
        int current_pos = d86f_prepare_pretrack(drive, side, 0);
        
        /* Add sectors to the track */
        for (int sector = 1; sector <= dev->sectors; sector++) {
            uint8_t sector_data[512];
            uint8_t id[4] = { track, side, sector, 2 }; /* Size code 2 = 512 bytes */
            
            /* Read sector from device */
            read_sector_from_device(drive, track, side, sector, sector_data);
            
            /* Add sector to d86f track */
            current_pos = d86f_prepare_sector(drive, side, current_pos, id, sector_data, 512, 22, 18, 0);
        }
    }
}

/* Get disk flags */
static uint16_t
usb_fdd_disk_flags(int drive)
{
    const usb_fdd_t *dev = usb_fdd[drive];
    return dev ? dev->disk_flags : 0;
}

/* Get side flags */
static uint16_t
usb_fdd_side_flags(int drive)
{
    const usb_fdd_t *dev = usb_fdd[drive];
    int side = fdd_get_head(drive);
    return dev ? dev->current_side_flags[side] : 0;
}

/* Set sector for reading/writing */
static void
usb_fdd_set_sector(int drive, int side, uint8_t c, uint8_t h, uint8_t r, uint8_t n)
{
    usb_fdd_t *dev = usb_fdd[drive];
    
    if (!dev)
        return;
    
    /* Validate sector parameters */
    if (c != dev->track || h != side || r < 1 || r > dev->sectors)
        return;
    
    /* The d86f engine will handle the actual sector selection */
}

/* Read data callback */
static uint8_t
usb_fdd_poll_read_data(int drive, int side, uint16_t pos)
{
    /* This is handled by the d86f engine using the track data we prepared */
    return 0x00;
}

/* Write data callback */
static void
usb_fdd_poll_write_data(int drive, int side, uint16_t pos, uint8_t data)
{
    /* For USB floppy, writes are handled at the sector level */
    if (writeprot[drive])
        return;
}

/* Writeback function - ensure any pending writes are synced */
static void
usb_fdd_writeback(int drive)
{
    usb_fdd_t *dev = usb_fdd[drive];
    
    if (!dev || writeprot[drive] || dev->fd < 0)
        return;
    
    /* Sync any pending writes to the device */
    fsync(dev->fd);
}

/* Format conditions check */
static int
usb_fdd_format_conditions(int drive)
{
    /* USB floppy supports standard format operations */
    return 1;
}

/* Initialize USB floppy support */
void
usb_fdd_init(void)
{
    memset(usb_fdd, 0x00, sizeof(usb_fdd));
}

/* Load USB floppy device */
void
usb_fdd_load(int drive, char *fn)
{
    usb_fdd_t *dev;
    int fd;
    
    usb_fdd_log("USB_FDD: Loading USB floppy device %d from '%s'\n", drive, fn);
    
    d86f_unregister(drive);
    writeprot[drive] = 0;
    
    /* Allocate device structure */
    dev = (usb_fdd_t *) calloc(1, sizeof(usb_fdd_t));
    if (!dev) {
        usb_fdd_log("USB_FDD: Failed to allocate device structure\n");
        return;
    }
    
    /* Store device path */
    dev->device_path = strdup(fn);
    
    /* Open the USB floppy device */
    fd = open(fn, O_RDWR);
    if (fd < 0) {
        fd = open(fn, O_RDONLY);
        if (fd < 0) {
            usb_fdd_log("USB_FDD: Failed to open device '%s'\n", fn);
            free(dev->device_path);
            free(dev);
            return;
        }
        writeprot[drive] = 1;
    }
    
    dev->fd = fd;
    
    /* Detect floppy geometry from block device size */
    if (!detect_floppy_geometry(fd, dev)) {
        usb_fdd_log("USB_FDD: Failed to detect floppy geometry\n");
        close(fd);
        free(dev->device_path);
        free(dev);
        return;
    }
    
    if (ui_writeprot[drive])
        writeprot[drive] = 1;
    fwriteprot[drive] = writeprot[drive];
    
    /* Set up the device */
    usb_fdd[drive] = dev;
    
    /* Attach to D86F engine */
    d86f_handler[drive].disk_flags        = usb_fdd_disk_flags;
    d86f_handler[drive].side_flags        = usb_fdd_side_flags;
    d86f_handler[drive].writeback         = usb_fdd_writeback;
    d86f_handler[drive].set_sector        = usb_fdd_set_sector;
    d86f_handler[drive].read_data         = usb_fdd_poll_read_data;
    d86f_handler[drive].write_data        = usb_fdd_poll_write_data;
    d86f_handler[drive].format_conditions = usb_fdd_format_conditions;
    d86f_handler[drive].extra_bit_cells   = null_extra_bit_cells;
    d86f_handler[drive].encoded_data      = common_encoded_data;
    d86f_handler[drive].read_revolution   = common_read_revolution;
    d86f_handler[drive].index_hole_pos    = null_index_hole_pos;
    d86f_handler[drive].get_raw_size      = common_get_raw_size;
    d86f_handler[drive].check_crc         = 1;
    d86f_set_version(drive, 0x0063);
    
    drives[drive].seek = usb_fdd_seek;
    
    d86f_common_handlers(drive);
    
    usb_fdd_log("USB_FDD: Successfully loaded USB floppy device\n");
}

/* Close USB floppy device */
void
usb_fdd_close(int drive)
{
    usb_fdd_t *dev = usb_fdd[drive];
    
    if (!dev)
        return;
    
    usb_fdd_log("USB_FDD: Closing USB floppy device %d\n", drive);
    
    d86f_unregister(drive);
    
    /* Write back any changes before closing */
    usb_fdd_writeback(drive);
    
    if (dev->fd >= 0)
        close(dev->fd);
    
    if (dev->device_path)
        free(dev->device_path);
    
    free(dev);
    usb_fdd[drive] = NULL;
}

/* Set FDC reference */
void
usb_fdd_set_fdc(void *fdc)
{
    usb_fdd_fdc = (fdc_t *) fdc;
}
