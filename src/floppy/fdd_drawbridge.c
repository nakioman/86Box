/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Implementation of the DrawBridge Arduino floppy drive reader
 *          interface for 86Box. Based on the USB floppy implementation.
 *
 * Authors: Ruud van der Moer
 *          GitHub Copilot AI Assistant
 *
 *          Copyright 2024 Ruud van der Moer.
 *          Copyright 2025 86Box contributors.
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
#include <86box/86box.h>
#include <86box/timer.h>
#include <86box/crc.h>
#include <86box/plat.h>
#include <86box/fdd.h>
#include <86box/fdd_86f.h>
#include <86box/fdd_drawbridge.h>
#include <86box/fdc.h>
#include <86box/drawbridge.h>

/* DrawBridge floppy structure - interfaces with Arduino hardware for real floppy reading */
typedef struct drawbridge_t {
    ArduinoInterface *arduino;       /* Arduino interface for communication */
    int               track;         /* Current track */
    int               heads;         /* Number of heads (detected from disk) */
    int               sectors;       /* Sectors per track (detected from disk) */
    int               tracks;        /* Total number of tracks (detected from disk) */
    bool              disk_inserted; /* Whether a disk is present */
    bool              is_hd;         /* Whether disk is HD (1.44MB) or DD (720KB) */
    uint16_t          disk_flags;    /* Disk flags */
    uint16_t          track_flags;   /* Track flags for MFM encoding and data rate */
    uint8_t           gap2_size;     /* Gap2 size for d86f */
    uint8_t           gap3_size;     /* Gap3 size for d86f */
    uint8_t           data_rate;     /* Data rate index */
    /* Track caching for performance */
    uint8_t track_data[2][RAW_TRACKDATA_LENGTH_HD]; /* Raw MFM track data for both sides */
    bool    track_data_valid[2];                    /* Whether cached track data is valid for each side */
    int     cached_track;                           /* Track number of cached data */
    /* Sector tracking for read_data callback */
    uint8_t current_sector_track;     /* Track of currently selected sector */
    uint8_t current_sector_head;      /* Head of currently selected sector */
    uint8_t current_sector_r;         /* Sector number of currently selected sector */
    uint8_t current_sector_data[512]; /* Data buffer for currently selected sector */
    int     current_sector_valid;     /* Whether current sector data is valid */
    char   *device_path;              /* Device path (/dev/ttyUSB0) */
} drawbridge_t;

static drawbridge_t *drawbridge_fdd[FDD_NUM];
static fdc_t        *drawbridge_fdd_fdc;

#ifdef ENABLE_DRAWBRIDGE_LOG
int drawbridge_fdd_do_log = ENABLE_DRAWBRIDGE_LOG;

