/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Implementation of the GreaseWeazle USB floppy controller backend.
 *          Communicates with GreaseWeazle hardware over USB-serial to
 *          read/write real physical floppy disks through the emulator.
 *
 *          Linux only for now; stubs on other platforms.
 *
 * Authors: <YOUR_NAME>
 *
 *          Copyright 2026 86Box contributors.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <wchar.h>
#define HAVE_STDARG_H
#include <86box/86box.h>
#include <86box/timer.h>
#include <86box/plat.h>
#include <86box/fdd.h>
#include <86box/fdd_86f.h>
#include <86box/fdd_gw.h>
#include <86box/fdc.h>

static fdc_t *gw_fdc;

static void
gw_log(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    pclog_ex(fmt, ap);
    va_end(ap);
}

#ifdef __linux__

#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>
#include <errno.h>

/* GreaseWeazle protocol commands (from Cmd class in usb.py) */
#define GW_CMD_GET_INFO        0
#define GW_CMD_SEEK            2
#define GW_CMD_HEAD            3
#define GW_CMD_SET_PARAMS      4
#define GW_CMD_GET_PARAMS      5
#define GW_CMD_MOTOR           6
#define GW_CMD_READ_FLUX       7
#define GW_CMD_WRITE_FLUX      8
#define GW_CMD_GET_FLUX_STATUS 9
#define GW_CMD_SELECT         12
#define GW_CMD_DESELECT       13
#define GW_CMD_SET_BUS_TYPE   14
#define GW_CMD_RESET          16

/* GW ack status codes */
#define GW_ACK_OK       0

/* GW bus types */
#define GW_BUS_IBMPC    1

/* GW GetInfo sub-commands */
#define GW_INFO_FIRMWARE 0

/* Flux stream encoding */
#define GW_FLUX_ESCAPE   0xFF  /* Next byte is an opcode */
#define GW_FLUX_OP_INDEX 1     /* Index pulse (followed by 28-bit value) */
#define GW_FLUX_OP_SPACE 2     /* Space/gap (followed by 28-bit value) */
#define GW_FLUX_END      0     /* End of stream */

/* Maximum flux buffer size */
#define GW_MAX_FLUX_TICKS  500000

/* Track cache */
#define GW_MAX_SECTORS     26
#define GW_MAX_SECTOR_SIZE 8192

typedef struct gw_t {
    int      fd;
    char     dev_path[256];
    uint8_t  fw_major, fw_minor;
    uint32_t sample_freq;
    uint8_t  hw_model;

    int      tracks, sides, sectors, sector_size;
    int      data_rate;
    int      is_hd;
    uint16_t disk_flags, track_flags;
    int      gap2_size, gap3_size;

    int      current_cylinder;
    int      motor_on;

    struct {
        int     valid, cylinder;
        int     sector_count[2];
        uint8_t sector_id[2][GW_MAX_SECTORS][4];
        uint8_t sector_data[2][GW_MAX_SECTORS][GW_MAX_SECTOR_SIZE];
        int     sector_error[2][GW_MAX_SECTORS];
    } cache;

    int sel_side;
    int sel_idx;

    uint8_t write_data[2][GW_MAX_SECTORS][GW_MAX_SECTOR_SIZE];
    int     write_pending;
} gw_t;

static gw_t *gw[FDD_NUM];

/* ========================================================================
 * CRC-16/CCITT
 * ======================================================================== */
static uint16_t
gw_crc16(const uint8_t *data, int len)
{
    uint16_t crc = 0xFFFF;

    for (int i = 0; i < len; i++) {
        crc ^= (uint16_t) data[i] << 8;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x8000)
                crc = (crc << 1) ^ 0x1021;
            else
                crc <<= 1;
        }
    }

    return crc;
}

/* ========================================================================
 * Serial I/O
 * ======================================================================== */
static int
gw_serial_open(const char *path)
{
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        gw_log("GW: failed to open %s: %s\n", path, strerror(errno));
        return -1;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

    struct termios tio;
    memset(&tio, 0, sizeof(tio));
    tcgetattr(fd, &tio);
    cfmakeraw(&tio);
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cc[VMIN]  = 0;
    tio.c_cc[VTIME] = 20;
    cfsetispeed(&tio, B9600);
    cfsetospeed(&tio, B9600);
    tcsetattr(fd, TCSANOW, &tio);
    tcflush(fd, TCIOFLUSH);

    return fd;
}

static void
gw_serial_close(int fd)
{
    if (fd >= 0)
        close(fd);
}

static int
gw_serial_write(int fd, const uint8_t *buf, int len)
{
    int total = 0;
    while (total < len) {
        int n = write(fd, buf + total, len - total);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            gw_log("GW: serial write error: %s\n", strerror(errno));
            return -1;
        }
        total += n;
    }
    return total;
}

static int
gw_serial_read(int fd, uint8_t *buf, int len)
{
    int            total = 0;
    fd_set         rfds;
    struct timeval tv;

    while (total < len) {
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        tv.tv_sec  = 2;
        tv.tv_usec = 0;

        int ret = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (ret == 0)
            return total;

        int n = read(fd, buf + total, len - total);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return total;
        total += n;
    }
    return total;
}

static int
gw_serial_read_flux(int fd, uint8_t *buf, int maxlen)
{
    int            total = 0;
    fd_set         rfds;
    struct timeval tv;

    while (total < maxlen) {
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        tv.tv_sec  = 5;
        tv.tv_usec = 0;

        int ret = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (ret <= 0)
            break;

        int n = read(fd, buf + total, maxlen - total);
        if (n <= 0)
            break;

        total += n;

        /* Stream ends with a 0x00 byte */
        if (buf[total - 1] == GW_FLUX_END)
            break;
    }
    return total;
}

/* ========================================================================
 * GW Protocol Layer
 *
 * Wire format (all firmware versions):
 *   Send:  [cmd_byte, total_len_byte, params...]
 *   Response: 2 bytes [cmd_echo, ack_status]
 *   Additional response data (e.g. GetInfo) is read separately after ack.
 * ======================================================================== */

