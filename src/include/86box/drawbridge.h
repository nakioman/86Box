#ifndef ARDUINO_FLOPPY_READER_WRITER_INTERFACE_C
#define ARDUINO_FLOPPY_READER_WRITER_INTERFACE_C
/* ArduinoFloppyReaderWriter aka DrawBridge - C Language Version
*
* Copyright (C) 2021-2024 Robert Smith (@RobSmithDev)
* https://amiga.robsmithdev.co.uk
*
* This file is multi-licensed under the terms of the Mozilla Public
* License Version 2.0 as published by Mozilla Corporation and the
* GNU General Public License, version 2 or later, as published by the
* Free Software Foundation.
*
* MPL2: https://www.mozilla.org/en-US/MPL/2.0/
* GPL2: https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html
*
* This file, along with currently active and supported interfaces
* are maintained from by GitHub repo at
* https://github.com/RobSmithDev/FloppyDriveBridge
*/

////////////////////////////////////////////////////////////////////////////////////////
// C Language Interface to manage the communication between computer and Arduino    //
////////////////////////////////////////////////////////////////////////////////////////

#include <stdbool.h>
#include <stdint.h>

#define NUM_SECTORS_PER_TRACK_DD 11						 // Number of sectors per track
#define NUM_SECTORS_PER_TRACK_HD 22						  // Same but for HD disks

// Paula on the Amiga used to find the SYNC then read 1900 WORDS. (12868 bytes)
// As the PC is doing the SYNC we need to read more than this to allow a further overlap
// This number must match what the sketch in the Arduino is set to. 
#define RAW_TRACKDATA_LENGTH_DD (0x1900*2+0x440)
#define RAW_TRACKDATA_LENGTH_HD (2*RAW_TRACKDATA_LENGTH_DD)

#define FLAGS_HIGH_PRECISION_SUPPORT   (1 << 0)
#define FLAGS_DISKCHANGE_SUPPORT	   (1 << 1)
#define FLAGS_DRAWBRIDGE_PLUSMODE	   (1 << 2)
#define FLAGS_DENSITYDETECT_ENABLED    (1 << 3)
#define FLAGS_SLOWSEEKING_MODE		   (1 << 4)
#define FLAGS_INDEX_ALIGN_MODE		   (1 << 5)
#define FLAGS_FLUX_READ				   (1 << 6)
#define FLAGS_FIRMWARE_BETA            (1 << 7)

// Forward declarations
typedef struct ArduinoInterface ArduinoInterface;
typedef struct SerialIO SerialIO;
typedef struct MFMExtractionTarget MFMExtractionTarget;
typedef struct BridgePLL BridgePLL;
typedef struct RotationExtractor RotationExtractor;

// Array to hold data from a floppy disk read
typedef unsigned char RawTrackDataDD[RAW_TRACKDATA_LENGTH_DD];
typedef unsigned char RawTrackDataHD[RAW_TRACKDATA_LENGTH_HD];

// Sketch firmware version
typedef struct {
    unsigned char major;
    unsigned char minor;
    bool fullControlMod;
    
    // Extra in V1.9
    unsigned char deviceFlags1;
    unsigned char deviceFlags2;
    unsigned char buildNumber;
} FirmwareVersion;

// Represent which side of the disk we're looking at
typedef enum {
    DISK_SURFACE_UPPER = 0,     // The upper side of the disk
    DISK_SURFACE_LOWER = 1      // The lower side of the disk
} DiskSurface;