static void
drawbridge_fdd_log(const char *fmt, ...)
{
    va_list ap;

    if (drawbridge_fdd_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
#else
#    define drawbridge_fdd_log(fmt, ...)
#endif

/* IBM PC MFM decoder structures and functions (adapted from ibm_pc.c) */

// Helper function to get maximum of two integers
static int32_t
max_int32(int32_t a, int32_t b)
{
    return a > b ? a : b;
}

// Simple byte swap
static uint16_t
wordSwap(uint16_t w)
{
    return (w << 8) | (w >> 8);
}

// CRC16 with custom initial value
static uint16_t
crc16(const char *pData, uint32_t length, uint16_t wCrc)
{
    uint8_t i;
    while (length--) {
        wCrc ^= *(unsigned char *) pData++ << 8;
        for (i = 0; i < 8; i++)
            wCrc = wCrc & 0x8000 ? (wCrc << 1) ^ 0x1021 : wCrc << 1;
    }
    return wCrc & 0xffff;
}

static void
init_raw_decoded_sector(RawDecodedSector *sector)
{
    sector->data     = NULL;
    sector->size     = 0;
    sector->capacity = 0;
}

static void
resize_raw_decoded_sector(RawDecodedSector *sector, uint32_t newSize)
{
    if (newSize > sector->capacity) {
        uint32_t newCapacity = newSize * 2; // grow by 2x
        sector->data         = (uint8_t *) realloc(sector->data, newCapacity);
        sector->capacity     = newCapacity;
    }
    sector->size = newSize;
}

static void
free_raw_decoded_sector(RawDecodedSector *sector)
{
    if (sector->data) {
        free(sector->data);
        sector->data = NULL;
    }
    sector->size     = 0;
    sector->capacity = 0;
}

// Helper functions for managing the sector map (linked list)
static SectorMapEntry *
find_sector_entry(DecodedTrack *track, int sectorNumber)
{
    SectorMapEntry *current = track->sectors;
    while (current != NULL) {
        if (current->sectorNumber == sectorNumber) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

static void
add_sector_entry(DecodedTrack *track, int sectorNumber, DecodedSector *sector)
{
    SectorMapEntry *entry = (SectorMapEntry *) malloc(sizeof(SectorMapEntry));
    entry->sectorNumber   = sectorNumber;
    entry->sector         = *sector;
    entry->next           = track->sectors;
    track->sectors        = entry;
}

static void
update_sector_entry(SectorMapEntry *entry, DecodedSector *sector)
{
    // Free old data if it exists
    free_raw_decoded_sector(&entry->sector.data);
    entry->sector = *sector;
}

// Function to free all sectors in a decoded track
static void
free_decoded_track(DecodedTrack *track)
{
    SectorMapEntry *current = track->sectors;
    while (current != NULL) {
        SectorMapEntry *next = current->next;
        free_raw_decoded_sector(&current->sector.data);
        free(current);
        current = next;
    }
    track->sectors           = NULL;
    track->sectorsWithErrors = 0;
}

// Extract the data, properly aligned into the output
static void
extractMFMDecodeRaw(const unsigned char *inTrack, const uint32_t dataLengthInBits,
                    const uint32_t bitPos, uint32_t outputBytes, uint8_t *output)
{
    uint32_t       realBitPos = bitPos + 1; // the +1 skips past the clock bit
    unsigned char *memOut     = output;

    while (outputBytes) {
        *memOut = 0; // Initialize the output byte
        for (uint32_t bit = 0; bit <= 7; bit++) {
            *memOut <<= 1;
            const uint32_t trackBytePos = realBitPos >> 3;
            const uint32_t trackBitPos  = 7 - (realBitPos & 7);
            if (inTrack[trackBytePos] & (1 << trackBitPos))
                *memOut |= 1;
            realBitPos = (realBitPos + 2) % dataLengthInBits; // skip those clock bits
        }
        // Move along and reset
        memOut++;
        outputBytes--;
    }
}

// Main IBM PC MFM sector finder function (adapted from ibm_pc.c)
static void
findSectors_IBM(const uint8_t *track, const uint32_t dataLengthInBits, const bool isHD,
                const uint32_t cylinder, const uint32_t head, const uint32_t expectedNumSectors,
                DecodedTrack *decodedTrack, bool *nonstandardTimings)
{
    const bool upperSide = (head == 1);

    drawbridge_fdd_log("DrawBridge: IBM MFM decoder processing cylinder %d head %d, %d bits, HD=%s\n",
                       cylinder, head, dataLengthInBits, isHD ? "yes" : "no");

    // Prepare our test buffer
    uint64_t  decoded = 0;
    IBMSector sector;

    bool headerFound    = false;
    sector.headerErrors = 0xFFFF;
    sector.dataValid    = false;
    // Initialize sector data
    sector.data.data         = NULL;
    sector.data.dataSize     = 0;
    int8_t  lastSectorNumber = -1;
    uint8_t sectorSize       = 2; // default - 512
    *nonstandardTimings      = false;

    uint32_t expectedSectors = expectedNumSectors ? expectedNumSectors : (isHD ? IBM_HD_SECTORS : IBM_DD_SECTORS);
    uint32_t sectorEndPoint  = 0;

    uint32_t gapTotal         = 0;
    uint32_t numGaps          = 0;
    uint32_t syncHeadersFound = 0;
    uint32_t syncDataFound    = 0;

    // run the entire track length with some space to wrap around
    for (uint32_t bit = 0; bit < dataLengthInBits; bit++) {
        const uint32_t realBitPos   = bit % dataLengthInBits;
        const uint32_t trackBytePos = realBitPos >> 3;
        const uint32_t trackBitPos  = 7 - (realBitPos & 7);
        decoded <<= 1ULL; // shift off one bit to make room for the new bit
        if (track[trackBytePos] & (1 << trackBitPos))
            decoded |= 1;

        if (decoded == MFM_SYNC_SECTOR_HEADER) {
            syncHeadersFound++;
            // Grab sector header
            if (sectorEndPoint) {
                uint32_t markerStart         = bit + 1 - 64;
                uint32_t bytesBetweenSectors = (markerStart - sectorEndPoint) / 16;
                bytesBetweenSectors          = max_int32(0, (int32_t) bytesBetweenSectors - (12 * 2)); // these would be the SYNC AA or 55
                if (bytesBetweenSectors > 200)
                    bytesBetweenSectors = 200; // shouldnt get this high
                // For a PC disk this should be around 84
                gapTotal += bytesBetweenSectors;
                numGaps++;
            }

            extractMFMDecodeRaw(track, dataLengthInBits, bit + 1 - 64, sizeof(sector.header), (uint8_t *) &sector.header);
            uint16_t crc        = crc16((char *) &sector.header, sizeof(sector.header) - 2, 0xFFFF);
            sector.headerErrors = 0;
            headerFound         = true;

            if (sector.header.sector < 1) {
                sector.header.sector = 1;
                sector.headerErrors++;
                drawbridge_fdd_log("DrawBridge: Fixed sector number < 1\n");
            }

            if (crc != wordSwap(*(uint16_t *) sector.header.crc)) {
                sector.headerErrors++;
                drawbridge_fdd_log("DrawBridge: Header CRC mismatch\n");
            }
            if (!sector.headerErrors)
                sectorSize = sector.header.length;
            if (sector.header.cylinder != cylinder) {
                sector.headerErrors++;
            }
            if (sector.header.head != (upperSide ? 1 : 0)) {
                sector.headerErrors++;
                drawbridge_fdd_log("DrawBridge: Head mismatch: expected %d, got %d\n", upperSide ? 1 : 0, sector.header.head);
            }

            lastSectorNumber = sector.header.sector;
        } else if (decoded == MFM_SYNC_SECTOR_DATA) {
            syncDataFound++;
            if (!headerFound) {
                // Sector header was missing. We'll "guess" one - not ideal!
                lastSectorNumber++;
                memset(&sector.header, 0, sizeof(sector.header));
                sector.header.sector   = (uint8_t) lastSectorNumber;
                sector.header.length   = sectorSize;
                sector.header.cylinder = cylinder;
                sector.header.head     = upperSide ? 1 : 0;
                sector.headerErrors    = 0xF0;
            }
            if (headerFound) {
                const uint32_t sectorDataSize = 1 << (7 + sector.header.length);
                if (sector.data.dataSize != sectorDataSize) {
                    sector.data.data     = (unsigned char *) realloc(sector.data.data, sectorDataSize);
                    sector.data.dataSize = sectorDataSize;
                }
                uint32_t bitStart = bit + 1 - 64;
                // Extract the header section
                extractMFMDecodeRaw(track, dataLengthInBits, bitStart, 4, (uint8_t *) &sector.data.dataMark);
                // Extract the sector data
                bitStart += 4 * 8 * 2;
                extractMFMDecodeRaw(track, dataLengthInBits, bitStart, sector.data.dataSize, sector.data.data);
                // Extract the sector CRC
                bitStart += sectorDataSize * 8 * 2;
                extractMFMDecodeRaw(track, dataLengthInBits, bitStart, 2, (uint8_t *) &sector.data.crc);
                // Validate
                uint16_t crc     = crc16((char *) &sector.data.dataMark, 4, 0xFFFF);
                crc              = crc16((char *) sector.data.data, sector.data.dataSize, crc);
                sector.dataValid = crc == wordSwap(*(uint16_t *) sector.data.crc);

                // Debug: Print data validation info only for errors
                if (!sector.dataValid) {
                    drawbridge_fdd_log("DrawBridge: Data CRC FAIL calc:0x%04X stored:0x%04X\n",
                                       crc, wordSwap(*(uint16_t *) sector.data.crc));
                }

                // Standardize the sector
                DecodedSector sec;
                init_raw_decoded_sector(&sec.data);
                resize_raw_decoded_sector(&sec.data, sector.data.dataSize);
                memcpy(sec.data.data, sector.data.data, sector.data.dataSize);
                sec.numErrors = sector.headerErrors + (sector.dataValid ? 0 : 1);

                // See if this already exists
                SectorMapEntry *existing = find_sector_entry(decodedTrack, sector.header.sector - 1);
                if (existing == NULL) {
                    if (sector.header.sector <= 22)
                        add_sector_entry(decodedTrack, sector.header.sector - 1, &sec);
                } else {
                    // Does exist. Keep the better copy
                    if (existing->sector.numErrors > sec.numErrors) {
                        update_sector_entry(existing, &sec);
                    }
                }

                // Reset for next sector
                sector.headerErrors = 0xFFFF;
                sector.dataValid    = false;
                headerFound         = false;
                sectorEndPoint      = bitStart + (4 * 8); // mark the end of the sector
            }
        } else if (decoded == MFM_SYNC_TRACK_HEADER) {
            // Reset here, not really required, but why not!
            headerFound         = false;
            sector.headerErrors = 0xFFFF;
            sector.dataValid    = false;
            lastSectorNumber    = -1;
        }
    }

    // Debug output
    drawbridge_fdd_log("DrawBridge: Found %d sector headers, %d sector data markers\n", syncHeadersFound, syncDataFound);

    // Work out the average gap size
    if (numGaps) {
        gapTotal /= numGaps;
        *nonstandardTimings = gapTotal < 70; // less than is probably an Atari ST disk
    }

    const uint32_t sectorDataSize = 1 << (7 + sectorSize);

    // Add dummy sectors up to expectedSectors
    decodedTrack->sectorsWithErrors = 0;
    for (uint32_t sec = 0; sec < expectedSectors; sec++) {
        SectorMapEntry *existing = find_sector_entry(decodedTrack, sec);

        // Does a sector with this number exist?
        if (existing == NULL) {
            if (expectedNumSectors) {
                // No. Create a dummy one - VERY NOT IDEAL!
                DecodedSector tmp;
                init_raw_decoded_sector(&tmp.data);
                resize_raw_decoded_sector(&tmp.data, sectorDataSize);
                memset(tmp.data.data, 0, sectorDataSize);
                tmp.numErrors = 0xFFFF;
                add_sector_entry(decodedTrack, sec, &tmp);
                decodedTrack->sectorsWithErrors++;
            }
        } else if (existing->sector.numErrors)
            decodedTrack->sectorsWithErrors++;
    }

    // Clean up
    if (sector.data.data) {
        free(sector.data.data);
    }
}

/* Gap size calculation tables (from fdd_img.c) */
static const uint8_t maximum_sectors[8][6] = {
    { 26, 31, 38, 53, 64, 118 }, /*   128 */
    { 15, 19, 23, 32, 38, 73  }, /*   256 */
    { 7,  10, 12, 17, 22, 41  }, /*   512 */
    { 3,  5,  6,  9,  11, 22  }, /*  1024 */
    { 2,  2,  3,  4,  5,  11  }, /*  2048 */
    { 1,  1,  1,  2,  2,  5   }, /*  4096 */
    { 0,  0,  0,  1,  1,  3   }, /*  8192 */
    { 0,  0,  0,  0,  0,  1   }  /* 16384 */
};

static const uint8_t rates[6] = { 2, 2, 1, 4, 0, 3 };
static const uint8_t holes[6] = { 0, 0, 0, 1, 1, 2 };

/* Calculate gap sizes based on floppy geometry */
static void
calculate_gap_sizes(drawbridge_t *dev)
{
    uint8_t sector_size_code = 2; /* 512 bytes = code 2 */
    uint8_t temp_rate        = 0xFF;

    /* Find the appropriate data rate based on sectors per track */
    for (uint8_t i = 0; i < 6; i++) {
        if (dev->sectors <= maximum_sectors[sector_size_code][i]) {
            temp_rate      = rates[i];
            dev->data_rate = temp_rate;
            /* Add hole info to existing disk_flags instead of overwriting */
            dev->disk_flags |= holes[i] << 1;
            break;
        }
    }

    if (temp_rate == 0xFF) {
        drawbridge_fdd_log("DrawBridge: Unknown floppy format, using default gap sizes\n");
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
        drawbridge_fdd_log("DrawBridge: Gap3 lookup out of bounds: rate=%d, size_code=%d, sectors=%d\n",
                           temp_rate, sector_size_code, dev->sectors);
        dev->gap3_size = 108;
    }

    if (dev->gap3_size == 0) {
        drawbridge_fdd_log("DrawBridge: Invalid gap3 size, using default\n");
        dev->gap3_size = 108;
    }

    /* Set up track flags like IMG format does */
    dev->track_flags = 0x08;           /* MFM encoding */
    dev->track_flags |= temp_rate & 3; /* Data rate (bits 0-1) */
    if (temp_rate & 4)
        dev->track_flags |= 0x20; /* RPM flag (bit 5) */

    /* Add the extra bit cells flag - required for proper DOS compatibility */
    dev->disk_flags |= 0x80;

    drawbridge_fdd_log("DrawBridge: Calculated gap sizes - gap2: %d, gap3: %d, data_rate: %d\n",
                       dev->gap2_size, dev->gap3_size, dev->data_rate);
}

/* Detect floppy disk geometry from DrawBridge */
static int
detect_floppy_geometry(drawbridge_t *dev)
{
    bool               isHD = false;
    DiagnosticResponse response;

    if (!dev || !dev->arduino) {
        drawbridge_fdd_log("DrawBridge: Invalid device in detect_floppy_geometry\n");
        return 0;
    }

    /* Check if disk is present */
    response = arduino_interface_check_for_disk(dev->arduino, true);
    if (response != DIAGNOSTIC_RESPONSE_OK) {
        drawbridge_fdd_log("DrawBridge: No disk detected\n");
        dev->disk_inserted = false;
        return 0;
    }

    dev->disk_inserted = true;

    /* Check disk capacity (HD vs DD) */
    response = arduino_interface_check_disk_capacity(dev->arduino, &isHD);
    if (response != DIAGNOSTIC_RESPONSE_OK) {
        drawbridge_fdd_log("DrawBridge: Failed to detect disk capacity, assuming DD\n");
        isHD = false;
    }

    dev->is_hd = isHD;

    /* Set the disk capacity in the Arduino to match what we detected */
    response = arduino_interface_set_disk_capacity(dev->arduino, isHD);
    if (response != DIAGNOSTIC_RESPONSE_OK) {
        drawbridge_fdd_log("DrawBridge: Failed to set disk capacity to %s: %d\n",
                           isHD ? "HD" : "DD", response);
        return 0;
    }

    if (isHD) {
        dev->tracks     = 80;
        dev->heads      = 2;
        dev->sectors    = 18;
        dev->disk_flags = 0x08; /* Double sided */
        drawbridge_fdd_log("DrawBridge: Detected HD disk (1.44MB)\n");
    } else {
        dev->tracks     = 80;
        dev->heads      = 2;
        dev->sectors    = 9;
        dev->disk_flags = 0x08; /* Double sided */
        drawbridge_fdd_log("DrawBridge: Detected DD disk (720KB)\n");
    }

    drawbridge_fdd_log("DrawBridge: Geometry set in Arduino - tracks: %d, heads: %d, sectors: %d\n",
                       dev->tracks, dev->heads, dev->sectors);

    /* Calculate appropriate gap sizes for this floppy format */
    calculate_gap_sizes(dev);

    return 1;
}

/* Read sector directly from DrawBridge device */
static void
read_sector_from_device(int drive, int track, int head, int sector, uint8_t *buffer)
{
    drawbridge_t      *dev = drawbridge_fdd[drive];
    DiagnosticResponse response;

    if (!dev || !dev->arduino) {
        drawbridge_fdd_log("DrawBridge: Invalid device for sector read\n");
        memset(buffer, 0, 512);
        return;
    }

    if (!dev->disk_inserted) {
        drawbridge_fdd_log("DrawBridge: No disk inserted for sector read\n");
        memset(buffer, 0, 512);
        return;
    }

    drawbridge_fdd_log("DrawBridge: Reading T:%d H:%d S:%d\n", track, head, sector);

    /* Seek to the desired track if not already there */
    if (dev->track != track) {
        response = arduino_interface_select_track(dev->arduino, track);
        if (response != DIAGNOSTIC_RESPONSE_OK) {
            drawbridge_fdd_log("DrawBridge: Failed to seek to cylinder %d: %d\n", track, response);
            memset(buffer, 0, 512);
            return;
        }
        dev->track = track;
        /* Invalidate track cache when changing tracks */
        dev->track_data_valid[0] = false;
        dev->track_data_valid[1] = false;
    }

    /* Select the head/side */
    response = arduino_interface_select_surface(dev->arduino, (head == 0) ? DISK_SURFACE_LOWER : DISK_SURFACE_UPPER);
    if (response != DIAGNOSTIC_RESPONSE_OK) {
        drawbridge_fdd_log("DrawBridge: Failed to select side %d: %d\n", head, response);
        memset(buffer, 0, 512);
        return;
    }

    /* Read track data if not cached or cache is invalid */
    if (!dev->track_data_valid[head] || dev->cached_track != track) {
        drawbridge_fdd_log("DrawBridge: Cache miss - reading track %d head %d (cached: track=%d valid[%d]=%s)\n",
                           track, head, dev->cached_track, head, dev->track_data_valid[head] ? "true" : "false");
        int data_length = dev->is_hd ? RAW_TRACKDATA_LENGTH_HD : RAW_TRACKDATA_LENGTH_DD;

        /* Try reading the track data, with retry and calibration if needed */
        int       retry_count = 0;
        const int max_retries = 3;
        while (retry_count < max_retries) {
            response = arduino_interface_read_current_track(dev->arduino, dev->track_data[head], data_length, true);
            if (response == DIAGNOSTIC_RESPONSE_OK) {
                break; /* Success! */
            }

            drawbridge_fdd_log("DrawBridge: Failed to read track %d side %d (attempt %d/%d): %d\n",
                               track, head, retry_count + 1, max_retries, response);

            if (retry_count < max_retries - 1) {
                /* Perform calibration seek (like the reference code) */
                uint32_t calibration_track = (track < 40) ? track + 30 : track - 30;
                drawbridge_fdd_log("DrawBridge: Performing calibration seek to cylinder %d\n", calibration_track);

                arduino_interface_select_track(dev->arduino, calibration_track);
                arduino_interface_select_track(dev->arduino, track); /* Seek back */
            }
            retry_count++;
        }

        if (response != DIAGNOSTIC_RESPONSE_OK) {
            drawbridge_fdd_log("DrawBridge: Failed to read track %d side %d: %d\n", track, head, response);
            memset(buffer, 0, 512);
            return;
        }

        /* Mark cache as valid for this side */
        dev->track_data_valid[head] = true;
        dev->cached_track           = track;

        drawbridge_fdd_log("DrawBridge: Successfully cached track %d head %d data\n", track, head);

    } else {
        drawbridge_fdd_log("DrawBridge: Using cached track %d head %d data\n", dev->cached_track, head);
    }

    /* Use IBM MFM decoder to extract the sector */
    drawbridge_fdd_log("DrawBridge: Using IBM MFM decoder to extract sector %d\n", sector);

    /* Create track structure for decoding */
    DecodedTrack decoded_track;
    decoded_track.sectors           = NULL;
    decoded_track.sectorsWithErrors = 0;
    /* Calculate track number for decoder (combines cylinder and head) */
    bool nonstandard_timings = false;

    /* Decode the entire track using IBM MFM decoder */
    findSectors_IBM(dev->track_data[head],
                    (dev->is_hd ? RAW_TRACKDATA_LENGTH_HD : RAW_TRACKDATA_LENGTH_DD) * 8, /* convert bytes to bits */
                    dev->is_hd,
                    track, /* cylinder */
                    head,  /* head */
                    dev->sectors,
                    &decoded_track,
                    &nonstandard_timings);

    /* Look for the requested sector in the decoded track */
    SectorMapEntry *sector_entry = find_sector_entry(&decoded_track, sector - 1); /* sectors are 0-based in decoder */

    if (sector_entry && sector_entry->sector.numErrors == 0) {
        /* Found perfect sector data */
        if (sector_entry->sector.data.size >= 512) {
            memcpy(buffer, sector_entry->sector.data.data, 512);
            drawbridge_fdd_log("DrawBridge: Successfully extracted REAL sector data T%d H%d S%d (0 errors)\n",
                               track, head, sector);
        } else {
            /* Sector size mismatch */
            memcpy(buffer, sector_entry->sector.data.data, sector_entry->sector.data.size);
            memset(buffer + sector_entry->sector.data.size, 0, 512 - sector_entry->sector.data.size);
            drawbridge_fdd_log("DrawBridge: Extracted partial sector data T%d H%d S%d (%d bytes, 0 errors)\n",
                               track, head, sector, sector_entry->sector.data.size);
        }
    } else if (sector_entry && sector_entry->sector.numErrors < 0xFF) {
        /* Found sector data but with errors */
        if (sector_entry->sector.data.size >= 512) {
            memcpy(buffer, sector_entry->sector.data.data, 512);
            drawbridge_fdd_log("DrawBridge: Extracted sector data with errors T%d H%d S%d (%d errors)\n",
                               track, head, sector, sector_entry->sector.numErrors);
        } else {
            memcpy(buffer, sector_entry->sector.data.data, sector_entry->sector.data.size);
            memset(buffer + sector_entry->sector.data.size, 0, 512 - sector_entry->sector.data.size);
            drawbridge_fdd_log("DrawBridge: Extracted partial sector data with errors T%d H%d S%d (%d bytes, %d errors)\n",
                               track, head, sector, sector_entry->sector.data.size, sector_entry->sector.numErrors);
        }
    } else {
        /* Sector not found or too many errors, use dummy data */
        drawbridge_fdd_log("DrawBridge: Sector %d not found or too many errors, using dummy data for T%d H%d S%d\n",
                           sector, track, head, sector);
        memset(buffer, 0xAA, 512); /* Pattern for testing */

        /* Mark the first few bytes with sector info for debugging */
        buffer[0] = track;
        buffer[1] = head;
        buffer[2] = sector;
        buffer[3] = 0x02; /* Sector size code for 512 bytes */
    }

    /* Clean up decoded track */
    free_decoded_track(&decoded_track);

    drawbridge_fdd_log("DrawBridge: Successfully read sector T%d H%d S%d\n", track, head, sector);
}

/* Seek to track */
static void
drawbridge_fdd_seek(int drive, int track)
{
    drawbridge_t *dev = drawbridge_fdd[drive];

    if (!dev) {
        drawbridge_fdd_log("DrawBridge: Seek called on uninitialized drive %d\n", drive);
        return;
    }

    drawbridge_fdd_log("DrawBridge: Seeking drive %d to track %d\n", drive, track);

    /* Check if track is within bounds */
    if (track >= dev->tracks) {
        drawbridge_fdd_log("DrawBridge: Track %d out of bounds (max: %d)\n", track, dev->tracks - 1);
        return;
    }

    /* For DrawBridge, we use the d86f engine for track management */
    d86f_set_cur_track(drive, track);
    d86f_reset_index_hole_pos(drive, 0);
    d86f_reset_index_hole_pos(drive, 1);
    d86f_destroy_linked_lists(drive, 0);
    d86f_destroy_linked_lists(drive, 1);
    d86f_zero_track(drive);

    if (dev->track != track){
        arduino_interface_enable_reading(dev->arduino, true, false, false);
    }

    /* Build track data for both sides */
    for (int side = 0; side < dev->heads; side++) {
        int current_pos = d86f_prepare_pretrack(drive, side, 0);

        drawbridge_fdd_log("DrawBridge: Building track %d side %d\n", track, side);

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

    arduino_interface_enable_reading(dev->arduino, false, false, false);
    drawbridge_fdd_log("DrawBridge: Completed seek to track %d\n", track);
}

/* Get disk flags */
static uint16_t
drawbridge_fdd_disk_flags(int drive)
{
    const drawbridge_t *dev = drawbridge_fdd[drive];
    if (!dev) {
        drawbridge_fdd_log("DrawBridge: disk_flags requested for invalid drive %d\n", drive);
        return 0;
    }

    /* Reduce logging noise - only log first few requests */
    static int log_count = 0;
    if (log_count < 5) {
        drawbridge_fdd_log("DrawBridge: disk_flags requested for drive %d: 0x%04X\n", drive, dev->disk_flags);
        log_count++;
    }
    return dev->disk_flags;
}

/* Get side flags */
static uint16_t
drawbridge_fdd_side_flags(int drive)
{
    const drawbridge_t *dev             = drawbridge_fdd[drive];
    uint16_t            temp_side_flags = 0;

    if (!dev) {
        drawbridge_fdd_log("DrawBridge: side_flags requested for invalid drive %d\n", drive);
        return 0;
    }

    /* Reduce logging noise - only log first few requests */
    static int      side_flags_log_count = 0;
    static int      last_logged_drive    = -1;
    static uint16_t last_logged_flags    = 0;

    /* Calculate side flags based on actual data rate from geometry */
    switch (dev->data_rate) {
        case 0: /* 500 kbps (HD) */
            temp_side_flags = 0;
            break;
        case 1: /* 300 kbps */
            temp_side_flags = 1;
            break;
        case 2: /* 250 kbps (DD) */
            temp_side_flags = 2;
            break;
        case 3: /* 1000 kbps (ED) */
            temp_side_flags = 3;
            break;
        case 4: /* Special rate - treat as DD */
            temp_side_flags = 2;
            break;
        default:
            temp_side_flags = 2; /* Default to DD */
            break;
    }

    /* Always set MFM encoding flag */
    temp_side_flags |= 0x08;

    /* Only log first few requests or when flags change */
    if (side_flags_log_count < 5 || drive != last_logged_drive || temp_side_flags != last_logged_flags) {
        drawbridge_fdd_log("DrawBridge: side_flags for drive %d: data_rate=%d, flags=0x%04X\n",
                           drive, dev->data_rate, temp_side_flags);
        side_flags_log_count++;
        last_logged_drive = drive;
        last_logged_flags = temp_side_flags;
    }

    return temp_side_flags;
}

/* Set sector for reading/writing */
static void
drawbridge_fdd_set_sector(int drive, int side, uint8_t c, uint8_t h, uint8_t r, uint8_t n)
{
    drawbridge_fdd_log("DrawBridge: set_sector drive=%d, side=%d, C=%d H=%d R=%d N=%d\n", drive, side, c, h, r, n);
    drawbridge_t *dev = drawbridge_fdd[drive];

    if (!dev)
        return;

    /* Validate sector parameters */
    if (c >= dev->tracks || h >= dev->heads || r < 1 || r > dev->sectors)
        return;

    /* Check if we need to read a new sector */
    if (dev->current_sector_track != c || dev->current_sector_head != h || dev->current_sector_r != r || !dev->current_sector_valid) {

        /* Read the new sector data */
        read_sector_from_device(drive, c, h, r, dev->current_sector_data);

        /* Update current sector tracking */
        dev->current_sector_track = c;
        dev->current_sector_head  = h;
        dev->current_sector_r     = r;
        dev->current_sector_valid = 1;

        drawbridge_fdd_log("DrawBridge: Set sector drive=%d, C=%d H=%d R=%d N=%d\n", drive, c, h, r, n);
    }
}

/* Read data callback */
static uint8_t
drawbridge_fdd_poll_read_data(int drive, int side, uint16_t pos)
{
    drawbridge_fdd_log("DrawBridge: read_data drive=%d, side=%d, pos=%d\n", drive, side, pos);
    drawbridge_t *dev = drawbridge_fdd[drive];

    if (!dev || !dev->current_sector_valid || pos >= 512) {
        return 0x00;
    }

    return dev->current_sector_data[pos];
}

/* Write data callback */
static void
drawbridge_fdd_poll_write_data(int drive, int side, uint16_t pos, uint8_t data)
{
    /* DrawBridge is read-only */
    if (writeprot[drive])
        return;
}

/* Writeback function - ensure any pending writes are synced */
static void
drawbridge_fdd_writeback(int drive)
{
    drawbridge_t *dev = drawbridge_fdd[drive];

    if (!dev || writeprot[drive])
        return;

    /* DrawBridge is read-only, no writeback needed */
}

/* Format conditions check */
static int
drawbridge_fdd_format_conditions(int drive)
{
    /* DrawBridge is read-only */
    return 0;
}

/* Initialize DrawBridge floppy support */
void
drawbridge_init(void)
{
    memset(drawbridge_fdd, 0x00, sizeof(drawbridge_fdd));
}

/* Load DrawBridge floppy device */
void
drawbridge_load(int drive, char *fn)
{
    drawbridge_t      *dev;
    DiagnosticResponse response;

    drawbridge_fdd_log("DrawBridge: Loading DrawBridge device %d from '%s'\n", drive, fn);

    d86f_unregister(drive);
    writeprot[drive] = 1; /* DrawBridge is read-only */

    /* Allocate device structure */
    dev = (drawbridge_t *) calloc(1, sizeof(drawbridge_t));
    if (!dev) {
        drawbridge_fdd_log("DrawBridge: Failed to allocate device structure\n");
        return;
    }

    /* Store device path */
    dev->device_path = strdup(fn);

    /* Create Arduino interface */
    dev->arduino = arduino_interface_create();
    if (!dev->arduino) {
        drawbridge_fdd_log("DrawBridge: Failed to create Arduino interface\n");
        free(dev->device_path);
        free(dev);
        return;
    }

    /* Open the Arduino port */
    response = arduino_interface_open_port(dev->arduino, fn, true);
    if (response != DIAGNOSTIC_RESPONSE_OK) {
        drawbridge_fdd_log("DrawBridge: Failed to open port %s: %d\n", fn, response);
        drawbridge_fdd_log("DrawBridge: Error: %s\n", arduino_interface_get_last_error_str(dev->arduino));
        arduino_interface_destroy(dev->arduino);
        free(dev->device_path);
        free(dev);
        return;
    }

    drawbridge_fdd_log("DrawBridge: Successfully opened Arduino port %s\n", fn);

    /* Enable reading mode */
    response = arduino_interface_enable_reading(dev->arduino, true, true, false);
    if (response != DIAGNOSTIC_RESPONSE_OK) {
        drawbridge_fdd_log("DrawBridge: Failed to enable reading: %d\n", response);
        arduino_interface_close_port(dev->arduino);
        arduino_interface_destroy(dev->arduino);
        free(dev->device_path);
        free(dev);
        return;
    }

    /* Initialize device state */
    dev->disk_inserted        = false;
    dev->is_hd                = false;
    dev->track_data_valid[0]  = false;
    dev->track_data_valid[1]  = false;
    dev->current_sector_valid = 0;
    dev->current_sector_track = 0xFF; /* Invalid track to force initial read */
    dev->current_sector_head  = 0xFF;
    dev->current_sector_r     = 0xFF;
    dev->cached_track         = -1;
    dev->track                = -1;

    /* Detect floppy geometry from Arduino */
    if (!detect_floppy_geometry(dev)) {
        drawbridge_fdd_log("DrawBridge: Failed to detect floppy geometry\n");
        arduino_interface_close_port(dev->arduino);
        arduino_interface_destroy(dev->arduino);
        free(dev->device_path);
        free(dev);
        return;
    }

    if (ui_writeprot[drive])
        writeprot[drive] = 1;
    fwriteprot[drive] = writeprot[drive];

    /* Set up the device */
    drawbridge_fdd[drive] = dev;

    /* Set up D86F engine for this drive - this creates the d86f structure and CRC table */
    d86f_setup(drive);
    drawbridge_fdd_log("DrawBridge: Initialized d86f engine for drive %d\n", drive);

    /* Attach to D86F engine */
    d86f_handler[drive].disk_flags        = drawbridge_fdd_disk_flags;
    d86f_handler[drive].side_flags        = drawbridge_fdd_side_flags;
    d86f_handler[drive].writeback         = drawbridge_fdd_writeback;
    d86f_handler[drive].set_sector        = drawbridge_fdd_set_sector;
    d86f_handler[drive].read_data         = drawbridge_fdd_poll_read_data;
    d86f_handler[drive].write_data        = drawbridge_fdd_poll_write_data;
    d86f_handler[drive].format_conditions = drawbridge_fdd_format_conditions;
    d86f_handler[drive].extra_bit_cells   = null_extra_bit_cells;
    d86f_handler[drive].encoded_data      = common_encoded_data;
    d86f_handler[drive].read_revolution   = common_read_revolution;
    d86f_handler[drive].index_hole_pos    = null_index_hole_pos;
    d86f_handler[drive].get_raw_size      = common_get_raw_size;
    d86f_handler[drive].check_crc         = 1; /* Enable CRC checking to match other formats */
    
    d86f_set_version(drive, 0x0063);
    fdd_set_turbo(drive, 1); /* Enable turbo mode for DrawBridge */

    drives[drive].seek = drawbridge_fdd_seek;

    d86f_common_handlers(drive);

    drawbridge_fdd_log("DrawBridge: Successfully loaded DrawBridge device\n");
}

/* Close DrawBridge floppy device */
void
drawbridge_close(int drive)
{
    drawbridge_t *dev = drawbridge_fdd[drive];

    if (!dev)
        return;

    drawbridge_fdd_log("DrawBridge: Closing DrawBridge device %d\n", drive);

    /* Clean up d86f engine and unregister handlers */
    d86f_destroy(drive);

    /* Write back any changes before closing (none for read-only device) */
    drawbridge_fdd_writeback(drive);

    /* Close Arduino interface */
    if (dev->arduino) {
        if (arduino_interface_is_open(dev->arduino)) {
            arduino_interface_enable_reading(dev->arduino, false, false, false);
            arduino_interface_close_port(dev->arduino);
        }
        arduino_interface_destroy(dev->arduino);
    }

    if (dev->device_path)
        free(dev->device_path);

    free(dev);
    drawbridge_fdd[drive] = NULL;
}

/* Set FDC reference */
void
drawbridge_set_fdc(void *fdc)
{
    drawbridge_fdd_fdc = (fdc_t *) fdc;
}