/* Send a command and read the 2-byte ack. Returns 0 on success, -1 on error. */
static int
gw_send_cmd(int fd, const uint8_t *cmd, int cmd_len)
{
    if (gw_serial_write(fd, cmd, cmd_len) < 0)
        return -1;

    uint8_t ack[2];
    int n = gw_serial_read(fd, ack, 2);
    if (n < 2) {
        gw_log("GW: short ack (%d bytes)\n", n);
        return -1;
    }

    if (ack[0] != cmd[0]) {
        gw_log("GW: ack cmd mismatch (got 0x%02X, expected 0x%02X)\n", ack[0], cmd[0]);
        return -1;
    }

    if (ack[1] != GW_ACK_OK) {
        gw_log("GW: command 0x%02X failed with status %d\n", cmd[0], ack[1]);
        return -1;
    }

    return 0;
}

static int
gw_cmd_get_info(gw_t *dev)
{
    /* GetInfo: [cmd=0, len=3, sub_cmd=0(Firmware)] */
    uint8_t cmd[] = { GW_CMD_GET_INFO, 3, GW_INFO_FIRMWARE };

    if (gw_send_cmd(dev->fd, cmd, sizeof(cmd)) < 0)
        return -1;

    /* Response: 32 bytes of firmware info
     * Format (little-endian): 4B + I + 4B + 3H + 14x
     *   [0] fw_major, [1] fw_minor, [2] is_main, [3] max_cmd,
     *   [4..7] sample_freq (uint32 LE),
     *   [8] hw_model, [9] hw_submodel, [10] usb_speed,
     *   [11..12] mcu_id, [13..14] mcu_mhz, [15..16] mcu_sram_kb,
     *   [17..18] usb_buf_kb, [19..31] reserved */
    uint8_t info[32];
    int n = gw_serial_read(dev->fd, info, 32);
    if (n < 8) {
        gw_log("GW: GetInfo response too short (%d bytes)\n", n);
        return -1;
    }

    dev->fw_major    = info[0];
    dev->fw_minor    = info[1];
    dev->sample_freq = info[4] | (info[5] << 8) | (info[6] << 16) | (info[7] << 24);
    if (n >= 9)
        dev->hw_model = info[8];

    gw_log("GW: firmware v%d.%d, sample_freq=%u Hz, hw_model=%d\n",
           dev->fw_major, dev->fw_minor, dev->sample_freq, dev->hw_model);
    return 0;
}

static int
gw_cmd_set_bus_type(gw_t *dev, uint8_t bus_type)
{
    uint8_t cmd[] = { GW_CMD_SET_BUS_TYPE, 3, bus_type };
    return gw_send_cmd(dev->fd, cmd, sizeof(cmd));
}

static int
gw_cmd_select(gw_t *dev)
{
    uint8_t cmd[] = { GW_CMD_SELECT, 3, 0 /* unit 0 */ };
    return gw_send_cmd(dev->fd, cmd, sizeof(cmd));
}

static int
gw_cmd_deselect(gw_t *dev)
{
    uint8_t cmd[] = { GW_CMD_DESELECT, 2 };
    return gw_send_cmd(dev->fd, cmd, sizeof(cmd));
}

static int
gw_cmd_motor(gw_t *dev, int on)
{
    uint8_t cmd[] = { GW_CMD_MOTOR, 4, 0 /* unit */, on ? 1 : 0 };
    return gw_send_cmd(dev->fd, cmd, sizeof(cmd));
}

static int
gw_cmd_seek(gw_t *dev, int cylinder)
{
    /* Seek: [cmd=2, len=3, cyl (signed byte)] */
    uint8_t cmd[] = { GW_CMD_SEEK, 3, (uint8_t) (int8_t) cylinder };
    return gw_send_cmd(dev->fd, cmd, sizeof(cmd));
}

static int
gw_cmd_head(gw_t *dev, int head)
{
    uint8_t cmd[] = { GW_CMD_HEAD, 3, (uint8_t) head };
    return gw_send_cmd(dev->fd, cmd, sizeof(cmd));
}

static int
gw_cmd_read_flux(gw_t *dev, uint8_t *flux_buf, int max_flux_len)
{
    /* ReadFlux: [cmd=7, len=8, ticks(4 LE), revolutions(2 LE)]
     * ticks=0 means no limit, revolutions = revs+1 (so 3 = read 2 revolutions) */
    uint8_t cmd[8];
    cmd[0] = GW_CMD_READ_FLUX;
    cmd[1] = 8;
    /* ticks = 0 (no tick limit) */
    cmd[2] = 0; cmd[3] = 0; cmd[4] = 0; cmd[5] = 0;
    /* revolutions = 3 (read 2 full revolutions) */
    cmd[6] = 3; cmd[7] = 0;

    if (gw_send_cmd(dev->fd, cmd, sizeof(cmd)) < 0)
        return -1;

    /* Read flux stream data until end marker (0x00 byte) */
    int n = gw_serial_read_flux(dev->fd, flux_buf, max_flux_len);

    /* GetFluxStatus to check for errors */
    uint8_t status_cmd[] = { GW_CMD_GET_FLUX_STATUS, 2 };
    gw_send_cmd(dev->fd, status_cmd, sizeof(status_cmd));

    return n;
}

/* ========================================================================
 * Flux-to-MFM PLL Decoder
 * ======================================================================== */
/* Read a 28-bit value from 4 bytes of flux stream (GW bit-packed encoding) */
static uint32_t
gw_read_28bit(const uint8_t *p)
{
    uint32_t val;
    val  = (p[0] & 0xFE) >> 1;
    val += (uint32_t)(p[1] & 0xFE) << 6;
    val += (uint32_t)(p[2] & 0xFE) << 13;
    val += (uint32_t)(p[3] & 0xFE) << 20;
    return val;
}