// Diagnostic responses from the interface
typedef enum {
    DIAGNOSTIC_RESPONSE_OK = 0,

    // Responses from openPort
    DIAGNOSTIC_RESPONSE_PORT_IN_USE,
    DIAGNOSTIC_RESPONSE_PORT_NOT_FOUND,
    DIAGNOSTIC_RESPONSE_PORT_ERROR,
    DIAGNOSTIC_RESPONSE_ACCESS_DENIED,
    DIAGNOSTIC_RESPONSE_COMPORT_CONFIG_ERROR,
    DIAGNOSTIC_RESPONSE_BAUD_RATE_NOT_SUPPORTED,
    DIAGNOSTIC_RESPONSE_ERROR_READING_VERSION,
    DIAGNOSTIC_RESPONSE_ERROR_MALFORMED_VERSION,
    DIAGNOSTIC_RESPONSE_OLD_FIRMWARE,

    // Responses from commands
    DIAGNOSTIC_RESPONSE_SEND_FAILED,
    DIAGNOSTIC_RESPONSE_SEND_PARAMETER_FAILED,
    DIAGNOSTIC_RESPONSE_READ_RESPONSE_FAILED,
    DIAGNOSTIC_RESPONSE_WRITE_TIMEOUT,
    DIAGNOSTIC_RESPONSE_SERIAL_OVERRUN,
    DIAGNOSTIC_RESPONSE_FRAMING_ERROR,
    DIAGNOSTIC_RESPONSE_ERROR,

    // Response from selectTrack
    DIAGNOSTIC_RESPONSE_TRACK_RANGE_ERROR,
    DIAGNOSTIC_RESPONSE_SELECT_TRACK_ERROR,
    DIAGNOSTIC_RESPONSE_WRITE_PROTECTED,
    DIAGNOSTIC_RESPONSE_STATUS_ERROR,
    DIAGNOSTIC_RESPONSE_SEND_DATA_FAILED,
    DIAGNOSTIC_RESPONSE_TRACK_WRITE_RESPONSE_ERROR,

    // Returned if there is no disk in the drive
    DIAGNOSTIC_RESPONSE_NO_DISK_IN_DRIVE,

    DIAGNOSTIC_RESPONSE_DIAGNOSTIC_NOT_AVAILABLE,
    DIAGNOSTIC_RESPONSE_USB_SERIAL_BAD,
    DIAGNOSTIC_RESPONSE_CTS_FAILURE,
    DIAGNOSTIC_RESPONSE_REWIND_FAILURE,

    DIAGNOSTIC_RESPONSE_MEDIA_TYPE_MISMATCH
} DiagnosticResponse;

typedef enum {
    LAST_COMMAND_OPEN_PORT = 0,
    LAST_COMMAND_GET_VERSION,
    LAST_COMMAND_ENABLE_WRITE,
    LAST_COMMAND_REWIND,
    LAST_COMMAND_DISABLE_MOTOR,
    LAST_COMMAND_ENABLE_MOTOR,
    LAST_COMMAND_GOTO_TRACK,
    LAST_COMMAND_SELECT_SURFACE,
    LAST_COMMAND_READ_TRACK,
    LAST_COMMAND_WRITE_TRACK,
    LAST_COMMAND_RUN_DIAGNOSTICS,
    LAST_COMMAND_SWITCH_DISK_MODE,
    LAST_COMMAND_READ_TRACK_STREAM,
    LAST_COMMAND_CHECK_DISK_IN_DRIVE,
    LAST_COMMAND_CHECK_DISK_WRITE_PROTECTED,
    LAST_COMMAND_ERASE_TRACK,
    LAST_COMMAND_NO_CLICK_CHECK,
    LAST_COMMAND_CHECK_DENSITY,
    LAST_COMMAND_MEASURE_RPM,
    LAST_COMMAND_EEPROM_READ,
    LAST_COMMAND_EEPROM_WRITE,
    LAST_COMMAND_WRITE_FLUX,
    LAST_COMMAND_ERASE_FLUX
} LastCommand;

// Precompensation modes
#define PRECOMP_NONE 0x00
#define PRECOMP_ERLY 0x04   
#define PRECOMP_LATE 0x08   


// MFM Sample structure (placeholder - define according to your needs)
typedef struct {
    // Define this according to your RotationExtractor implementation
    uint32_t data;
    uint32_t timing;
} MFMSample;

