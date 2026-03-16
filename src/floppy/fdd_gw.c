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

#ifdef ENABLE_GW_LOG
int gw_do_log = ENABLE_GW_LOG;

static void
gw_log(const char *fmt, ...)
{
    va_list ap;

    if (gw_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
#else
#    define gw_log(fmt, ...)
#endif

#ifdef __linux__

#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>
#include <errno.h>

/* GreaseWeazle protocol commands */
#define GW_CMD_GET_INFO    0
#define GW_CMD_SEEK        7
#define GW_CMD_HEAD        8
#define GW_CMD_MOTOR       11
#define GW_CMD_READ_FLUX   12
#define GW_CMD_WRITE_FLUX  13
#define GW_CMD_GET_FLUX_STATUS 14
#define GW_CMD_SELECT      18
#define GW_CMD_DESELECT    19
#define GW_CMD_SET_BUS_TYPE 20
#define GW_CMD_RESET       23

/* GW ack status codes */
#define GW_ACK_OK       0

/* GW bus types */
#define GW_BUS_IBMPC    1

/* GW GetInfo sub-commands */
#define GW_INFO_FIRMWARE 0

/* Flux stream opcodes */
#define GW_FLUX_OVFL16   0xFF
#define GW_FLUX_INDEX    1
#define GW_FLUX_SPACE    2
#define GW_FLUX_END      0

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
    int      is_v4;

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

    int sel_sector_idx[2];

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
        tv.tv_sec  = 3;
        tv.tv_usec = 0;

        int ret = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (ret <= 0)
            break;

        int n = read(fd, buf + total, maxlen - total);
        if (n <= 0)
            break;

        for (int i = total; i < total + n; i++) {
            if (buf[i] == GW_FLUX_END && i > 0)
                return i + 1;
        }
        total += n;
    }
    return total;
}

/* ========================================================================
 * GW Protocol Layer
 * ======================================================================== */
static int
gw_send_cmd(gw_t *dev, uint8_t cmd, const uint8_t *params, int param_len)
{
    uint8_t frame[64];
    int     frame_len;

    if (dev->is_v4) {
        int total = 4 + param_len;
        frame[0]  = total & 0xFF;
        frame[1]  = (total >> 8) & 0xFF;
        frame[2]  = cmd;
        frame[3]  = 0;
        if (param_len > 0)
            memcpy(frame + 4, params, param_len);
        frame_len = total;
    } else {
        frame_len = 2 + param_len;
        frame[0]  = frame_len;
        frame[1]  = cmd;
        if (param_len > 0)
            memcpy(frame + 2, params, param_len);
    }

    return gw_serial_write(dev->fd, frame, frame_len);
}

static int
gw_recv_ack(gw_t *dev, uint8_t expected_cmd)
{
    uint8_t resp[64];
    int     hdr_len = dev->is_v4 ? 4 : 2;

    int n = gw_serial_read(dev->fd, resp, hdr_len);
    if (n < hdr_len)
        return -1;

    int resp_len;
    if (dev->is_v4)
        resp_len = resp[0] | (resp[1] << 8);
    else
        resp_len = resp[0];

    int extra = resp_len - hdr_len;
    if (extra > 0 && extra < (int) sizeof(resp) - hdr_len)
        gw_serial_read(dev->fd, resp + hdr_len, extra);

    if (dev->is_v4) {
        if (resp[2] != (expected_cmd | 0x80) || resp[3] != GW_ACK_OK)
            return -1;
    } else {
        if (!(resp[1] & 0x80))
            return -1;
        if (extra > 0 && resp[hdr_len] != GW_ACK_OK)
            return -1;
    }

    return 0;
}

static int
gw_cmd_get_info(gw_t *dev)
{
    uint8_t params[4] = { GW_INFO_FIRMWARE, 0, 0, 0 };
    uint8_t resp[32];
    int     hdr_len = dev->is_v4 ? 4 : 2;

    if (gw_send_cmd(dev, GW_CMD_GET_INFO, params, dev->is_v4 ? 4 : 1) < 0)
        return -1;

    int n = gw_serial_read(dev->fd, resp, hdr_len);
    if (n < hdr_len)
        return -1;

    int resp_len = dev->is_v4 ? (resp[0] | (resp[1] << 8)) : resp[0];
    int extra    = resp_len - hdr_len;
    if (extra > 0 && extra <= (int) sizeof(resp) - hdr_len)
        gw_serial_read(dev->fd, resp + hdr_len, extra);

    if (dev->is_v4) {
        if (extra < 8)
            return -1;
        dev->fw_major    = resp[4];
        dev->fw_minor    = resp[5];
        dev->sample_freq = resp[8] | (resp[9] << 8) | (resp[10] << 16) | (resp[11] << 24);
        if (extra >= 9)
            dev->hw_model = resp[12];
    } else {
        if (extra < 6)
            return -1;
        dev->fw_major    = resp[2];
        dev->fw_minor    = resp[3];
        dev->sample_freq = resp[5] | (resp[6] << 8) | (resp[7] << 16) | (resp[8] << 24);
    }

    gw_log("GW: firmware v%d.%d, sample_freq=%u Hz\n",
           dev->fw_major, dev->fw_minor, dev->sample_freq);
    return 0;
}