/* Decode GW flux byte stream into flux tick values.
 *
 * Encoding:
 *   0        = end of stream
 *   1-249    = direct flux tick value
 *   250-254  = multi-byte: val = 250 + (byte-250)*255 + next_byte - 1
 *   255      = escape; next byte is opcode:
 *              1 = Index (+ 28-bit value) -- ignored for sector parsing
 *              2 = Space (+ 28-bit value) -- added to accumulator
 */
static int
gw_decode_flux_stream(const uint8_t *raw, int raw_len, uint32_t *ticks, int max_ticks)
{
    int      tick_count = 0;
    uint32_t accum      = 0;
    int      i          = 0;

    while (i < raw_len && tick_count < max_ticks) {
        uint8_t b = raw[i++];

        if (b == GW_FLUX_END)
            break;

        if (b == GW_FLUX_ESCAPE) {
            /* Escape: next byte is opcode */
            if (i >= raw_len)
                break;
            uint8_t opcode = raw[i++];
            if (opcode == GW_FLUX_OP_INDEX) {
                /* Index pulse marker -- skip the 28-bit value */
                if (i + 4 <= raw_len)
                    i += 4;
            } else if (opcode == GW_FLUX_OP_SPACE) {
                /* Space: add 28-bit value to accumulator */
                if (i + 4 <= raw_len) {
                    accum += gw_read_28bit(raw + i);
                    i += 4;
                }
            }
            /* Other opcodes: skip */
            continue;
        }

        uint32_t val;
        if (b < 250) {
            /* Direct flux value */
            val = b;
        } else {
            /* Multi-byte: val = 250 + (b - 250) * 255 + next_byte - 1 */
            if (i >= raw_len)
                break;
            val = 250 + (b - 250) * 255 + raw[i++] - 1;
        }

        accum += val;
        ticks[tick_count++] = accum;
        accum = 0;
    }

    return tick_count;
}

static int
gw_flux_to_mfm(uint32_t sample_freq, int data_rate, const uint32_t *ticks, int tick_count,
                uint8_t *mfm_bits, int max_bits)
{
    if (tick_count == 0 || data_rate == 0)
        return 0;

    double cell_width = (double) sample_freq / ((double) data_rate * 2.0);
    double pll_phase  = 0.0;
    double window     = cell_width * 0.25;
    double adapt_rate = 0.05;
    int    bit_count  = 0;

    for (int i = 0; i < tick_count && bit_count < max_bits - 8; i++) {
        double flux = (double) ticks[i];

        int cells = (int) ((flux + pll_phase) / cell_width + 0.5);
        if (cells < 1)
            cells = 1;
        if (cells > 16)
            cells = 16;

        for (int c = 0; c < cells - 1 && bit_count < max_bits; c++)
            mfm_bits[bit_count++] = 0;
        if (bit_count < max_bits)
            mfm_bits[bit_count++] = 1;

        double expected = cells * cell_width;
        double error    = flux - expected;
        pll_phase += error * adapt_rate;
        if (pll_phase > window)
            pll_phase = window;
        else if (pll_phase < -window)
            pll_phase = -window;

        cell_width += error * adapt_rate * 0.1;
    }

    return bit_count;
}

/* ========================================================================
 * MFM Sector Parser
 * ======================================================================== */
static void
gw_read_mfm_bytes(const uint8_t *mfm_bits, int total_bits, int pos, uint8_t *out, int nbytes)
{
    for (int b = 0; b < nbytes; b++) {
        uint8_t byte = 0;
        for (int bit = 0; bit < 8; bit++) {
            int idx = pos + b * 16 + bit * 2 + 1;
            if (idx < total_bits)
                byte = (byte << 1) | mfm_bits[idx];
            else
                byte <<= 1;
        }
        out[b] = byte;
    }
}

static int
gw_find_sync(const uint8_t *mfm_bits, int total_bits, int start)
{
    /* MFM sync A1 with missing clock = 0x4489:
     * Normal A1 (0x44A9): 01 00 01 00 10 10 10 01
     * Sync   A1 (0x4489): 01 00 01 00 10 00 10 01
     *                                     ^^ missing clock at bit 2
     * Three consecutive sync A1 bytes (48 bits total): */
    static const uint8_t sync_pattern[] = {
        0, 1, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1,
        0, 1, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1,
        0, 1, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1
    };
    int sync_len = 48;

    for (int i = start; i <= total_bits - sync_len; i++) {
        int match = 1;
        for (int j = 0; j < sync_len; j++) {
            if (mfm_bits[i + j] != sync_pattern[j]) {
                match = 0;
                break;
            }
        }
        if (match)
            return i + sync_len;
    }

    return -1;
}