// Index sequence marker structure (placeholder)
typedef struct {
    // Define this according to your RotationExtractor implementation
    uint32_t pattern;
    uint32_t position;
} IndexSequenceMarker;

// Callback function types
typedef bool (*OnRotationCallback)(MFMSample** mfmData, const unsigned int dataLengthInBits, void* userData);

// String buffer for port names and error messages
#define MAX_STRING_LENGTH 256
#define MAX_PORTS 32

typedef struct {
    char data[MAX_STRING_LENGTH];
} StringBuffer;

typedef struct {
    StringBuffer ports[MAX_PORTS];
    int count;
} PortList;

// Arduino Interface structure (opaque - implementation in .c file)
struct ArduinoInterface {
    // Private implementation details should be in the .c file
    void* private_data;
};

// Function prototypes

// Constructor/Destructor
ArduinoInterface* arduino_interface_create(void);
void arduino_interface_destroy(ArduinoInterface* interface);

// Status functions
LastCommand arduino_interface_get_last_failed_command(const ArduinoInterface* interface);
DiagnosticResponse arduino_interface_get_last_error(const ArduinoInterface* interface);
const char* arduino_interface_get_last_error_str(const ArduinoInterface* interface);
bool arduino_interface_is_open(const ArduinoInterface* interface);
bool arduino_interface_is_in_write_mode(const ArduinoInterface* interface);
bool arduino_interface_is_disk_in_drive(const ArduinoInterface* interface);
FirmwareVersion arduino_interface_get_firmware_version(const ArduinoInterface* interface);

// Port management
void arduino_interface_enumerate_ports(PortList* portList);
bool arduino_interface_is_port_correct(const char* portName);
DiagnosticResponse arduino_interface_open_port(ArduinoInterface* interface, const char* portName, bool enableCTSflowcontrol);
void arduino_interface_close_port(ArduinoInterface* interface);

// Basic operations
DiagnosticResponse arduino_interface_enable_reading(ArduinoInterface* interface, bool enable, bool reset, bool dontWait);
DiagnosticResponse arduino_interface_enable_writing(ArduinoInterface* interface, bool enable, bool reset);
DiagnosticResponse arduino_interface_check_if_disk_is_write_protected(ArduinoInterface* interface, bool forceCheck);
DiagnosticResponse arduino_interface_find_track0(ArduinoInterface* interface);
DiagnosticResponse arduino_interface_check_for_disk(ArduinoInterface* interface, bool forceCheck);

// Track operations
DiagnosticResponse arduino_interface_select_track(ArduinoInterface* interface, unsigned char trackIndex);
DiagnosticResponse arduino_interface_perform_no_click_seek(ArduinoInterface* interface);
DiagnosticResponse arduino_interface_select_surface(ArduinoInterface* interface, DiskSurface side);

// Reading operations
DiagnosticResponse arduino_interface_read_rotation(ArduinoInterface* interface, 
    MFMExtractionTarget* extractor, unsigned int maxOutputSize, 
    MFMSample* firstOutputBuffer, IndexSequenceMarker* startBitPatterns, 
    OnRotationCallback onRotation, void* userData, bool useHalfPLL);

DiagnosticResponse arduino_interface_read_flux(ArduinoInterface* interface, 
    BridgePLL* pll, unsigned int maxOutputSize, 
    MFMSample* firstOutputBuffer, IndexSequenceMarker* startBitPatterns, 
    OnRotationCallback onRotation, void* userData);

DiagnosticResponse arduino_interface_read_data(ArduinoInterface* interface, BridgePLL* pll);
DiagnosticResponse arduino_interface_read_current_track(ArduinoInterface* interface, void* trackData, int dataLength, bool readFromIndexPulse);
DiagnosticResponse arduino_interface_read_current_track_dd(ArduinoInterface* interface, RawTrackDataDD trackData, bool readFromIndexPulse);

