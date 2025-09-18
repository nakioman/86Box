#ifndef FDD_DRAWBRIDGE_H
#define FDD_DRAWBRIDGE_H

#include <stdbool.h>

#define IBM_DD_SECTORS 9
#define IBM_HD_SECTORS 18

// IAM A1A1A1FC
#define MFM_SYNC_TRACK_HEADER 0x5224522452245552ULL
// IDAM A1A1A1FE
#define MFM_SYNC_SECTOR_HEADER 0x4489448944895554ULL
// DAM A1A1A1FB (data address mark)
#define MFM_SYNC_SECTOR_DATA 0x4489448944895545ULL
// DDAM A1A1A1F8 (deleted data address mark)
#define MFM_SYNC_DELETED_SECTOR_DATA 0x448944894489554AULL

// Helper functions for RawDecodedSector management
typedef struct {
    uint8_t *data;
    uint32_t size;
    uint32_t capacity;
} RawDecodedSector;

typedef struct {
    RawDecodedSector data;
    uint32_t         numErrors; // number of errors in this sector. 0 means PERFECT!
} DecodedSector;

typedef struct SectorMapEntry {
    int                    sectorNumber;
    DecodedSector          sector;
    struct SectorMapEntry *next;
} SectorMapEntry;

typedef struct {
    SectorMapEntry *sectors; // linked list of sectors
    uint32_t        sectorsWithErrors;
} DecodedTrack;

// IAM Header data structure
typedef struct __attribute__((packed)) {
    unsigned char addressMark[4]; // should be 0xA1A1A1FE
    unsigned char cylinder;
    unsigned char head;
    unsigned char sector;
    unsigned char length; // 2^(length+7) sector size = should be 2 for 512
    unsigned char crc[2];
} IBMSectorHeader;

// IDAM data
typedef struct {
    unsigned char  dataMark[4]; // should be 0xA1A1A1FB
    unsigned char *data;        // *should* be 512 but doesn't have to be
    uint32_t       dataSize;    // size of the data array
    unsigned char  crc[2];
} IBMSectorData;

typedef struct {
    IBMSectorHeader header;
    IBMSectorData   data;
    uint32_t        headerErrors; // number of errors in the header. 0 means PERFECT!
    bool            dataValid;
} IBMSector;


void drawbridge_load(int drive, char *fn);
void drawbridge_close(int drive);
void drawbridge_init(void);

#endif // FDD_DRAWBRIDGE_H