static void
gw_parse_mfm_track(gw_t *dev, const uint8_t *mfm_bits, int total_bits, int side)
{
    int search_pos   = 0;
    int sector_count = 0;

    dev->cache.sector_count[side] = 0;

    while (sector_count < GW_MAX_SECTORS) {
        int sync_pos = gw_find_sync(mfm_bits, total_bits, search_pos);
        if (sync_pos < 0)
            break;

        uint8_t mark;
        gw_read_mfm_bytes(mfm_bits, total_bits, sync_pos, &mark, 1);

        if (mark == 0xFE) {
            uint8_t idam[6];
            gw_read_mfm_bytes(mfm_bits, total_bits, sync_pos + 16, idam, 6);

            uint8_t  c = idam[0], h = idam[1], r = idam[2], n = idam[3];
            uint16_t idam_crc = (idam[4] << 8) | idam[5];

            uint8_t  crc_buf[8] = { 0xA1, 0xA1, 0xA1, 0xFE, c, h, r, n };
            uint16_t calc_crc   = gw_crc16(crc_buf, 8);

            if (calc_crc != idam_crc) {
                gw_log("GW: IDAM CRC error at bit %d (C=%d H=%d R=%d N=%d)\n",
                       sync_pos, c, h, r, n);
                search_pos = sync_pos + 16;
                continue;
            }

            int dam_search = sync_pos + 16 + 6 * 16;
            int dam_pos    = gw_find_sync(mfm_bits, total_bits, dam_search);
            if (dam_pos < 0 || (dam_pos - dam_search) > 80 * 16) {
                search_pos = sync_pos + 16;
                continue;
            }

            uint8_t dam_mark;
            gw_read_mfm_bytes(mfm_bits, total_bits, dam_pos, &dam_mark, 1);

            if (dam_mark != 0xFB && dam_mark != 0xF8) {
                search_pos = dam_pos;
                continue;
            }

            int sector_bytes = 128 << n;
            if (sector_bytes > GW_MAX_SECTOR_SIZE)
                sector_bytes = GW_MAX_SECTOR_SIZE;

            int data_pos = dam_pos + 16;
            gw_read_mfm_bytes(mfm_bits, total_bits, data_pos,
                              dev->cache.sector_data[side][sector_count], sector_bytes);

            uint8_t data_crc_bytes[2];
            gw_read_mfm_bytes(mfm_bits, total_bits, data_pos + sector_bytes * 16,
                              data_crc_bytes, 2);
            uint16_t data_crc = (data_crc_bytes[0] << 8) | data_crc_bytes[1];

            uint8_t *crc_data = (uint8_t *) malloc(4 + sector_bytes);
            crc_data[0] = 0xA1;
            crc_data[1] = 0xA1;
            crc_data[2] = 0xA1;
            crc_data[3] = dam_mark;
            memcpy(crc_data + 4, dev->cache.sector_data[side][sector_count], sector_bytes);
            uint16_t calc_data_crc = gw_crc16(crc_data, 4 + sector_bytes);
            free(crc_data);

            dev->cache.sector_id[side][sector_count][0] = c;
            dev->cache.sector_id[side][sector_count][1] = h;
            dev->cache.sector_id[side][sector_count][2] = r;
            dev->cache.sector_id[side][sector_count][3] = n;
            dev->cache.sector_error[side][sector_count] = (calc_data_crc != data_crc) ? 1 : 0;

            if (dev->cache.sector_error[side][sector_count])
                gw_log("GW: Data CRC error: C=%d H=%d R=%d N=%d\n", c, h, r, n);

            sector_count++;
            search_pos = data_pos + (sector_bytes + 2) * 16;
        } else {
            search_pos = sync_pos + 16;
        }
    }

    /* Deduplicate sectors from multi-revolution reads.
     * Keep only the first CRC-good occurrence of each R value.
     * If no good copy exists, keep the first occurrence (even if CRC-bad). */
    {
        int dedup_count = 0;
        uint8_t  dedup_id[GW_MAX_SECTORS][4];
        uint8_t  dedup_data[GW_MAX_SECTORS][GW_MAX_SECTOR_SIZE];
        int      dedup_error[GW_MAX_SECTORS];

        for (int i = 0; i < sector_count; i++) {
            uint8_t r = dev->cache.sector_id[side][i][2];

            /* Check if we already have this R value */
            int existing = -1;
            for (int j = 0; j < dedup_count; j++) {
                if (dedup_id[j][2] == r) {
                    existing = j;
                    break;
                }
            }

            if (existing >= 0) {
                /* Replace if existing has CRC error and this one doesn't */
                if (dedup_error[existing] && !dev->cache.sector_error[side][i]) {
                    memcpy(dedup_id[existing], dev->cache.sector_id[side][i], 4);
                    int ssize = 128 << dev->cache.sector_id[side][i][3];
                    memcpy(dedup_data[existing], dev->cache.sector_data[side][i], ssize);
                    dedup_error[existing] = 0;
                }
            } else if (dedup_count < GW_MAX_SECTORS) {
                memcpy(dedup_id[dedup_count], dev->cache.sector_id[side][i], 4);
                int ssize = 128 << dev->cache.sector_id[side][i][3];
                memcpy(dedup_data[dedup_count], dev->cache.sector_data[side][i], ssize);
                dedup_error[dedup_count] = dev->cache.sector_error[side][i];
                dedup_count++;
            }
        }

        /* Copy deduplicated sectors back */
        for (int i = 0; i < dedup_count; i++) {
            memcpy(dev->cache.sector_id[side][i], dedup_id[i], 4);
            int ssize = 128 << dedup_id[i][3];
            memcpy(dev->cache.sector_data[side][i], dedup_data[i], ssize);
            dev->cache.sector_error[side][i] = dedup_error[i];
        }
        sector_count = dedup_count;
    }

    dev->cache.sector_count[side] = sector_count;

    /* Log sector summary with CRC status */
    int crc_errors = 0;
    for (int i = 0; i < sector_count; i++) {
        if (dev->cache.sector_error[side][i])
            crc_errors++;
    }
    gw_log("GW: side %d: %d unique sectors (%d CRC errors). IDs:", side, sector_count, crc_errors);
    for (int i = 0; i < sector_count; i++) {
        gw_log(" %d/%d/%d/%d%s",
               dev->cache.sector_id[side][i][0],
               dev->cache.sector_id[side][i][1],
               dev->cache.sector_id[side][i][2],
               dev->cache.sector_id[side][i][3],
               dev->cache.sector_error[side][i] ? "!" : "");
    }
    gw_log("\n");
}

/* ========================================================================
 * Physical Track Read
 * ======================================================================== */