static int
gw_cmd_set_bus_type(gw_t *dev, uint8_t bus_type)
{
    uint8_t params[4] = { bus_type, 0, 0, 0 };
    if (gw_send_cmd(dev, GW_CMD_SET_BUS_TYPE, params, dev->is_v4 ? 4 : 1) < 0)
        return -1;
    return gw_recv_ack(dev, GW_CMD_SET_BUS_TYPE);
}

static int
gw_cmd_select(gw_t *dev)
{
    if (gw_send_cmd(dev, GW_CMD_SELECT, NULL, 0) < 0)
        return -1;
    return gw_recv_ack(dev, GW_CMD_SELECT);
}

static int
gw_cmd_deselect(gw_t *dev)
{
    if (gw_send_cmd(dev, GW_CMD_DESELECT, NULL, 0) < 0)
        return -1;
    return gw_recv_ack(dev, GW_CMD_DESELECT);
}

static int
gw_cmd_motor(gw_t *dev, int on)
{
    uint8_t params[4] = { 0, on ? 1 : 0, 0, 0 };
    if (gw_send_cmd(dev, GW_CMD_MOTOR, params, dev->is_v4 ? 4 : 2) < 0)
        return -1;
    return gw_recv_ack(dev, GW_CMD_MOTOR);
}

static int
gw_cmd_seek(gw_t *dev, int cylinder)
{
    uint8_t params[4] = { (uint8_t) cylinder, 0, 0, 0 };
    if (gw_send_cmd(dev, GW_CMD_SEEK, params, dev->is_v4 ? 4 : 1) < 0)
        return -1;
    return gw_recv_ack(dev, GW_CMD_SEEK);
}

static int
gw_cmd_head(gw_t *dev, int head)
{
    uint8_t params[4] = { (uint8_t) head, 0, 0, 0 };
    if (gw_send_cmd(dev, GW_CMD_HEAD, params, dev->is_v4 ? 4 : 1) < 0)
        return -1;
    return gw_recv_ack(dev, GW_CMD_HEAD);
}

static int
gw_cmd_read_flux(gw_t *dev, uint8_t *flux_buf, int max_flux_len)
{
    uint8_t params[8];
    memset(params, 0, sizeof(params));
    params[0] = 2; /* 2 revolutions */

    if (gw_send_cmd(dev, GW_CMD_READ_FLUX, params, dev->is_v4 ? 8 : 2) < 0)
        return -1;

    if (gw_recv_ack(dev, GW_CMD_READ_FLUX) < 0)
        return -1;

    int n = gw_serial_read_flux(dev->fd, flux_buf, max_flux_len);

    if (gw_send_cmd(dev, GW_CMD_GET_FLUX_STATUS, NULL, 0) >= 0)
        gw_recv_ack(dev, GW_CMD_GET_FLUX_STATUS);

    return n;
}

/* ========================================================================
 * Flux-to-MFM PLL Decoder
 * ======================================================================== */
