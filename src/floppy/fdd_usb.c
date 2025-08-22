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
#include <errno.h>
#define HAVE_STDARG_H
#define ENABLE_USB_FDD_LOG 1
#include <86box/86box.h>
#include <86box/timer.h>
#include <86box/crc.h>
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
    uint16_t    track_flags;    /* Track flags for MFM encoding and data rate */
    uint8_t     gap2_size;      /* Gap2 size for d86f */
    uint8_t     gap3_size;      /* Gap3 size for d86f */
    uint8_t     data_rate;      /* Data rate index */
    /* Sector tracking for read_data callback */
    uint8_t     current_sector_track;   /* Track of currently selected sector */
    uint8_t     current_sector_head;    /* Head of currently selected sector */
    uint8_t     current_sector_r;       /* Sector number of currently selected sector */
    uint8_t     current_sector_data[512]; /* Data buffer for currently selected sector */
    int         current_sector_valid;   /* Whether current sector data is valid */
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

/* Gap size calculation tables (missing ones from fdd_img.c) */
static const uint8_t maximum_sectors[8][6] = {
    { 26, 31, 38, 53, 64, 118 }, /*   128 */
    { 15, 19, 23, 32, 38,  73 }, /*   256 */
    {  7, 10, 12, 17, 22,  41 }, /*   512 */
    {  3,  5,  6,  9, 11,  22 }, /*  1024 */
    {  2,  2,  3,  4,  5,  11 }, /*  2048 */
    {  1,  1,  1,  2,  2,   5 }, /*  4096 */
    {  0,  0,  0,  1,  1,   3 }, /*  8192 */
    {  0,  0,  0,  0,  0,   1 }  /* 16384 */
};

static const uint8_t rates[6] = { 2, 2, 1, 4, 0, 3 };
static const uint8_t holes[6] = { 0, 0, 0, 1, 1, 2 };