static int
gw_read_physical_track(gw_t *dev, int cylinder, int side)
{
    static uint8_t  flux_raw[GW_MAX_FLUX_TICKS * 2];
    static uint32_t flux_ticks[GW_MAX_FLUX_TICKS];
    static uint8_t  mfm_bits[GW_MAX_FLUX_TICKS * 4];

    gw_log("GW: read_physical_track cyl=%d side=%d\n", cylinder, side);

    if (dev->current_cylinder != cylinder) {
        if (gw_cmd_seek(dev, cylinder) < 0) {
            /* Seek failed -- try recovery: re-select, re-motor, retry */
            gw_log("GW: seek to cyl %d failed, attempting recovery\n", cylinder);
            gw_cmd_select(dev);
            gw_cmd_motor(dev, 1);
            if (gw_cmd_seek(dev, cylinder) < 0) {
                gw_log("GW: seek to cyl %d FAILED after recovery\n", cylinder);
                return -1;
            }
        }
        dev->current_cylinder = cylinder;
    }

    if (gw_cmd_head(dev, side) < 0) {
        gw_log("GW: head select %d FAILED\n", side);
        return -1;
    }

    int raw_len = gw_cmd_read_flux(dev, flux_raw, sizeof(flux_raw));
    gw_log("GW: read_flux returned %d raw bytes\n", raw_len);
    if (raw_len <= 0)
        return -1;

    /* Log first 32 raw bytes for debugging */
    gw_log("GW: raw flux[0..31]:");
    for (int i = 0; i < 32 && i < raw_len; i++)
        gw_log(" %02X", flux_raw[i]);
    gw_log("\n");

    int tick_count = gw_decode_flux_stream(flux_raw, raw_len, flux_ticks, GW_MAX_FLUX_TICKS);
    gw_log("GW: decoded %d flux ticks from %d raw bytes\n", tick_count, raw_len);
    if (tick_count == 0)
        return -1;

    /* Log first few tick values */
    gw_log("GW: ticks[0..9]:");
    for (int i = 0; i < 10 && i < tick_count; i++)
        gw_log(" %u", flux_ticks[i]);
    gw_log("\n");

    gw_log("GW: PLL: sample_freq=%u data_rate=%d cell_width=%.1f\n",
           dev->sample_freq, dev->data_rate,
           (double) dev->sample_freq / ((double) dev->data_rate * 2.0));

    int bit_count = gw_flux_to_mfm(dev->sample_freq, dev->data_rate, flux_ticks, tick_count,
                                    mfm_bits, sizeof(mfm_bits));
    gw_log("GW: PLL decoded %d MFM bits\n", bit_count);
    if (bit_count == 0)
        return -1;

    gw_parse_mfm_track(dev, mfm_bits, bit_count, side);
    gw_log("GW: cyl=%d side=%d: found %d sectors\n",
           cylinder, side, dev->cache.sector_count[side]);
    return 0;
}

/* ========================================================================
 * MFM Encoder + Write Flux
 * ======================================================================== */
static int
gw_encode_mfm_byte(uint8_t byte, int prev_bit, uint8_t *mfm_out)
{
    for (int i = 7; i >= 0; i--) {
        int data_bit  = (byte >> i) & 1;
        int clock_bit = (!data_bit && !prev_bit) ? 1 : 0;
        *mfm_out++ = clock_bit;
        *mfm_out++ = data_bit;
        prev_bit   = data_bit;
    }
    return prev_bit;
}

static int
gw_encode_mfm_sync(int prev_bit, uint8_t *mfm_out)
{
    /* Sync A1 = 0x4489: 01 00 01 00 10 00 10 01 */
    static const uint8_t sync_a1[] = { 0, 1, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1 };
    memcpy(mfm_out, sync_a1, 16);
    (void) prev_bit;
    return 1;
}

/* Encode flux ticks to GW wire format for WriteFlux.
 * Uses the same encoding as the read path:
 *   1-249: direct value
 *   250-254 + next_byte: multi-byte for values 250-1524
 *   For larger values, use Space opcode. */
static int
gw_encode_flux_wire(const uint32_t *ticks, int tick_count, uint8_t *wire, int max_wire)
{
    int pos = 0;

    for (int i = 0; i < tick_count && pos < max_wire - 8; i++) {
        uint32_t t = ticks[i];

        if (t < 250) {
            if (t == 0) t = 1;
            wire[pos++] = (uint8_t) t;
        } else if (t <= 1524) {
            /* Multi-byte: first_byte = 250 + ((t-250) / 255), second_byte = (t-250) % 255 + 1 */
            uint32_t v = t - 250;
            wire[pos++] = (uint8_t)(250 + v / 255);
            wire[pos++] = (uint8_t)(v % 255 + 1);
        } else {
            /* Use Space opcode for very large values, then emit a 1-tick flux */
            wire[pos++] = GW_FLUX_ESCAPE;
            wire[pos++] = GW_FLUX_OP_SPACE;
            /* 28-bit encoding */
            wire[pos++] = (uint8_t)(((t >> 0) & 0x7F) << 1);
            wire[pos++] = (uint8_t)(((t >> 7) & 0x7F) << 1);
            wire[pos++] = (uint8_t)(((t >> 14) & 0x7F) << 1);
            wire[pos++] = (uint8_t)(((t >> 21) & 0x7F) << 1);
            wire[pos++] = 1; /* 1-tick flux transition */
        }
    }

    if (pos < max_wire)
        wire[pos++] = GW_FLUX_END;

    return pos;
}

