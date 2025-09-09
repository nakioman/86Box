/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Linux CD-ROM support via IOCTL.
 *
 * Authors: GitHub Copilot Assistant
 *
 *          Copyright 2025 86Box contributors.
 */

#define HAVE_STDARG_H
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

#ifdef ENABLE_IOCTL_LOG
#include <stdarg.h>
#endif

#include <stdio.h>
#include <stdint.h>
#include <limits.h>

#include <86box/86box.h>
#include <86box/cdrom.h>
#include <86box/log.h>
#include <86box/plat_cdrom_ioctl.h>
#include <86box/scsi_device.h>

#include <linux/cdrom.h>

typedef struct linux_ioctl_t {
    cdrom_t     *dev;
    void        *log;
    int         is_dvd;
    int         has_audio;
    int         blocks_num;      /* Number of TOC entries like Windows */
    uint8_t     cur_rti[65536];
    int         fd;
    char        path[256];
    int         first_track;
    int         last_track;
} linux_ioctl_t;

/* Forward declaration */
static uint32_t linux_ioctl_get_last_block(const void *local);

#ifdef ENABLE_IOCTL_LOG
int ioctl_do_log = ENABLE_IOCTL_LOG;

static void
linux_ioctl_log(void *priv, const char *fmt, ...)
{
    va_list ap;
    
    if (ioctl_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
#else
#define linux_ioctl_log(priv, fmt, ...)
#endif

/* Internal functions. */
static void
linux_ioctl_close_handle(const linux_ioctl_t *dev_ioctl)
{
    if (dev_ioctl->fd >= 0) {
        linux_ioctl_log(dev_ioctl->log, "Linux IOCTL: Closing device fd=%d\n", dev_ioctl->fd);
        close(dev_ioctl->fd);
    }
}

static int
linux_ioctl_open_handle(linux_ioctl_t *dev_ioctl)
{
    linux_ioctl_log(dev_ioctl->log, "Linux IOCTL: Opening device: %s\n", dev_ioctl->path);
    
    dev_ioctl->fd = open(dev_ioctl->path, O_RDONLY | O_NONBLOCK);
    
    if (dev_ioctl->fd < 0) {
        linux_ioctl_log(dev_ioctl->log, "Linux IOCTL: Failed to open %s: %s\n", 
                       dev_ioctl->path, strerror(errno));
        return 0;
    }
    
    linux_ioctl_log(dev_ioctl->log, "Linux IOCTL: Successfully opened device fd=%d\n", dev_ioctl->fd);
    return 1;
}

static int
linux_ioctl_read_toc(linux_ioctl_t *dev_ioctl, uint8_t *toc_buf, int32_t *tracks_num)
{
    struct cdrom_tochdr toc_hdr;
    struct cdrom_tocentry toc_entry;
    int i;
    uint8_t *buf_ptr = toc_buf;
    
    *tracks_num = 0;
    memset(toc_buf, 0x00, 65536);
    
    /* Get TOC header */
    if (ioctl(dev_ioctl->fd, CDROMREADTOCHDR, &toc_hdr) < 0) {
        linux_ioctl_log(dev_ioctl->log, "Linux IOCTL: Failed to read TOC header: %s\n", strerror(errno));
        return 0;
    }
    
    dev_ioctl->first_track = toc_hdr.cdth_trk0;
    dev_ioctl->last_track = toc_hdr.cdth_trk1;
    *tracks_num = toc_hdr.cdth_trk1;
    
    linux_ioctl_log(dev_ioctl->log, "Linux IOCTL: TOC: first track %d, last track %d\n", 
                   toc_hdr.cdth_trk0, toc_hdr.cdth_trk1);
    
    /* Write TOC header to buffer */
    buf_ptr[0] = 0; /* Length high byte */
    buf_ptr[1] = ((*tracks_num + 1) * 8) + 2; /* Length low byte */
    buf_ptr[2] = toc_hdr.cdth_trk0; /* First track */
    buf_ptr[3] = toc_hdr.cdth_trk1; /* Last track */
    buf_ptr += 4;
    
    /* Read each track entry */
    for (i = toc_hdr.cdth_trk0; i <= toc_hdr.cdth_trk1; i++) {
        toc_entry.cdte_track = i;
        toc_entry.cdte_format = CDROM_MSF;
        
        if (ioctl(dev_ioctl->fd, CDROMREADTOCENTRY, &toc_entry) < 0) {
            linux_ioctl_log(dev_ioctl->log, "Linux IOCTL: Failed to read TOC entry %d: %s\n", 
                           i, strerror(errno));
            continue;
        }
        
        buf_ptr[0] = 0; /* Reserved */
        buf_ptr[1] = (toc_entry.cdte_ctrl << 4) | toc_entry.cdte_adr;
        buf_ptr[2] = i; /* Track number */
        buf_ptr[3] = 0; /* Reserved */
        buf_ptr[4] = 0; /* Start address - reserved */
        buf_ptr[5] = toc_entry.cdte_addr.msf.minute;
        buf_ptr[6] = toc_entry.cdte_addr.msf.second;
        buf_ptr[7] = toc_entry.cdte_addr.msf.frame;
        buf_ptr += 8;
        
        if (!(toc_entry.cdte_ctrl & CDROM_DATA_TRACK)) {
            dev_ioctl->has_audio = 1;
        }
    }
    
    /* Read leadout track */
    toc_entry.cdte_track = CDROM_LEADOUT;
    toc_entry.cdte_format = CDROM_MSF;
    
    if (ioctl(dev_ioctl->fd, CDROMREADTOCENTRY, &toc_entry) == 0) {
        buf_ptr[0] = 0; /* Reserved */
        buf_ptr[1] = (toc_entry.cdte_ctrl << 4) | toc_entry.cdte_adr;
        buf_ptr[2] = 0xAA; /* Leadout track number */
        buf_ptr[3] = 0; /* Reserved */
        buf_ptr[4] = 0; /* Start address - reserved */
        buf_ptr[5] = toc_entry.cdte_addr.msf.minute;
        buf_ptr[6] = toc_entry.cdte_addr.msf.second;
        buf_ptr[7] = toc_entry.cdte_addr.msf.frame;
        
        /* Calculate total blocks from leadout position */
        int minute = toc_entry.cdte_addr.msf.minute;
        int second = toc_entry.cdte_addr.msf.second;
        int frame = toc_entry.cdte_addr.msf.frame;
        
        linux_ioctl_log(dev_ioctl->log, "Linux IOCTL: Leadout MSF: %02d:%02d:%02d\n", 
                       minute, second, frame);
        
        /* Try LBA format as MSF seems unreliable */
        toc_entry.cdte_format = CDROM_LBA;
        if (ioctl(dev_ioctl->fd, CDROMREADTOCENTRY, &toc_entry) == 0) {
            dev_ioctl->blocks_num = toc_entry.cdte_addr.lba;
            linux_ioctl_log(dev_ioctl->log, "Linux IOCTL: Using LBA leadout: %d blocks\n", 
                           dev_ioctl->blocks_num);
        } else {
            linux_ioctl_log(dev_ioctl->log, "Linux IOCTL: LBA leadout failed, using MSF calculation\n");
            /* Fallback to MSF calculation with bounds checking */
            if (minute <= 99 && second <= 59 && frame <= 74) {
                dev_ioctl->blocks_num = (minute * 60 * 75) + (second * 75) + frame - 150;
            } else {
                linux_ioctl_log(dev_ioctl->log, "Linux IOCTL: Invalid MSF, defaulting to safe size\n");
                dev_ioctl->blocks_num = 74 * 60 * 75; /* Default to 74 minute CD */
            }
        }
        
        /* Additional sanity check */
        if (dev_ioctl->blocks_num <= 0 || dev_ioctl->blocks_num > 90 * 60 * 75) {
            linux_ioctl_log(dev_ioctl->log, "Linux IOCTL: Unreasonable block count %d, using default\n", 
                           dev_ioctl->blocks_num);
            dev_ioctl->blocks_num = 74 * 60 * 75; /* 74 minute CD default */
        }
    } else {
        linux_ioctl_log(dev_ioctl->log, "Linux IOCTL: Failed to read leadout track\n");
        dev_ioctl->blocks_num = 74 * 60 * 75; /* Default size */
    }
    
    return 1;
}

static void
linux_ioctl_read_raw_toc(linux_ioctl_t *dev_ioctl)
{
    uint8_t toc_buf[65536];
    int32_t tracks_num = 0;
    raw_track_info_t *rti = (raw_track_info_t *) dev_ioctl->cur_rti;
    
    /* Detect DVD capability like Windows does */
    int dvd_caps = ioctl(dev_ioctl->fd, CDROM_GET_CAPABILITY, 0);
    if (dvd_caps >= 0) {
        linux_ioctl_log(dev_ioctl->log, "Linux IOCTL: Drive capabilities=0x%08X\n", dvd_caps);
        
        /* Check multiple DVD capability flags */
        dev_ioctl->is_dvd = 0;
        if (dvd_caps & CDC_DVD) {
            dev_ioctl->is_dvd = 1;
            linux_ioctl_log(dev_ioctl->log, "Linux IOCTL: Detected DVD drive via CDC_DVD (0x%08X)\n", CDC_DVD);
        }
        if (dvd_caps & CDC_DVD_R) {
            dev_ioctl->is_dvd = 1;
            linux_ioctl_log(dev_ioctl->log, "Linux IOCTL: Detected DVD drive via CDC_DVD_R (0x%08X)\n", CDC_DVD_R);
        }
        if (dvd_caps & CDC_DVD_RAM) {
            dev_ioctl->is_dvd = 1;
            linux_ioctl_log(dev_ioctl->log, "Linux IOCTL: Detected DVD drive via CDC_DVD_RAM (0x%08X)\n", CDC_DVD_RAM);
        }
        
        /* Also check for CD-R/RW capabilities which DVD drives usually have */
        if (!dev_ioctl->is_dvd && (dvd_caps & (CDC_CD_R | CDC_CD_RW))) {
            /* If it can write CDs and has other advanced features, likely a DVD drive */
            if (dvd_caps & (CDC_MRW | CDC_MRW_W | CDC_RAM)) {
                dev_ioctl->is_dvd = 1;
                linux_ioctl_log(dev_ioctl->log, "Linux IOCTL: Detected DVD drive via advanced features (CD-R/RW + MRW/RAM)\n");
            }
        }
        
        linux_ioctl_log(dev_ioctl->log, "Linux IOCTL: Final DVD detection result: is_dvd=%d\n", dev_ioctl->is_dvd);
    } else {
        dev_ioctl->is_dvd = 0;
        linux_ioctl_log(dev_ioctl->log, "Linux IOCTL: CDROM_GET_CAPABILITY failed: %s, assuming CD-ROM only\n", strerror(errno));
    }
    
    dev_ioctl->has_audio = 0;
    dev_ioctl->blocks_num = 0;  /* This will track TOC entries like Windows */
    memset(dev_ioctl->cur_rti, 0x00, 65536);
    
    /* Read normal TOC first to get track info */
    int status = linux_ioctl_read_toc(dev_ioctl, toc_buf, &tracks_num);
    
    linux_ioctl_log(dev_ioctl->log, "Linux IOCTL: Normal TOC read status=%d, tracks_num=%d\n", status, tracks_num);
    
    if (status && (tracks_num >= 1)) {
        /* Build raw TOC entries exactly like Windows implementation */
        struct cdrom_tocentry toc_entry;
        
        /* Get the last track info for control/ADR values */
        toc_entry.cdte_track = tracks_num;
        toc_entry.cdte_format = CDROM_MSF;
        
        if (ioctl(dev_ioctl->fd, CDROMREADTOCENTRY, &toc_entry) == 0) {
            linux_ioctl_log(dev_ioctl->log, "Linux IOCTL: Last track ADR=%d, Control=%d\n", 
                           toc_entry.cdte_adr, toc_entry.cdte_ctrl);
            
            /* A0 entry - first track number */
            rti[0].adr_ctl = ((toc_entry.cdte_adr & 0xf) << 4) | (toc_entry.cdte_ctrl & 0xf);
            rti[0].point = 0xa0;
            rti[0].pm = dev_ioctl->first_track;
            linux_ioctl_log(dev_ioctl->log, "Linux IOCTL: A0 entry: adr_ctl=0x%02X, pm=%d\n", 
                           rti[0].adr_ctl, rti[0].pm);
            
            /* A1 entry - last track number */  
            rti[1].adr_ctl = rti[0].adr_ctl;
            rti[1].point = 0xa1;
            rti[1].pm = dev_ioctl->last_track;
            linux_ioctl_log(dev_ioctl->log, "Linux IOCTL: A1 entry: adr_ctl=0x%02X, pm=%d\n", 
                           rti[1].adr_ctl, rti[1].pm);
            
            /* A2 entry - leadout start address */
            struct cdrom_tocentry leadout_entry;
            leadout_entry.cdte_track = CDROM_LEADOUT;
            leadout_entry.cdte_format = CDROM_MSF;
            
            if (ioctl(dev_ioctl->fd, CDROMREADTOCENTRY, &leadout_entry) == 0) {
                rti[2].adr_ctl = rti[0].adr_ctl;
                rti[2].point = 0xa2;
                rti[2].pm = leadout_entry.cdte_addr.msf.minute;
                rti[2].ps = leadout_entry.cdte_addr.msf.second;
                rti[2].pf = leadout_entry.cdte_addr.msf.frame;
                linux_ioctl_log(dev_ioctl->log, "Linux IOCTL: A2 entry: adr_ctl=0x%02X, MSF=%02d:%02d:%02d\n", 
                               rti[2].adr_ctl, rti[2].pm, rti[2].ps, rti[2].pf);
            }
            
            dev_ioctl->blocks_num = 3;  /* A0, A1, A2 entries */
            
            /* Add individual track entries - should be all tracks, not (tracks_num - 1) */
            for (int i = 0; i < tracks_num; i++) {
                raw_track_info_t *crt = &(rti[dev_ioctl->blocks_num]);
                
                toc_entry.cdte_track = i + 1;  /* Track numbers start at 1 */
                toc_entry.cdte_format = CDROM_MSF;
                
                if (ioctl(dev_ioctl->fd, CDROMREADTOCENTRY, &toc_entry) == 0) {
                    crt->adr_ctl = ((toc_entry.cdte_adr & 0xf) << 4) | (toc_entry.cdte_ctrl & 0xf);
                    crt->point = toc_entry.cdte_track;
                    crt->pm = toc_entry.cdte_addr.msf.minute;
                    crt->ps = toc_entry.cdte_addr.msf.second;
                    crt->pf = toc_entry.cdte_addr.msf.frame;
                    
                    linux_ioctl_log(dev_ioctl->log, "Linux IOCTL: Track %d: adr_ctl=0x%02X, MSF=%02d:%02d:%02d\n", 
                                   crt->point, crt->adr_ctl, crt->pm, crt->ps, crt->pf);
                    
                    dev_ioctl->blocks_num++;
                }
            }
        } else if (status > 0) {
            /* Announce that we've had a failure - match Windows logic */
            status = 0;
        }
    }
    
    /* Check for audio tracks - match Windows logic exactly */
    if (dev_ioctl->blocks_num) {
        for (int i = 0; i < dev_ioctl->blocks_num; i++) {
            const raw_track_info_t *crt = &(rti[i]);
            
            if ((crt->point >= 1) && (crt->point <= 99) && !(crt->adr_ctl & 0x04)) {
                dev_ioctl->has_audio = 1;
                break;
            }
        }
    }
    
    linux_ioctl_log(dev_ioctl->log, "Linux IOCTL: Built raw TOC with %d TOC blocks, has_audio=%d\n",
                   dev_ioctl->blocks_num, dev_ioctl->has_audio);
    
    /* Debug: Log all TOC entries like Windows does */
    for (int i = 0; i < dev_ioctl->blocks_num; i++) {
        uint8_t *t = (uint8_t *) &rti[i];
        linux_ioctl_log(dev_ioctl->log, "Block %03i: %02X %02X %02X %02X %02X %02X %02X %02X "
                       "%02X %02X %02X\n",
                       i, t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7], t[8], t[9], t[10]);
    }
}

static int
linux_ioctl_get_track(const linux_ioctl_t *dev_ioctl, const uint32_t sector)
{
    struct cdrom_tocentry toc_entry;
    int i;
    
    if (!dev_ioctl || dev_ioctl->fd < 0) {
        return 1; /* Default to track 1 */
    }
    
    /* Sanity check track range */
    if (dev_ioctl->first_track < 1 || dev_ioctl->first_track > 99 ||
        dev_ioctl->last_track < 1 || dev_ioctl->last_track > 99 ||
        dev_ioctl->first_track > dev_ioctl->last_track) {
        linux_ioctl_log(dev_ioctl->log, "Linux IOCTL: Invalid track range %d-%d\n",
                       dev_ioctl->first_track, dev_ioctl->last_track);
        return 1;
    }
    
    /* Find which track contains this sector */
    for (i = dev_ioctl->first_track; i <= dev_ioctl->last_track; i++) {
        toc_entry.cdte_track = i;
        toc_entry.cdte_format = CDROM_LBA;
        
        if (ioctl(dev_ioctl->fd, CDROMREADTOCENTRY, &toc_entry) == 0) {
            if (sector >= (uint32_t)toc_entry.cdte_addr.lba) {
                /* Check if this is the last track or if sector is before next track */
                if (i == dev_ioctl->last_track) {
                    return i;
                }
                
                struct cdrom_tocentry next_entry;
                next_entry.cdte_track = i + 1;
                next_entry.cdte_format = CDROM_LBA;
                
                if (ioctl(dev_ioctl->fd, CDROMREADTOCENTRY, &next_entry) == 0) {
                    if (sector < (uint32_t)next_entry.cdte_addr.lba) {
                        return i;
                    }
                }
            }
        }
    }
    
    return dev_ioctl->first_track;
}

static int
linux_ioctl_is_track_audio(const linux_ioctl_t *dev_ioctl, const uint32_t pos)
{
    int track = linux_ioctl_get_track(dev_ioctl, pos);
    struct cdrom_tocentry toc_entry;
    
    toc_entry.cdte_track = track;
    toc_entry.cdte_format = CDROM_LBA;
    
    if (ioctl(dev_ioctl->fd, CDROMREADTOCENTRY, &toc_entry) == 0) {
        return !(toc_entry.cdte_ctrl & CDROM_DATA_TRACK);
    }
    
    return 0;
}

/* Shared functions. */
static int
linux_ioctl_get_track_info(const void *local, const uint32_t track,
                           int end, track_info_t *ti)
{
    const linux_ioctl_t *dev_ioctl = (const linux_ioctl_t *) local;
    struct cdrom_tocentry toc_entry;
    
    if (track < (uint32_t)dev_ioctl->first_track || track > (uint32_t)dev_ioctl->last_track) {
        return 0;
    }
    
    toc_entry.cdte_track = track;
    toc_entry.cdte_format = CDROM_MSF;
    
    if (ioctl(dev_ioctl->fd, CDROMREADTOCENTRY, &toc_entry) != 0) {
        return 0;
    }
    
    ti->number = track;
    ti->attr = toc_entry.cdte_ctrl;
    ti->m = toc_entry.cdte_addr.msf.minute;
    ti->s = toc_entry.cdte_addr.msf.second;
    ti->f = toc_entry.cdte_addr.msf.frame;
    
    return 1;
}

static void
linux_ioctl_get_raw_track_info(const void *local, int *num, uint8_t *rti)
{
    const linux_ioctl_t *dev_ioctl = (const linux_ioctl_t *) local;
    
    *num = dev_ioctl->blocks_num;  /* Number of TOC entries, like Windows */
    memcpy(rti, dev_ioctl->cur_rti, dev_ioctl->blocks_num * 11);  /* 11 bytes per entry */
    
    linux_ioctl_log(dev_ioctl->log, "Linux IOCTL: Returning %d raw track info blocks\n", *num);
}

static int
linux_ioctl_is_track_pre(const void *local, const uint32_t sector)
{
    /* Linux doesn't easily provide pre-emphasis info, return 0 */
    return 0;
}

static int
linux_ioctl_read_sector(const void *local, uint8_t *buffer, const uint32_t sector)
{
    const linux_ioctl_t *dev_ioctl = (const linux_ioctl_t *) local;
    const raw_track_info_t *rti = (const raw_track_info_t *) dev_ioctl->cur_rti;
    ssize_t bytes_read = 0;
    const int sc_offs = (sector == 0xffffffff) ? 0 : 2352;
    int len = (sector == 0xffffffff) ? 16 : 2368;
    int m = 0, s = 0, f = 0;
    uint32_t lba = sector;
    int track = -1;
    int toc_index = -1;
    int ret = 0;
    
    linux_ioctl_log(dev_ioctl->log, "Linux IOCTL: read_sector() called for sector %u (DVD=%d)\n", 
                   sector, dev_ioctl->is_dvd);
    
    if (!dev_ioctl || dev_ioctl->fd < 0 || !buffer) {
        linux_ioctl_log(dev_ioctl->log, "Linux IOCTL: read_sector() invalid parameters: dev_ioctl=%p, fd=%d, buffer=%p\n", 
                       (void*)dev_ioctl, dev_ioctl ? dev_ioctl->fd : -1, (void*)buffer);
        return 0;
    }
    
    /* Handle DVD drives (like Windows implementation) */
    if (dev_ioctl->is_dvd) {
        linux_ioctl_log(dev_ioctl->log, "Linux IOCTL: Using DVD drive logic\n");
        
        ret = 0;
        
        if (lba == 0xffffffff) {
            lba = dev_ioctl->dev->seek_pos;
            track = linux_ioctl_get_track(dev_ioctl, lba);
            
            if (track != -1) {
                len = 16;  /* Subchannel only */
                ret = 1;
            }
        } else {
            len = COOKED_SECTOR_SIZE; /* 2048 bytes for DVD drives */
            track = linux_ioctl_get_track(dev_ioctl, lba);
            
            if (track != -1) {
                /* DVD drives use simple file-based reading like Windows */
                if (lseek(dev_ioctl->fd, (long)lba * COOKED_SECTOR_SIZE, SEEK_SET) != -1) {
                    bytes_read = read(dev_ioctl->fd, &buffer[16], COOKED_SECTOR_SIZE);
                    ret = (bytes_read == COOKED_SECTOR_SIZE) ? 1 : 0;
                    
                    linux_ioctl_log(dev_ioctl->log, "Linux IOCTL: DVD read sector %u: %zd bytes, ret=%d\n",
                                   sector, bytes_read, ret);
                }
            }
        }
        
        if (ret && (bytes_read >= len) && (track != -1)) {
            /* Find TOC entry for this track */
            for (int i = 0; i < dev_ioctl->blocks_num; i++) {
                if (rti[i].point == track) {
                    toc_index = i;
                    break;
                }
            }
            
            if (toc_index != -1) {
                const raw_track_info_t *ct = &rti[toc_index];
                const uint32_t start = (ct->pm * 60 * 75) + (ct->ps * 75) + ct->pf;
                
                m = s = f = 0;
                
                /* Construct sector header and sub-header for DVD */
                if (sector != 0xffffffff) {
                    /* Sync bytes */
                    buffer[0] = 0x00;
                    memset(&buffer[1], 0xff, 10);
                    buffer[11] = 0x00;
                    
                    /* Sector header */
                    FRAMES_TO_MSF(lba + 150, &m, &s, &f);
                    buffer[12] = bin2bcd(m);
                    buffer[13] = bin2bcd(s);
                    buffer[14] = bin2bcd(f);
                    
                    /* Mode 1 data */
                    buffer[15] = 0x01;
                }
                
                /* Construct Q subchannel */
                buffer[sc_offs + 0] = (ct->adr_ctl >> 4) | ((ct->adr_ctl & 0xf) << 4);
                buffer[sc_offs + 1] = bin2bcd(ct->point);
                buffer[sc_offs + 2] = 1;
                FRAMES_TO_MSF((int32_t)(lba + 150 - start), &m, &s, &f);
                buffer[sc_offs + 3] = bin2bcd(m);
                buffer[sc_offs + 4] = bin2bcd(s);
                buffer[sc_offs + 5] = bin2bcd(f);
                FRAMES_TO_MSF(lba + 150, &m, &s, &f);
                buffer[sc_offs + 7] = bin2bcd(m);
                buffer[sc_offs + 8] = bin2bcd(s);
                buffer[sc_offs + 9] = bin2bcd(f);
            }
        }
    } else {
        /* Handle CD-ROM drives (original logic with improvements) */
        linux_ioctl_log(dev_ioctl->log, "Linux IOCTL: Using CD-ROM drive logic\n");
        
        /* Special case for subchannel-only read */
        if (lba == 0xffffffff) {
            lba = dev_ioctl->dev->seek_pos;
            track = linux_ioctl_get_track(dev_ioctl, lba);
            if (track == -1) return 0;
            
            /* Find the correct TOC entry for this track number */
            for (int i = 0; i < dev_ioctl->blocks_num; i++) {
                if (rti[i].point == track) {
                    toc_index = i;
                    break;
                }
            }
            if (toc_index == -1) return 0;
            
            /* For subchannel only, we construct Q data */
            len = 16;
            ret = 1;
        } else {
            /* Get actual disc capacity from A2 entry for bounds check */
            uint32_t disc_capacity = linux_ioctl_get_last_block(local);
            
            /* Bounds check using actual disc capacity */
            if (disc_capacity > 0 && sector >= disc_capacity) {
                linux_ioctl_log(dev_ioctl->log, "Linux IOCTL: Sector %u beyond disc end (%u blocks)\n", 
                               sector, disc_capacity);
                return 0;
            }
            
            track = linux_ioctl_get_track(dev_ioctl, lba);
            if (track == -1) return 0;
            
            /* Find the correct TOC entry for this track number */
            for (int i = 0; i < dev_ioctl->blocks_num; i++) {
                if (rti[i].point == track) {
                    toc_index = i;
                    break;
                }
            }
            if (toc_index == -1) {
                linux_ioctl_log(dev_ioctl->log, "Linux IOCTL: Could not find TOC entry for track %d\n", track);
                return 0;
            }
            
            linux_ioctl_log(dev_ioctl->log, "Linux IOCTL: Track %d mapped to TOC index %d (point=0x%02X)\n", 
                           track, toc_index, rti[toc_index].point);
            
            /* Clear the buffer first, then construct the sector */
            memset(buffer, 0, 2368);
            
            /* Read cooked data (2048 bytes) into offset 16 like Windows */
            if (lseek(dev_ioctl->fd, sector * COOKED_SECTOR_SIZE, SEEK_SET) == -1) {
                linux_ioctl_log(dev_ioctl->log, "Linux IOCTL: Seek to sector %u failed: %s\n", 
                               sector, strerror(errno));
                return 0;
            }
            
            bytes_read = read(dev_ioctl->fd, &buffer[16], COOKED_SECTOR_SIZE);
            
            linux_ioctl_log(dev_ioctl->log, "Linux IOCTL: read_sector(%u) read %zd bytes from fd %d\n", 
                           sector, bytes_read, dev_ioctl->fd);
            
            if (bytes_read != COOKED_SECTOR_SIZE) {
                linux_ioctl_log(dev_ioctl->log, "Linux IOCTL: Read sector %u failed: read %zd bytes, %s\n",
                               sector, bytes_read, strerror(errno));
                return 0;
            }
            
            /* Now construct CD-ROM sector header (this won't overwrite the data) */
            /* Sync bytes - correct CD-ROM sync pattern */
            buffer[0] = 0x00;
            memset(&buffer[1], 0xff, 10);
            buffer[11] = 0x00;
            
            /* Show the ISO 9660 data for sector 16 */
            if (sector == 16) {
                linux_ioctl_log(dev_ioctl->log, "Linux IOCTL: Sector 16 sync+header: ");
                for (int i = 0; i < 16; i++) {
                    linux_ioctl_log(dev_ioctl->log, "%02X ", buffer[i]);
                }
                linux_ioctl_log(dev_ioctl->log, "\nLinux IOCTL: Sector 16 ISO data: ");
                for (int i = 16; i < 32; i++) {
                    linux_ioctl_log(dev_ioctl->log, "%02X ", buffer[i]);
                }
                linux_ioctl_log(dev_ioctl->log, " (ASCII: ");
                for (int i = 16; i < 32; i++) {
                    linux_ioctl_log(dev_ioctl->log, "%c", (buffer[i] >= 32 && buffer[i] < 127) ? buffer[i] : '.');
                }
                linux_ioctl_log(dev_ioctl->log, ")\n");
            }
            
            /* Continue with sector header construction */
            /* Sector header with MSF address */
            FRAMES_TO_MSF(lba + 150, &m, &s, &f);
            buffer[12] = bin2bcd(m);
            buffer[13] = bin2bcd(s);
            buffer[14] = bin2bcd(f);
            buffer[15] = 0x01; /* Mode 1 data */
            
            linux_ioctl_log(dev_ioctl->log, "Linux IOCTL: Sector %u header: MSF=%02d:%02d:%02d -> BCD=%02X:%02X:%02X\n",
                           sector, m, s, f, buffer[12], buffer[13], buffer[14]);
            
            /* len was already set to 2368 at the top of function for normal sectors */
            ret = 1;
        }
        
        /* Construct Q subchannel data for CD-ROM drives */
        if (ret && toc_index != -1) {
            const raw_track_info_t *ct = &rti[toc_index];
            const uint32_t start = (ct->pm * 60 * 75) + (ct->ps * 75) + ct->pf;
            
            linux_ioctl_log(dev_ioctl->log, "Linux IOCTL: Using TOC index %d: adr_ctl=0x%02X, point=%d (raw), start=%u\n",
                           toc_index, ct->adr_ctl, ct->point, start);
            
            /* Construct Q subchannel at the appropriate offset */
            buffer[sc_offs + 0] = (ct->adr_ctl >> 4) | ((ct->adr_ctl & 0xf) << 4);
            buffer[sc_offs + 1] = bin2bcd(ct->point);  /* This should be 0x01 for track 1 */
            buffer[sc_offs + 2] = 1;
            
            linux_ioctl_log(dev_ioctl->log, "Linux IOCTL: bin2bcd(%d) = 0x%02X\n", ct->point, bin2bcd(ct->point));
            FRAMES_TO_MSF((int32_t)(lba + 150 - start), &m, &s, &f);
            buffer[sc_offs + 3] = bin2bcd(m);
            buffer[sc_offs + 4] = bin2bcd(s);
            buffer[sc_offs + 5] = bin2bcd(f);
            FRAMES_TO_MSF(lba + 150, &m, &s, &f);
            buffer[sc_offs + 7] = bin2bcd(m);
            buffer[sc_offs + 8] = bin2bcd(s);
            buffer[sc_offs + 9] = bin2bcd(f);
            
            linux_ioctl_log(dev_ioctl->log, "Linux IOCTL: Q data at offset %d: %02X %02X %02X %02X %02X %02X\n",
                           sc_offs, buffer[sc_offs], buffer[sc_offs+1], buffer[sc_offs+2], 
                           buffer[sc_offs+3], buffer[sc_offs+4], buffer[sc_offs+5]);
        }
    }
    
    /* Construct raw subchannel data from Q only like Windows (for both DVD and CD-ROM) */
    if (ret && (bytes_read >= len || len == 16)) {
        for (int i = 11; i >= 0; i--) {
            for (int j = 7; j >= 0; j--) {
                buffer[2352 + (i * 8) + j] = ((buffer[sc_offs + i] >> (7 - j)) & 0x01) << 6;
            }
        }
    }
    
    linux_ioctl_log(dev_ioctl->log, "Linux IOCTL: read_sector final result: ret=%d, len=%d\n", ret, len);
    
    return ret ? ((bytes_read >= len || len == 16) ? 1 : 0) : -1;
}

static uint8_t
linux_ioctl_get_track_type(const void *local, const uint32_t sector)
{
    const linux_ioctl_t *dev_ioctl = (const linux_ioctl_t *) local;
    
    return linux_ioctl_is_track_audio(dev_ioctl, sector) ? CD_STATUS_HAS_AUDIO : CD_STATUS_DATA_ONLY;
}

static uint32_t
linux_ioctl_get_last_block(const void *local)
{
    const linux_ioctl_t *dev_ioctl = (const linux_ioctl_t *) local;
    raw_track_info_t *rti = (raw_track_info_t *) dev_ioctl->cur_rti;
    uint32_t lb = 0;
    
    /* Find the A2 entry (leadout) to get disc capacity - match Windows logic */
    for (int i = (dev_ioctl->blocks_num - 1); i >= 0; i--) {
        if (rti[i].point == 0xa2) {
            lb = MSFtoLBA(rti[i].pm, rti[i].ps, rti[i].pf) - 151;
            linux_ioctl_log(dev_ioctl->log, "Linux IOCTL: Found A2 entry at index %d, MSF=%02d:%02d:%02d, LB=%d\n",
                           i, rti[i].pm, rti[i].ps, rti[i].pf, lb);
            break;
        }
    }
    
    linux_ioctl_log(dev_ioctl->log, "Linux IOCTL: get_last_block() returning %d\n", lb);
    
    return lb;
}

static int
linux_ioctl_read_dvd_structure(const void *local, const uint8_t layer, const uint8_t format,
                               uint8_t *buffer, uint32_t *info)
{
    /* TODO: Implement DVD structure reading */
    return 0;
}

static int
linux_ioctl_is_dvd(const void *local)
{
    const linux_ioctl_t *dev_ioctl = (const linux_ioctl_t *) local;
    
    return dev_ioctl->is_dvd;
}

static int
linux_ioctl_has_audio(const void *local)
{
    const linux_ioctl_t *dev_ioctl = (const linux_ioctl_t *) local;
    
    return dev_ioctl->has_audio;
}

static int
linux_ioctl_is_empty(const void *local)
{
    const linux_ioctl_t *dev_ioctl = (const linux_ioctl_t *) local;
    
    if (dev_ioctl->fd < 0) {
        return 1;
    }
    
    if (ioctl(dev_ioctl->fd, CDROM_DRIVE_STATUS, CDSL_CURRENT) != CDS_DISC_OK) {
        linux_ioctl_log(dev_ioctl->log, "Linux IOCTL: No disc in drive\n");
        return 1;
    }
    
    return 0;
}

static void
linux_ioctl_close(void *local)
{
    linux_ioctl_t *dev_ioctl = (linux_ioctl_t *) local;
    
    if (dev_ioctl) {
        linux_ioctl_close_handle(dev_ioctl);
        free(dev_ioctl);
    }
}

static void
linux_ioctl_load(const void *local)
{
    linux_ioctl_t *dev_ioctl = (linux_ioctl_t *) local;
    
    if (!dev_ioctl || dev_ioctl->fd < 0) {
        return;
    }
    
    // Check if media has changed using Linux ioctl
    int media_changed = ioctl(dev_ioctl->fd, CDROM_MEDIA_CHANGED, CDSL_CURRENT);
    
    if (media_changed == 1) {
        linux_ioctl_log(dev_ioctl->log, "Linux IOCTL: Media change detected, reloading TOC\n");
        
        // Clear old data
        memset(dev_ioctl->cur_rti, 0, sizeof(dev_ioctl->cur_rti));
        dev_ioctl->blocks_num = 0;
        dev_ioctl->has_audio = 0;
        dev_ioctl->is_dvd = 0;
        
        // Re-read TOC to detect new media
        linux_ioctl_read_raw_toc(dev_ioctl);
        
        linux_ioctl_log(dev_ioctl->log, "Linux IOCTL: Media reload complete\n");
    } else if (media_changed == 0) {
        // No change, but still refresh if we don't have valid data
        if (dev_ioctl->blocks_num == 0) {
            linux_ioctl_read_raw_toc(dev_ioctl);
        }
    } else {
        // Error or no media - this is normal, don't log as error
        linux_ioctl_log(dev_ioctl->log, "Linux IOCTL: No media or media check failed\n");
    }
}

static const cdrom_ops_t linux_ioctl_ops = {
    linux_ioctl_get_track_info,
    linux_ioctl_get_raw_track_info,
    linux_ioctl_is_track_pre,
    linux_ioctl_read_sector,
    linux_ioctl_get_track_type,
    linux_ioctl_get_last_block,
    linux_ioctl_read_dvd_structure,
    linux_ioctl_is_dvd,
    linux_ioctl_has_audio,
    linux_ioctl_is_empty,
    linux_ioctl_close,
    linux_ioctl_load
};

/* Public functions. */
void *
ioctl_open(cdrom_t *dev, const char *drv)
{
    linux_ioctl_t *dev_ioctl;
    
    dev_ioctl = (linux_ioctl_t *) malloc(sizeof(linux_ioctl_t));
    if (!dev_ioctl) {
        return NULL;
    }
    
    memset(dev_ioctl, 0, sizeof(linux_ioctl_t));
    
    dev_ioctl->dev = dev;
    dev_ioctl->fd = -1;
    
    /* Copy device path */
    if (drv && strlen(drv) > 0) {
        strncpy(dev_ioctl->path, drv, sizeof(dev_ioctl->path) - 1);
        dev_ioctl->path[sizeof(dev_ioctl->path) - 1] = '\0';
    } else {
        /* Try common device paths if no specific path given */
        const char *common_paths[] = {
            "/dev/sr0", "/dev/sr1", "/dev/sr2", "/dev/sr3",
            "/dev/cdrom", "/dev/dvd", "/dev/cdrw",
            NULL
        };
        
        for (int i = 0; common_paths[i]; i++) {
            strncpy(dev_ioctl->path, common_paths[i], sizeof(dev_ioctl->path) - 1);
            dev_ioctl->path[sizeof(dev_ioctl->path) - 1] = '\0';
            if (linux_ioctl_open_handle(dev_ioctl)) {
                break;
            }
            linux_ioctl_close_handle(dev_ioctl);
        }
        
        if (dev_ioctl->fd < 0) {
            linux_ioctl_log(dev_ioctl->log, "Linux IOCTL: Failed to open any CD-ROM device\n");
            free(dev_ioctl);
            return NULL;
        }
    }
    
    if (dev_ioctl->fd < 0) {
        if (!linux_ioctl_open_handle(dev_ioctl)) {
            free(dev_ioctl);
            return NULL;
        }
    }
    
    /* Set up the device operations */
    dev->ops = &linux_ioctl_ops;
    dev->local = dev_ioctl;
    
    /* Read initial TOC */
    linux_ioctl_read_raw_toc(dev_ioctl);
    
    /* Validate device state before completing initialization */
    if (dev_ioctl->blocks_num <= 0) {
        linux_ioctl_log(dev_ioctl->log, "Linux IOCTL: No TOC entries, aborting\n");
        linux_ioctl_close_handle(dev_ioctl);
        free(dev_ioctl);
        return NULL;
    }
    
    if (dev_ioctl->first_track < 1 || dev_ioctl->last_track > 99 || 
        dev_ioctl->first_track > dev_ioctl->last_track) {
        linux_ioctl_log(dev_ioctl->log, "Linux IOCTL: Invalid track range %d-%d, aborting\n",
                       dev_ioctl->first_track, dev_ioctl->last_track);
        linux_ioctl_close_handle(dev_ioctl);
        free(dev_ioctl);
        return NULL;
    }
    
    linux_ioctl_log(dev_ioctl->log, "Linux IOCTL: CD-ROM opened successfully on %s\n", 
                   dev_ioctl->path);
    
    return dev_ioctl;
}