/* Calculate gap sizes based on floppy geometry */
static void
calculate_gap_sizes(usb_fdd_t *dev)
{
    uint8_t sector_size_code = 2;  /* 512 bytes = code 2 */
    uint8_t temp_rate = 0xFF;
    uint8_t rate_index = 0xFF;
    
    /* Find the appropriate data rate based on sectors per track */
    for (uint8_t i = 0; i < 6; i++) {
        if (dev->sectors <= maximum_sectors[sector_size_code][i]) {
            temp_rate = rates[i];
            rate_index = i;
            dev->data_rate = temp_rate;
            /* Add hole info to existing disk_flags instead of overwriting */
            dev->disk_flags |= holes[i] << 1;
            break;
        }
    }
    
    if (temp_rate == 0xFF) {
        usb_fdd_log("USB_FDD: Unknown floppy format, using default gap sizes\n");
        dev->gap2_size = 22;
        dev->gap3_size = 108;
        dev->data_rate = 0;
        return;
    }
    
    /* Calculate gap2 size */
    dev->gap2_size = (temp_rate == 3) ? 41 : 22;
    
    /* Calculate gap3 size from lookup table (declared in fdd.h) */
    if (temp_rate < 5 && sector_size_code < 8 && dev->sectors < 48) {
        dev->gap3_size = gap3_sizes[temp_rate][sector_size_code][dev->sectors];
    } else {
        usb_fdd_log("USB_FDD: Gap3 lookup out of bounds: rate=%d, size_code=%d, sectors=%d\n", 
                    temp_rate, sector_size_code, dev->sectors);
        dev->gap3_size = 108;
    }
    
    if (dev->gap3_size == 0) {
        usb_fdd_log("USB_FDD: Invalid gap3 size, using default\n");
        dev->gap3_size = 108;
    }
    
    /* Set up track flags like IMG format does */
    dev->track_flags = 0x08;           /* MFM encoding */
    dev->track_flags |= temp_rate & 3; /* Data rate (bits 0-1) */
    if (temp_rate & 4)
        dev->track_flags |= 0x20; /* RPM flag (bit 5) */
    
    usb_fdd_log("USB_FDD: Track flags setup: temp_rate=%d, MFM=0x08, data_rate=%d, RPM_flag=%s\n",
                temp_rate, temp_rate & 3, (temp_rate & 4) ? "yes(0x20)" : "no");
    usb_fdd_log("USB_FDD: Final track_flags = 0x%04X\n", dev->track_flags);
    
    /* Add the extra bit cells flag - required for proper DOS compatibility */
    dev->disk_flags |= 0x80;
    
    usb_fdd_log("USB_FDD: Calculated gap sizes - gap2: %d, gap3: %d, data_rate: %d\n", 
                dev->gap2_size, dev->gap3_size, dev->data_rate);
    if (rate_index < 6) {
        usb_fdd_log("USB_FDD: Applied hole flags: rate_index=%d, holes[%d] << 1 = 0x%02X, combined disk_flags: 0x%04X\n", 
                    rate_index, rate_index, holes[rate_index] << 1, dev->disk_flags);
    } else {
        usb_fdd_log("USB_FDD: Invalid rate_index: %d\n", rate_index);
    }
}

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
    } else if (size == 184320) { /* 180K (40 tracks, 1 side, 9 sectors) */
        dev->tracks = 40;
        dev->heads = 1;
        dev->sectors = 9;
        dev->disk_flags = 0x00;
    } else if (size == 327680) { /* 320K (40 tracks, 2 sides, 8 sectors) */
        dev->tracks = 40;
        dev->heads = 2;
        dev->sectors = 8;
        dev->disk_flags = 0x08; /* Double sided */
    } else if (size == 368640) { /* 360K (40 tracks, 2 sides, 9 sectors) */
        dev->tracks = 40;
        dev->heads = 2;
        dev->sectors = 9;
        dev->disk_flags = 0x08; /* Double sided */
    } else if (size == 737280) { /* 720K (80 tracks, 2 sides, 9 sectors) */
        dev->tracks = 80;
        dev->heads = 2;
        dev->sectors = 9;
        dev->disk_flags = 0x08; /* Double sided */
    } else if (size == 1228800) { /* 1.2M (80 tracks, 2 sides, 15 sectors) */
        dev->tracks = 80;
        dev->heads = 2;
        dev->sectors = 15;
        dev->disk_flags = 0x08; /* Double sided */
    } else if (size == 1474560) { /* 1.44M (80 tracks, 2 sides, 18 sectors) */
        dev->tracks = 80;
        dev->heads = 2;
        dev->sectors = 18;
        dev->disk_flags = 0x08; /* Double sided */
    } else if (size == 2949120) { /* 2.88M (80 tracks, 2 sides, 36 sectors) */
        dev->tracks = 80;
        dev->heads = 2;
        dev->sectors = 36;
        dev->disk_flags = 0x08; /* Double sided */
    } else {
        /* Unknown size, try to guess based on sector count */
        uint64_t total_sectors = size / 512;
        if (total_sectors <= 720) {
            dev->tracks = 40;
            dev->heads = 2;
            dev->sectors = 9;
            dev->disk_flags = 0x08;
        } else if (total_sectors <= 1440) {
            dev->tracks = 80;
            dev->heads = 2;
            dev->sectors = 18;
            dev->disk_flags = 0x08;
        } else {
            dev->tracks = 80;
            dev->heads = 2;
            dev->sectors = 36;
            dev->disk_flags = 0x08;
        }
    }
    
    usb_fdd_log("USB_FDD: Detected geometry from size %llu - tracks: %d, heads: %d, sectors: %d\n", 
                (unsigned long long)size, dev->tracks, dev->heads, dev->sectors);
    
    usb_fdd_log("USB_FDD: Initial disk_flags after geometry detection: 0x%04X\n", dev->disk_flags);
    
    /* Calculate appropriate gap sizes for this floppy format */
    calculate_gap_sizes(dev);
    
    usb_fdd_log("USB_FDD: Final disk_flags after gap calculation: 0x%04X\n", dev->disk_flags);
    
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
        usb_fdd_log("USB_FDD: Invalid device for sector read\n");
        memset(buffer, 0, 512);
        return;
    }
    
    /* Calculate the absolute sector number and offset */
    int absolute_sector = (track * dev->heads + head) * dev->sectors + (sector - 1);
    offset = (off_t)absolute_sector * dev->sector_size;
    
    usb_fdd_log("USB_FDD: Reading T:%d H:%d S:%d -> absolute sector %d, offset %ld\n", 
                track, head, sector, absolute_sector, offset);
    
    if (offset + dev->sector_size > (off_t)dev->total_size) {
        usb_fdd_log("USB_FDD: Sector %d out of bounds (offset %ld > size %llu)\n", 
                    absolute_sector, offset, (unsigned long long)dev->total_size);
        memset(buffer, 0, dev->sector_size);
        return;
    }
    
    /* Seek to the sector position */
    if (lseek(dev->fd, offset, SEEK_SET) == -1) {
        usb_fdd_log("USB_FDD: Failed to seek to sector %d (offset %ld): %s\n", 
                    absolute_sector, offset, strerror(errno));
        memset(buffer, 0, dev->sector_size);
        return;
    }
    
    /* Read the sector directly from the device */
    bytes_read = read(dev->fd, buffer, dev->sector_size);
    if (bytes_read != dev->sector_size) {
        usb_fdd_log("USB_FDD: Failed to read sector %d (got %zd bytes, expected %d): %s\n", 
                    absolute_sector, bytes_read, dev->sector_size, strerror(errno));
        memset(buffer, 0, dev->sector_size);
    } else {
        usb_fdd_log("USB_FDD: Successfully read sector %d (%zd bytes)\n", 
                    absolute_sector, bytes_read);
        
        /* Debug: Print first 16 bytes of sector for debugging */
        if (absolute_sector == 0) {  /* Only for boot sector */
            usb_fdd_log("USB_FDD: First 16 bytes of sector 0: ");
            for (int i = 0; i < 16 && i < bytes_read; i++) {
                usb_fdd_log("%02X ", buffer[i]);
            }
            usb_fdd_log("\n");
        }
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
    
    if (!dev) {
        usb_fdd_log("USB_FDD: Seek called on uninitialized drive %d\n", drive);
        return;
    }
    
    usb_fdd_log("USB_FDD: Seeking drive %d to track %d\n", drive, track);
    
    /* Check if track is within bounds */
    if (track >= dev->tracks) {
        usb_fdd_log("USB_FDD: Track %d out of bounds (max: %d)\n", track, dev->tracks - 1);
        return;
    }
    
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
        
        usb_fdd_log("USB_FDD: Building track %d side %d\n", track, side);
        
        /* Add sectors to the track */
        for (int sector = 1; sector <= dev->sectors; sector++) {
            uint8_t sector_data[512];
            uint8_t id[4] = { track, side, sector, 2 }; /* Size code 2 = 512 bytes */
            
            /* Read sector from device */
            read_sector_from_device(drive, track, side, sector, sector_data);
            
            /* Add sector to d86f track */
            /* Use calculated gap sizes based on floppy geometry */
            current_pos = d86f_prepare_sector(drive, side, current_pos, id, sector_data, 512, 
                                            dev->gap2_size, dev->gap3_size, 0);
            
            /* Initialize the last sector ID for the track */
            if (sector == dev->sectors) {
                d86f_initialize_last_sector_id(drive, id[0], id[1], id[2], id[3]);
            }
        }
    }
    
    usb_fdd_log("USB_FDD: Completed seek to track %d\n", track);
}