static void
gw_writeback(int drive)
{
    gw_t *dev = gw[drive];
    if (dev == NULL || !dev->write_pending)
        return;

    gw_log("GW: Writing back modified sectors on drive %d\n", drive);

    for (int side = 0; side < dev->sides; side++) {
        /* Build MFM bitstream for entire track */
        static uint8_t mfm_raw[200000];
        int mfm_pos  = 0;
        int prev_bit = 0;

        /* Pre-index gap (gap4a) */
        for (int i = 0; i < 80; i++) {
            prev_bit = gw_encode_mfm_byte(0x4E, prev_bit, mfm_raw + mfm_pos);
            mfm_pos += 16;
        }

        for (int s = 0; s < dev->cache.sector_count[side]; s++) {
            uint8_t *id   = dev->cache.sector_id[side][s];
            uint8_t *data = dev->write_data[side][s];
            int      n    = id[3];
            int      sector_bytes = 128 << n;

            /* Sync (12x 0x00) */
            for (int i = 0; i < 12; i++) {
                prev_bit = gw_encode_mfm_byte(0x00, prev_bit, mfm_raw + mfm_pos);
                mfm_pos += 16;
            }

            /* 3x A1 sync + FE IDAM */
            for (int i = 0; i < 3; i++) {
                prev_bit = gw_encode_mfm_sync(prev_bit, mfm_raw + mfm_pos);
                mfm_pos += 16;
            }
            prev_bit = gw_encode_mfm_byte(0xFE, prev_bit, mfm_raw + mfm_pos);
            mfm_pos += 16;

            /* C, H, R, N */
            for (int i = 0; i < 4; i++) {
                prev_bit = gw_encode_mfm_byte(id[i], prev_bit, mfm_raw + mfm_pos);
                mfm_pos += 16;
            }

            /* IDAM CRC */
            uint8_t  crc_buf[8] = { 0xA1, 0xA1, 0xA1, 0xFE, id[0], id[1], id[2], id[3] };
            uint16_t crc = gw_crc16(crc_buf, 8);
            prev_bit = gw_encode_mfm_byte((crc >> 8) & 0xFF, prev_bit, mfm_raw + mfm_pos);
            mfm_pos += 16;
            prev_bit = gw_encode_mfm_byte(crc & 0xFF, prev_bit, mfm_raw + mfm_pos);
            mfm_pos += 16;

            /* Gap2 */
            for (int i = 0; i < dev->gap2_size; i++) {
                prev_bit = gw_encode_mfm_byte(0x4E, prev_bit, mfm_raw + mfm_pos);
                mfm_pos += 16;
            }

            /* Sync + DAM */
            for (int i = 0; i < 12; i++) {
                prev_bit = gw_encode_mfm_byte(0x00, prev_bit, mfm_raw + mfm_pos);
                mfm_pos += 16;
            }
            for (int i = 0; i < 3; i++) {
                prev_bit = gw_encode_mfm_sync(prev_bit, mfm_raw + mfm_pos);
                mfm_pos += 16;
            }
            prev_bit = gw_encode_mfm_byte(0xFB, prev_bit, mfm_raw + mfm_pos);
            mfm_pos += 16;

            /* Sector data */
            for (int i = 0; i < sector_bytes; i++) {
                prev_bit = gw_encode_mfm_byte(data[i], prev_bit, mfm_raw + mfm_pos);
                mfm_pos += 16;
            }

            /* Data CRC */
            uint8_t *dcrc_buf = (uint8_t *) malloc(4 + sector_bytes);
            dcrc_buf[0] = 0xA1;
            dcrc_buf[1] = 0xA1;
            dcrc_buf[2] = 0xA1;
            dcrc_buf[3] = 0xFB;
            memcpy(dcrc_buf + 4, data, sector_bytes);
            uint16_t dcrc = gw_crc16(dcrc_buf, 4 + sector_bytes);
            free(dcrc_buf);
            prev_bit = gw_encode_mfm_byte((dcrc >> 8) & 0xFF, prev_bit, mfm_raw + mfm_pos);
            mfm_pos += 16;
            prev_bit = gw_encode_mfm_byte(dcrc & 0xFF, prev_bit, mfm_raw + mfm_pos);
            mfm_pos += 16;

            /* Gap3 */
            for (int i = 0; i < dev->gap3_size; i++) {
                prev_bit = gw_encode_mfm_byte(0x4E, prev_bit, mfm_raw + mfm_pos);
                mfm_pos += 16;
            }
        }

        /* Convert MFM bits to flux ticks */
        static uint32_t flux_ticks[GW_MAX_FLUX_TICKS];
        double cell_ticks = (double) dev->sample_freq / ((double) dev->data_rate * 2.0);
        int    flux_count = 0;
        int    zero_count = 0;

        for (int i = 0; i < mfm_pos && flux_count < GW_MAX_FLUX_TICKS; i++) {
            if (mfm_raw[i]) {
                flux_ticks[flux_count++] = (uint32_t) ((zero_count + 1) * cell_ticks + 0.5);
                zero_count               = 0;
            } else {
                zero_count++;
            }
        }

        if (flux_count > 0) {
            static uint8_t wire[GW_MAX_FLUX_TICKS * 2];
            int wire_len = gw_encode_flux_wire(flux_ticks, flux_count, wire, sizeof(wire));

            gw_cmd_seek(dev, dev->cache.cylinder);
            gw_cmd_head(dev, side);

            /* WriteFlux: [cmd=8, len=4, cue_at_index=1, terminate_at_index=1] */
            uint8_t wf_cmd[] = { GW_CMD_WRITE_FLUX, 4, 1, 1 };
            if (gw_send_cmd(dev->fd, wf_cmd, sizeof(wf_cmd)) >= 0) {
                gw_serial_write(dev->fd, wire, wire_len);
                uint8_t status_cmd[] = { GW_CMD_GET_FLUX_STATUS, 2 };
                gw_send_cmd(dev->fd, status_cmd, sizeof(status_cmd));
            }
        }
    }

    dev->write_pending = 0;
}

/* ========================================================================
 * d86f Backend Handlers
 * ======================================================================== */
static uint16_t
gw_disk_flags(int drive)
{
    const gw_t *dev = gw[drive];
    return dev ? dev->disk_flags : 0;
}

static uint16_t
gw_side_flags(int drive)
{
    const gw_t *dev = gw[drive];
    return dev ? dev->track_flags : 0;
}

static void
gw_set_sector(int drive, UNUSED(int side), UNUSED(uint8_t c), uint8_t h,
              uint8_t r, UNUSED(uint8_t n))
{
    gw_t *dev = gw[drive];
    if (dev == NULL)
        return;

    /* Use h (head from sector ID) to determine which side, matching fdd_img.c pattern.
     * Prefer sectors without CRC errors when duplicates exist (from multi-revolution reads). */
    int s = h;
    if (s >= dev->sides)
        s = 0;

    /* First pass: find a CRC-good sector */
    for (int i = 0; i < dev->cache.sector_count[s]; i++) {
        if (dev->cache.sector_id[s][i][2] == r && !dev->cache.sector_error[s][i]) {
            dev->sel_side = s;
            dev->sel_idx  = i;
            return;
        }
    }
    /* Second pass: accept any sector (even with CRC error) */
    for (int i = 0; i < dev->cache.sector_count[s]; i++) {
        if (dev->cache.sector_id[s][i][2] == r) {
            dev->sel_side = s;
            dev->sel_idx  = i;
            gw_log("GW: set_sector: using CRC-bad sector h=%d r=%d\n", h, r);
            return;
        }
    }

    gw_log("GW: set_sector: h=%d r=%d NOT FOUND (side has %d sectors)\n",
           h, r, dev->cache.sector_count[s]);
    dev->sel_side = s;
    dev->sel_idx  = -1;
}