static int
gw_decode_flux_stream(const uint8_t *raw, int raw_len, uint32_t *ticks, int max_ticks)
{
    int      tick_count = 0;
    uint32_t accum      = 0;

    for (int i = 0; i < raw_len && tick_count < max_ticks; i++) {
        uint8_t b = raw[i];

        if (b == GW_FLUX_END)
            break;
        if (b == GW_FLUX_INDEX)
            continue;
        if (b == GW_FLUX_SPACE) {
            if (i + 2 < raw_len) {
                accum += (raw[i + 1] << 8) | raw[i + 2];
                i += 2;
            }
            continue;
        }
        if (b == GW_FLUX_OVFL16) {
            accum += 250;
            continue;
        }

        ticks[tick_count++] = accum + b;
        accum               = 0;
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
    /* A1 sync with missing clock, 3 consecutive (48 bits total) */
    static const uint8_t sync_pattern[] = {
        0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1,
        0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1,
        0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1
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

    dev->cache.sector_count[side] = sector_count;
    gw_log("GW: Parsed %d sectors on side %d\n", sector_count, side);
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

    if (dev->current_cylinder != cylinder) {
        if (gw_cmd_seek(dev, cylinder) < 0)
            return -1;
        dev->current_cylinder = cylinder;
    }

    if (gw_cmd_head(dev, side) < 0)
        return -1;

    int raw_len = gw_cmd_read_flux(dev, flux_raw, sizeof(flux_raw));
    if (raw_len <= 0)
        return -1;

    int tick_count = gw_decode_flux_stream(flux_raw, raw_len, flux_ticks, GW_MAX_FLUX_TICKS);
    if (tick_count == 0)
        return -1;

    int bit_count = gw_flux_to_mfm(dev->sample_freq, dev->data_rate, flux_ticks, tick_count,
                                    mfm_bits, sizeof(mfm_bits));
    if (bit_count == 0)
        return -1;

    gw_parse_mfm_track(dev, mfm_bits, bit_count, side);
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
    static const uint8_t sync_a1[] = { 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1 };
    memcpy(mfm_out, sync_a1, 16);
    (void) prev_bit;
    return 1;
}

static int
gw_encode_flux_wire(const uint32_t *ticks, int tick_count, uint8_t *wire, int max_wire)
{
    int pos = 0;

    for (int i = 0; i < tick_count && pos < max_wire - 4; i++) {
        uint32_t t = ticks[i];
        while (t >= 250 && pos < max_wire - 2) {
            wire[pos++] = GW_FLUX_OVFL16;
            t -= 250;
        }
        if (t == 0)
            t = 1;
        wire[pos++] = (uint8_t) t;
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

            if (gw_send_cmd(dev, GW_CMD_WRITE_FLUX, NULL, 0) >= 0) {
                if (gw_recv_ack(dev, GW_CMD_WRITE_FLUX) >= 0) {
                    gw_serial_write(dev->fd, wire, wire_len);
                    gw_send_cmd(dev, GW_CMD_GET_FLUX_STATUS, NULL, 0);
                    gw_recv_ack(dev, GW_CMD_GET_FLUX_STATUS);
                }
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
gw_set_sector(int drive, UNUSED(int side), UNUSED(uint8_t c), UNUSED(uint8_t h),
              uint8_t r, UNUSED(uint8_t n))
{
    gw_t *dev = gw[drive];
    if (dev == NULL)
        return;

    for (int s = 0; s < 2; s++) {
        for (int i = 0; i < dev->cache.sector_count[s]; i++) {
            if (dev->cache.sector_id[s][i][2] == r) {
                dev->sel_sector_idx[s] = i;
                return;
            }
        }
    }
}

static uint8_t
gw_read_data(int drive, int side, uint16_t pos)
{
    const gw_t *dev = gw[drive];
    if (dev == NULL)
        return 0;

    int idx = dev->sel_sector_idx[side];
    if (idx < 0 || idx >= dev->cache.sector_count[side])
        return 0;

    return dev->cache.sector_data[side][idx][pos];
}

static void
gw_write_data(int drive, int side, uint16_t pos, uint8_t data)
{
    gw_t *dev = gw[drive];
    if (dev == NULL)
        return;

    int idx = dev->sel_sector_idx[side];
    if (idx < 0 || idx >= dev->cache.sector_count[side])
        return;

    dev->write_data[side][idx][pos] = data;
    dev->write_pending              = 1;
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
int
gw_detect_devices(char devices[][256], int max_devices)
{
    int  count = 0;
    char path[64];

    for (int i = 0; i < 10 && count < max_devices; i++) {
        snprintf(path, sizeof(path), "/dev/ttyACM%d", i);

        int fd = gw_serial_open(path);
        if (fd < 0)
            continue;

        uint8_t cmd_v3[] = { 3, GW_CMD_GET_INFO, GW_INFO_FIRMWARE };
        uint8_t resp[32];

        gw_serial_write(fd, cmd_v3, 3);

        struct termios tio;
        tcgetattr(fd, &tio);
        tio.c_cc[VTIME] = 5;
        tcsetattr(fd, TCSANOW, &tio);

        int n = read(fd, resp, sizeof(resp));

        if (n >= 4 && (resp[1] & 0x80)) {
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

    dev->fd = gw_serial_open(dev_path);
    if (dev->fd < 0) {
        gw_log("GW: Failed to open %s\n", dev_path);
        free(dev);
        return;
    }

    /* Determine protocol version */
    dev->is_v4 = 0;
    if (gw_cmd_get_info(dev) < 0) {
        dev->is_v4 = 1;
        tcflush(dev->fd, TCIOFLUSH);
        if (gw_cmd_get_info(dev) < 0) {
            gw_log("GW: Failed to get device info\n");
            gw_serial_close(dev->fd);
            free(dev);
            return;
        }
    }

    if (dev->fw_major >= 4)
        dev->is_v4 = 1;

    if (dev->sample_freq == 0)
        dev->sample_freq = 72000000;

    gw_cmd_set_bus_type(dev, GW_BUS_IBMPC);
    gw_cmd_select(dev);
    gw_cmd_motor(dev, 1);
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