/* Get disk flags */
static uint16_t
usb_fdd_disk_flags(int drive)
{
    const usb_fdd_t *dev = usb_fdd[drive];
    if (!dev) {
        usb_fdd_log("USB_FDD: disk_flags requested for invalid drive %d\n", drive);
        return 0;
    }
    
    /* Reduce logging noise - only log first few requests */
    static int log_count = 0;
    if (log_count < 5) {
        usb_fdd_log("USB_FDD: disk_flags requested for drive %d: 0x%04X\n", drive, dev->disk_flags);
        log_count++;
    }
    return dev->disk_flags;
}

/* Get side flags */
static uint16_t
usb_fdd_side_flags(int drive)
{
    const usb_fdd_t *dev = usb_fdd[drive];
    uint16_t temp_side_flags = 0;
    
    if (!dev) {
        usb_fdd_log("USB_FDD: side_flags requested for invalid drive %d\n", drive);
        return 0;
    }
    
    /* Reduce logging noise - only log first few requests */
    static int side_flags_log_count = 0;
    static int last_logged_drive = -1;
    static uint16_t last_logged_flags = 0;
    
    /* Calculate side flags based on actual data rate from geometry */
    switch (dev->data_rate) {
        case 0:  /* 500 kbps (HD) */
            temp_side_flags = 0;
            break;
        case 1:  /* 300 kbps */
            temp_side_flags = 1;
            break;
        case 2:  /* 250 kbps (DD) */
            temp_side_flags = 2;
            break;
        case 3:  /* 1000 kbps (ED) */
            temp_side_flags = 3;
            break;
        case 4:  /* Special rate - treat as DD */
            temp_side_flags = 2;
            break;
        default:
            temp_side_flags = 2;  /* Default to DD */
            break;
    }
    
    /* Always set MFM encoding flag */
    temp_side_flags |= 0x08;
    
    /* Only log first few requests or when flags change */
    if (side_flags_log_count < 5 || drive != last_logged_drive || temp_side_flags != last_logged_flags) {
        usb_fdd_log("USB_FDD: side_flags for drive %d: data_rate=%d, flags=0x%04X\n", 
                   drive, dev->data_rate, temp_side_flags);
        side_flags_log_count++;
        last_logged_drive = drive;
        last_logged_flags = temp_side_flags;
    }
    
    return temp_side_flags;
}