static uint8_t
gw_read_data(int drive, UNUSED(int side), uint16_t pos)
{
    const gw_t *dev = gw[drive];
    if (dev == NULL)
        return 0;

    int s   = dev->sel_side;
    int idx = dev->sel_idx;
    if (idx < 0 || idx >= dev->cache.sector_count[s])
        return 0;

    return dev->cache.sector_data[s][idx][pos];
}

static void
gw_write_data(int drive, UNUSED(int side), uint16_t pos, uint8_t data)
{
    gw_t *dev = gw[drive];
    if (dev == NULL)
        return;

    int s   = dev->sel_side;
    int idx = dev->sel_idx;
    if (idx < 0 || idx >= dev->cache.sector_count[s])
        return;

    dev->write_data[s][idx][pos] = data;
    dev->write_pending           = 1;
}

static int
gw_format_conditions(UNUSED(int drive))
{
    return 0;
}

/* ========================================================================
 * Seek Handler
 * ======================================================================== */
static void
gw_seek(int drive, int track)
{
    gw_t *dev = gw[drive];
    if (dev == NULL)
        return;

    int physical_track = track;
    if (fdd_doublestep_40(drive))
        physical_track /= 2;

    d86f_set_cur_track(drive, track);

    if (dev->cache.valid && dev->cache.cylinder == physical_track)
        goto prepare_track;

    dev->cache.valid    = 0;
    dev->cache.cylinder = physical_track;

    for (int side = 0; side < dev->sides; side++) {
        if (gw_read_physical_track(dev, physical_track, side) < 0)
            dev->cache.sector_count[side] = 0;
    }

    /* Auto-detect data rate on cylinder 0 */
    if (physical_track == 0 &&
        dev->cache.sector_count[0] == 0 && dev->cache.sector_count[1] == 0) {
        static const int alt_rates[] = { 500000, 300000, 250000 };
        int              orig        = dev->data_rate;

        for (int r = 0; r < 3; r++) {
            if (alt_rates[r] == orig)
                continue;
            dev->data_rate = alt_rates[r];
            for (int side = 0; side < dev->sides; side++)
                gw_read_physical_track(dev, physical_track, side);
            if (dev->cache.sector_count[0] > 0 || dev->cache.sector_count[1] > 0)
                break;
        }

        if (dev->cache.sector_count[0] == 0 && dev->cache.sector_count[1] == 0)
            dev->data_rate = orig;
    }

    dev->cache.valid = 1;

prepare_track:
    d86f_reset_index_hole_pos(drive, 0);
    d86f_reset_index_hole_pos(drive, 1);
    d86f_destroy_linked_lists(drive, 0);
    d86f_destroy_linked_lists(drive, 1);

    if (dev->cache.sector_count[0] == 0 && dev->cache.sector_count[1] == 0) {
        d86f_zero_track(drive);
        return;
    }

    for (int side = 0; side < dev->sides; side++) {
        int current_pos = d86f_prepare_pretrack(drive, side, 0);

        for (int s = 0; s < dev->cache.sector_count[side]; s++) {
            uint8_t *id    = dev->cache.sector_id[side][s];
            int      ssize = 128 << id[3];

            current_pos = d86f_prepare_sector(drive, side, current_pos, id,
                                              dev->cache.sector_data[side][s],
                                              ssize, dev->gap2_size, dev->gap3_size, 0);

            if (s == 0)
                d86f_initialize_last_sector_id(drive, id[0], id[1], id[2], id[3]);
        }
    }
}

/* ========================================================================
 * Geometry Detection from Drive Type
 * ======================================================================== */
static void
gw_detect_geometry(gw_t *dev, int drive)
{
    dev->sides = fdd_is_double_sided(drive) ? 2 : 1;

    if (fdd_is_ed(drive)) {
        dev->data_rate   = 1000000;
        dev->is_hd       = 1;
        dev->tracks      = 80;
        dev->sectors     = 36;
        dev->sector_size = 2;
        dev->disk_flags  = 0x04;
        dev->track_flags = 0x0B;
    } else if (fdd_is_hd(drive)) {
        if (fdd_is_525(drive)) {
            dev->data_rate   = 500000;
            dev->is_hd       = 1;
            dev->tracks      = 80;
            dev->sectors     = 15;
            dev->sector_size = 2;
            dev->disk_flags  = 0x02;
            dev->track_flags = 0x08;
        } else {
            dev->data_rate   = 500000;
            dev->is_hd       = 1;
            dev->tracks      = 80;
            dev->sectors     = 18;
            dev->sector_size = 2;
            dev->disk_flags  = 0x02;
            dev->track_flags = 0x08;
        }
    } else {
        if (fdd_is_525(drive)) {
            dev->data_rate   = 250000;
            dev->is_hd       = 0;
            dev->tracks      = 40;
            dev->sectors     = 9;
            dev->sector_size = 2;
            dev->disk_flags  = 0x00;
            dev->track_flags = 0x0A;
        } else {
            dev->data_rate   = 250000;
            dev->is_hd       = 0;
            dev->tracks      = 80;
            dev->sectors     = 9;
            dev->sector_size = 2;
            dev->disk_flags  = 0x00;
            dev->track_flags = 0x0A;
        }
    }

    if (dev->sides == 2)
        dev->disk_flags |= 0x08;

    dev->gap2_size = (dev->data_rate >= 500000) ? 41 : 22;
    dev->gap3_size = gap3_sizes[dev->track_flags & 3][dev->sector_size][dev->sectors];
    if (!dev->gap3_size)
        dev->gap3_size = 0x1B;

    gw_log("GW: Geometry: %d tracks, %d sides, %d sectors, rate=%d Hz\n",
           dev->tracks, dev->sides, dev->sectors, dev->data_rate);
}