// Writing operations
DiagnosticResponse arduino_interface_write_current_track(ArduinoInterface* interface, const unsigned char* data, unsigned short numBytes, bool writeFromIndexPulse);
DiagnosticResponse arduino_interface_write_current_track_precomp(ArduinoInterface* interface, const unsigned char* mfmData, unsigned short numBytes, bool writeFromIndexPulse, bool usePrecomp);
DiagnosticResponse arduino_interface_write_flux(ArduinoInterface* interface, const uint32_t* fluxTimes, int fluxCount, uint32_t offsetFromIndex, float driveRPM, bool compensateFluxTimings, bool terminateAtIndex);
DiagnosticResponse arduino_interface_erase_flux_on_track(ArduinoInterface* interface);
DiagnosticResponse arduino_interface_erase_current_track(ArduinoInterface* interface);

// Diagnostic operations
DiagnosticResponse arduino_interface_get_reset_reason(ArduinoInterface* interface, bool* WD, bool* BOD, bool* ExtReset, bool* PowerOn);
DiagnosticResponse arduino_interface_clear_reset_reason(ArduinoInterface* interface);
bool arduino_interface_abort_read_streaming(ArduinoInterface* interface);
bool arduino_interface_is_streaming(ArduinoInterface* interface);
DiagnosticResponse arduino_interface_test_index_pulse(ArduinoInterface* interface);
DiagnosticResponse arduino_interface_test_data_pulse(ArduinoInterface* interface);
DiagnosticResponse arduino_interface_measure_drive_rpm(ArduinoInterface* interface, float* rpm);
DiagnosticResponse arduino_interface_check_disk_capacity(ArduinoInterface* interface, bool* isHD);
DiagnosticResponse arduino_interface_set_disk_capacity(ArduinoInterface* interface, bool switchToHD_Disk);
DiagnosticResponse arduino_interface_guess_plus_mode(ArduinoInterface* interface, bool* isProbablyPlus);
DiagnosticResponse arduino_interface_test_cts(ArduinoInterface* interface);
DiagnosticResponse arduino_interface_test_transfer_speed(ArduinoInterface* interface);

// Utility functions
bool arduino_interface_track_contains_data(const ArduinoInterface* interface, const RawTrackDataDD trackData);
void unpack(const unsigned char* data, unsigned char* output, const int maxLength);
void writeBit(unsigned char* output, int* pos, int* bit, int value, const int maxLength);
int readBit(const unsigned char* buffer, const unsigned int maxLength, int* pos, int* bit);

// EEPROM operations
DiagnosticResponse arduino_interface_eeprom_is_advanced_controller(ArduinoInterface* interface, bool* enabled);
DiagnosticResponse arduino_interface_eeprom_is_drawbridge_plus_mode(ArduinoInterface* interface, bool* enabled);
DiagnosticResponse arduino_interface_eeprom_is_density_detect_disabled(ArduinoInterface* interface, bool* enabled);
DiagnosticResponse arduino_interface_eeprom_is_slow_seek_mode(ArduinoInterface* interface, bool* enabled);
DiagnosticResponse arduino_interface_eeprom_is_index_align_mode(ArduinoInterface* interface, bool* enabled);

DiagnosticResponse arduino_interface_eeprom_set_advanced_controller(ArduinoInterface* interface, bool enabled);
DiagnosticResponse arduino_interface_eeprom_set_drawbridge_plus_mode(ArduinoInterface* interface, bool enabled);
DiagnosticResponse arduino_interface_eeprom_set_density_detect_disabled(ArduinoInterface* interface, bool enabled);
DiagnosticResponse arduino_interface_eeprom_set_slow_seek_mode(ArduinoInterface* interface, bool enabled);
DiagnosticResponse arduino_interface_eeprom_set_index_align_mode(ArduinoInterface* interface, bool enabled);

#endif /* ARDUINO_FLOPPY_READER_WRITER_INTERFACE_C */