/* Set sector for reading/writing */
static void
usb_fdd_set_sector(int drive, int side, uint8_t c, uint8_t h, uint8_t r, uint8_t n)
{
    usb_fdd_t *dev = usb_fdd[drive];
    
    if (!dev)
        return;
    
    /* Validate sector parameters */
    if (c >= dev->tracks || h >= dev->heads || r < 1 || r > dev->sectors)
        return;
    
    /* Check if we need to read a new sector */
    if (dev->current_sector_track != c || dev->current_sector_head != h || 
        dev->current_sector_r != r || !dev->current_sector_valid) {
        
        /* Read the new sector data */
        read_sector_from_device(drive, c, h, r, dev->current_sector_data);
        
        /* Update current sector tracking */
        dev->current_sector_track = c;
        dev->current_sector_head = h;
        dev->current_sector_r = r;
        dev->current_sector_valid = 1;
        
        usb_fdd_log("USB_FDD: Set sector drive=%d, C=%d H=%d R=%d N=%d\n", drive, c, h, r, n);
    }
}

/* Read data callback */
static uint8_t
usb_fdd_poll_read_data(int drive, int side, uint16_t pos)
{
    usb_fdd_t *dev = usb_fdd[drive];
    
    if (!dev || !dev->current_sector_valid || pos >= 512) {
        usb_fdd_log("USB_FDD: Invalid read_data call - drive=%d, valid=%d, pos=%d\n", 
                    drive, dev ? dev->current_sector_valid : 0, pos);
        return 0x00;
    }
    
    return dev->current_sector_data[pos];
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
    
    /* Initialize sector tracking */
    dev->current_sector_valid = 0;
    dev->current_sector_track = 0xFF; /* Invalid track to force initial read */
    dev->current_sector_head = 0xFF;
    dev->current_sector_r = 0xFF;
    
    /* Set up D86F engine for this drive - this creates the d86f structure and CRC table */
    d86f_setup(drive);
    usb_fdd_log("USB_FDD: Initialized d86f engine for drive %d\n", drive);
    
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
    d86f_handler[drive].check_crc         = 1; /* Enable CRC checking to match other formats */
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
    
    /* Clean up d86f engine and unregister handlers */
    d86f_destroy(drive);
    
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