/* ========================================================================
 * Public API -- Linux implementations
 * ======================================================================== */
/* Check if a device path is already in use by a loaded drive */
static int
gw_device_in_use(const char *path)
{
    for (int d = 0; d < FDD_NUM; d++) {
        if (gw[d] != NULL && gw[d]->fd >= 0 && strcmp(gw[d]->dev_path, path) == 0)
            return 1;
    }
    return 0;
}

int
gw_detect_devices(char devices[][256], int max_devices)
{
    int  count = 0;
    char path[64];

    for (int i = 0; i < 10 && count < max_devices; i++) {
        snprintf(path, sizeof(path), "/dev/ttyACM%d", i);

        /* Skip devices already in use -- probing would corrupt their state */
        if (gw_device_in_use(path)) {
            strncpy(devices[count], path, 255);
            devices[count][255] = '\0';
            count++;
            continue;
        }

        int fd = gw_serial_open(path);
        if (fd < 0)
            continue;

        /* Send GetInfo command: [cmd=0, len=3, sub=0(Firmware)] */
        uint8_t cmd[] = { GW_CMD_GET_INFO, 3, GW_INFO_FIRMWARE };
        gw_serial_write(fd, cmd, sizeof(cmd));

        /* Read 2-byte ack: [cmd_echo, ack_status] */
        uint8_t ack[2];
        int n = gw_serial_read(fd, ack, 2);

        if (n >= 2 && ack[0] == GW_CMD_GET_INFO && ack[1] == GW_ACK_OK) {
            /* Valid GW device -- drain the 32-byte info response */
            uint8_t info[32];
            gw_serial_read(fd, info, 32);

            strncpy(devices[count], path, 255);
            devices[count][255] = '\0';
            count++;
            gw_log("GW: Detected device at %s\n", path);
        }

        gw_serial_close(fd);
    }

    return count;
}

void
gw_load(int drive, char *fn)
{
    gw_t       *dev;
    const char *dev_path;

    if (strstr(fn, "greaseweazle://") != fn) {
        gw_log("GW: Invalid URI: %s\n", fn);
        return;
    }
    dev_path = fn + strlen("greaseweazle://");

    d86f_unregister(drive);

    writeprot[drive]  = 0;
    fwriteprot[drive] = 0;

    dev = (gw_t *) calloc(1, sizeof(gw_t));
    dev->fd               = -1;
    dev->current_cylinder = -1;
    dev->cache.valid      = 0;

    strncpy(dev->dev_path, dev_path, sizeof(dev->dev_path) - 1);

    /* Prevent the same physical device from being loaded on two drives */
    if (gw_device_in_use(dev_path)) {
        gw_log("GW: Device %s already in use by another drive\n", dev_path);
        free(dev);
        return;
    }

    dev->fd = gw_serial_open(dev_path);
    if (dev->fd < 0) {
        gw_log("GW: Failed to open %s\n", dev_path);
        free(dev);
        return;
    }

    /* Get firmware info */
    if (gw_cmd_get_info(dev) < 0) {
        gw_log("GW: Failed to get device info\n");
        gw_serial_close(dev->fd);
        free(dev);
        return;
    }

    if (dev->sample_freq == 0)
        dev->sample_freq = 72000000;

    if (gw_cmd_set_bus_type(dev, GW_BUS_IBMPC) < 0)
        gw_log("GW: SetBusType failed (non-fatal)\n");
    if (gw_cmd_select(dev) < 0)
        gw_log("GW: Select failed\n");
    if (gw_cmd_motor(dev, 1) < 0)
        gw_log("GW: Motor ON failed\n");
    dev->motor_on = 1;

    gw_detect_geometry(dev, drive);

    gw[drive] = dev;

    d86f_handler[drive].disk_flags        = gw_disk_flags;
    d86f_handler[drive].side_flags        = gw_side_flags;
    d86f_handler[drive].writeback         = gw_writeback;
    d86f_handler[drive].set_sector        = gw_set_sector;
    d86f_handler[drive].read_data         = gw_read_data;
    d86f_handler[drive].write_data        = gw_write_data;
    d86f_handler[drive].format_conditions = gw_format_conditions;
    d86f_handler[drive].extra_bit_cells   = null_extra_bit_cells;
    d86f_handler[drive].encoded_data      = common_encoded_data;
    d86f_handler[drive].read_revolution   = common_read_revolution;
    d86f_handler[drive].index_hole_pos    = null_index_hole_pos;
    d86f_handler[drive].get_raw_size      = common_get_raw_size;
    d86f_handler[drive].check_crc         = 1;
    d86f_set_version(drive, D86FVER);

    drives[drive].seek = gw_seek;
    d86f_common_handlers(drive);

    gw_log("GW: Drive %d loaded from %s\n", drive, dev_path);
}

void
gw_close(int drive)
{
    gw_t *dev = gw[drive];
    if (dev == NULL)
        return;

    if (dev->write_pending)
        gw_writeback(drive);

    d86f_unregister(drive);

    if (dev->fd >= 0) {
        if (dev->motor_on) {
            gw_cmd_motor(dev, 0);
            dev->motor_on = 0;
        }
        gw_cmd_deselect(dev);
        gw_serial_close(dev->fd);
        dev->fd = -1;
    }

    free(dev);
    gw[drive] = NULL;
}

void
gw_set_fdc(void *fdc)
{
    gw_fdc = (fdc_t *) fdc;
}

#else /* !__linux__ */

/* Non-Linux stubs */
int
gw_detect_devices(char devices[][256], int max_devices)
{
    (void) devices;
    (void) max_devices;
    return 0;
}

void
gw_load(int drive, char *fn)
{
    (void) drive;
    (void) fn;
    pclog("GW: GreaseWeazle not supported on this platform\n");
}

void
gw_close(int drive)
{
    (void) drive;
}

void
gw_set_fdc(void *fdc)
{
    gw_fdc = (fdc_t *) fdc;
}

#endif /* __linux__ */
