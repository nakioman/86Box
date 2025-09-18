// For usleep function
#define _DEFAULT_SOURCE
#define ENABLE_DRAWBRIDGE_LOG 1
#define HAVE_STDARG_H

#include <86box/drawbridge.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <linux/serial.h>
#include <86box/86box.h>

// Command that the ARDUINO Sketch understands
#define COMMAND_VERSION '?'
#define COMMAND_REWIND '.'
#define COMMAND_GOTOTRACK '#'
#define COMMAND_HEAD0 '['
#define COMMAND_HEAD1 ']'
#define COMMAND_READTRACK '<'
#define COMMAND_ENABLE '+'
#define COMMAND_DISABLE '-'
#define COMMAND_WRITETRACK '>'
#define COMMAND_ENABLEWRITE '~'
#define COMMAND_DIAGNOSTICS '&'
#define COMMAND_ERASETRACK 'X'
#define COMMAND_SWITCHTO_DD 'D' // Requires Firmware V1.6
#define COMMAND_SWITCHTO_HD 'H' // Requires Firmware V1.6

// New commands for more direct control of the drive.  Some of these are more efficient or don't turn the disk motor on for modded hardware
#define COMMAND_READTRACKSTREAM '{'   // Requires Firmware V1.8
#define COMMAND_WRITETRACKPRECOMP '}' // Requires Firmware V1.8
#define COMMAND_CHECKDISKEXISTS '^'   // Requires Firmware V1.8 (and modded hardware for fast version)
#define COMMAND_ISWRITEPROTECTED '$'  // Requires Firmware V1.8
#define COMMAND_ENABLE_NOWAIT '*'     // Requires Firmware V1.8
#define COMMAND_GOTOTRACK_REPORT '='  // Requires Firmware V1.8
#define COMMAND_DO_NOCLICK_SEEK 'O'   // Requires Firmware V1.8a

#define COMMAND_CHECK_DENSITY 'T'                 // Requires Firmware V1.9
#define COMMAND_TEST_RPM 'P'                      // Requires Firmware V1.9
#define COMMAND_CHECK_FEATURES '@'                // Requires Firmware V1.9
#define COMMAND_READTRACKSTREAM_HIGHPRECISION 'F' // Requires Firmware V1.9
#define COMMAND_READTRACKSTREAM_FLUX 'L'          // Requires Firmware V1.9.22
#define COMMAND_READTRACKSTREAM_HALFPLL 'l'       // Requires Firmware V1.9.22
#define COMMAND_EEPROM_READ 'E'                   // Read a value from the eeprom
#define COMMAND_EEPROM_WRITE 'e'                  // Write a value to the eeprom
#define COMMAND_RESET 'R'                         // Reset
#define COMMAND_WRITEFLUX 'Y'                     // Requires Firmware V1.9.22
#define COMMAND_ERASEFLUX 'w'                     // Requires Firmware V1.9.18

#define SPECIAL_ABORT_CHAR 'x'

// Serial IO response codes
typedef enum
{
    SERIAL_RESPONSE_OK = 0,
    SERIAL_RESPONSE_ERROR,
    SERIAL_RESPONSE_NOT_FOUND,
    SERIAL_RESPONSE_IN_USE,
    SERIAL_RESPONSE_ACCESS_DENIED
} SerialResponse;

// Serial IO configuration
typedef struct
{
    int baudRate;
    bool ctsFlowControl;
} SerialConfiguration;

// Serial IO structure for Linux
struct SerialIO
{
    int fd; // File descriptor for the serial port
    bool isOpen;
    struct termios oldTermios; // Original terminal settings
    char portName[256];
    unsigned int readTimeout;            // in milliseconds
    unsigned int readTimeoutMultiplier;  // multiplier for read timeout
    unsigned int writeTimeout;           // in milliseconds
    unsigned int writeTimeoutMultiplier; // multiplier for write timeout
};

// ArduinoInterface private data structure
typedef struct
{
    struct SerialIO comPort;
    FirmwareVersion version;
    bool inWriteMode;
    LastCommand lastCommand;
    DiagnosticResponse lastError;
    bool abortStreaming;
    bool isWriteProtected;
    bool diskInDrive;
    bool abortSignalled;
    bool isStreaming;
    bool isHDMode;
} ArduinoInterfacePrivate;

#ifdef ENABLE_DRAWBRIDGE_LOG
int arduino_do_log = ENABLE_DRAWBRIDGE_LOG;

static void
arduino_log(const char *fmt, ...)
{
    va_list ap;

    if (arduino_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
#else
#    define arduino_log(fmt, ...)
#endif

// Helper function prototypes
static SerialResponse serial_open_port(struct SerialIO *port, const char *portName);
static SerialResponse serial_configure_port(struct SerialIO *port, const SerialConfiguration *config);
static SerialResponse serial_close_port(struct SerialIO *port);
static int serial_write(struct SerialIO *port, const void *data, unsigned int size);
static int serial_read(struct SerialIO *port, void *data, unsigned int size);
static void serial_set_read_timeout(struct SerialIO *port, int waitTimetimeout, int multiplier);
static void serial_set_write_timeout(struct SerialIO *port, int waitTimetimeout, int multiplier);
static void serial_set_dtr(struct SerialIO *port, bool state);
static void serial_set_rts(struct SerialIO *port, bool state);
static void serial_purge_buffers(struct SerialIO *port);
static int serial_get_bytes_waiting(struct SerialIO *port);
static bool serial_get_cts_status(struct SerialIO *port);

static DiagnosticResponse attempt_to_sync(char *versionString, struct SerialIO *port);
static DiagnosticResponse internal_open_port(const char *portName, bool enableCTSflowcontrol,
                                             bool triggerReset, char *versionString, struct SerialIO *port);
static DiagnosticResponse run_command(ArduinoInterfacePrivate *priv, char command, char parameter, char *actualResponse);
static bool device_read(ArduinoInterfacePrivate *priv, void *target, unsigned int numBytes, bool failIfNotAllRead);
static void apply_comm_timeouts(ArduinoInterfacePrivate *priv, bool shortTimeouts);
static DiagnosticResponse internal_write_track(ArduinoInterfacePrivate *priv, const unsigned char *data, int length, bool writeFromIndexPulse, bool usePrecomp);
static DiagnosticResponse write_current_track_hd(ArduinoInterfacePrivate *priv, const unsigned char *mfmData, unsigned short numBytes, bool writeFromIndexPulse);

// Get current time in milliseconds
static long long get_time_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)(tv.tv_sec) * 1000 + (long long)(tv.tv_usec) / 1000;
}

// Sleep for specified milliseconds
static void sleep_ms(int ms)
{
    usleep(ms * 1000);
}

// Linux Serial IO Implementation
static SerialResponse serial_open_port(struct SerialIO *port, const char *portName)
{
    arduino_log("[DEBUG] Opening serial port: %s\n", portName);

    if (port->isOpen)
    {
        arduino_log("[DEBUG] Port already open, closing first\n");
        serial_close_port(port);
    }

    strncpy(port->portName, portName, sizeof(port->portName) - 1);
    port->portName[sizeof(port->portName) - 1] = '\0';

    port->fd = open(portName, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (port->fd < 0)
    {
        arduino_log("[DEBUG] Failed to open port %s: %s (errno=%d)\n", portName, strerror(errno), errno);
        switch (errno)
        {
        case ENOENT:
        case ENODEV:
            return SERIAL_RESPONSE_NOT_FOUND;
        case EBUSY:
            return SERIAL_RESPONSE_IN_USE;
        case EACCES:
            return SERIAL_RESPONSE_ACCESS_DENIED;
        default:
            return SERIAL_RESPONSE_ERROR;
        }
    }

    arduino_log("[DEBUG] Port opened successfully, fd=%d\n", port->fd);

    // Save current terminal settings
    if (tcgetattr(port->fd, &port->oldTermios) != 0)
    {
        arduino_log("[DEBUG] Failed to get terminal attributes: %s\n", strerror(errno));
        close(port->fd);
        return SERIAL_RESPONSE_ERROR;
    }

    port->isOpen = true;
    arduino_log("[DEBUG] Port configuration saved\n");
    return SERIAL_RESPONSE_OK;
}

static SerialResponse serial_configure_port(struct SerialIO *port, const SerialConfiguration *config)
{
    arduino_log("[DEBUG] Configuring port with baud rate: %d, CTS flow control: %s\n",
           config->baudRate, config->ctsFlowControl ? "enabled" : "disabled");

    if (!port->isOpen)
    {
        arduino_log("[DEBUG] Error: Port not open for configuration\n");
        return SERIAL_RESPONSE_ERROR;
    }

    struct termios tty;
    memset(&tty, 0, sizeof(tty));

    if (tcgetattr(port->fd, &tty) != 0)
    {
        arduino_log("[DEBUG] Failed to get terminal attributes for configuration: %s\n", strerror(errno));
        return SERIAL_RESPONSE_ERROR;
    }

    // Set baud rate
    speed_t baudRate = B2000000;

    if (cfsetospeed(&tty, baudRate) != 0)
    {
        arduino_log("[DEBUG] Failed to set output baud rate: %s\n", strerror(errno));
        return SERIAL_RESPONSE_ERROR;
    }
    if (cfsetispeed(&tty, baudRate) != 0)
    {
        arduino_log("[DEBUG] Failed to set input baud rate: %s\n", strerror(errno));
        return SERIAL_RESPONSE_ERROR;
    }

    // 8 data bits, no parity, 1 stop bit
    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8; // 8 data bits
    tty.c_cflag &= ~PARENB;                     // no parity
    tty.c_cflag &= ~CSTOPB;                     // 1 stop bit

    // Enable receiver and set local mode
    tty.c_cflag |= CREAD | CLOCAL;

    // Hardware flow control
    if (config->ctsFlowControl)
    {
        tty.c_cflag |= CRTSCTS;
        arduino_log("[DEBUG] Hardware flow control enabled\n");
    }
    else
    {
        tty.c_cflag &= ~CRTSCTS;
        arduino_log("[DEBUG] Hardware flow control disabled\n");
    }

    // Set raw input/output
    tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG); // raw input
    tty.c_oflag &= ~OPOST;                          // raw output

    // No software flow control
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);

    // Set DTR (Data Terminal Ready) line explicitly
    int dtr_flag = TIOCM_DTR;
    if (ioctl(port->fd, TIOCMBIS, &dtr_flag) < 0)
    {
        perror("[DEBUG] Failted to set DTR, not fatal");
    }

    if (tcsetattr(port->fd, TCSANOW, &tty) != 0)
    {
        arduino_log("[DEBUG] Failed to set terminal attributes: %s\n", strerror(errno));
        return SERIAL_RESPONSE_ERROR;
    }

    arduino_log("[DEBUG] Port configured successfully\n");
    return SERIAL_RESPONSE_OK;
}

static SerialResponse serial_close_port(struct SerialIO *port)
{
    if (port->isOpen && port->fd >= 0)
    {
        // Restore original terminal settings
        tcsetattr(port->fd, TCSANOW, &port->oldTermios);
        close(port->fd);
        port->fd = -1;
        port->isOpen = false;
    }
    return SERIAL_RESPONSE_OK;
}

static int serial_write(struct SerialIO *port, const void *data, unsigned int size)
{
    if (!port->isOpen || port->fd < 0)
    {
        arduino_log("[DEBUG] Write failed: port not open\n");
        return -1;
    }

    unsigned int totalTime = port->writeTimeout + (port->writeTimeoutMultiplier * size);

    struct timeval timeout;
    timeout.tv_sec = totalTime / 1000;
    timeout.tv_usec = (totalTime - (timeout.tv_sec * 1000)) * 1000;

    size_t written = 0;
    unsigned char *buffer = (unsigned char *)data;

    fd_set fds;

    FD_ZERO(&fds);
    FD_SET(port->fd, &fds);

    // Write with a timeout
    while (written < size)
    {

        int result = select(port->fd + 1, NULL, &fds, NULL, &timeout);
        if (result < 0)
        {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            else
                return 0;
        }
        else if (result == 0)
            break;

        result = write(port->fd, buffer, size - written);

        if (result < 0)
        {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            else
                return 0;
        }

        written += result;
        buffer += result;
    }

    return written;
}

static int serial_read(struct SerialIO *port, void *data, unsigned int size)
{
    if ((data == NULL) || (size == 0))
        return 0;

    if (!port->isOpen || port->fd < 0)
    {
        arduino_log("[DEBUG] Read failed: port not open\n");
        return -1;
    }

    unsigned int totalTime = port->readTimeout + (port->readTimeoutMultiplier * size);

    struct timeval timeout;
    timeout.tv_sec = totalTime / 1000;
    timeout.tv_usec = (totalTime - (timeout.tv_sec * 1000)) * 1000;

    size_t bytesRead = 0;
    unsigned char *buffer = (unsigned char *)data;

    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(port->fd, &fds);

    while (bytesRead < size)
    {
        if ((timeout.tv_sec < 1) && (timeout.tv_usec < 1))
        {
            break;
        }

        int result = select(port->fd + 1, &fds, NULL, NULL, &timeout);

        if (result < 0)
        {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            else
                return 0;
        }
        else if (result == 0)
            break;
        result = read(port->fd, buffer, size - bytesRead);

        if (result < 0)
        {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            else
                return 0;
        }
        bytesRead += result;
        buffer += result;
    }

    return bytesRead;
}

static void serial_set_read_timeout(struct SerialIO *port, int waitTimetimeout, int multiplier)
{
    port->readTimeout = waitTimetimeout;
    port->readTimeoutMultiplier = multiplier;
}

static void serial_set_write_timeout(struct SerialIO *port, int waitTimetimeout, int multiplier)
{
    port->writeTimeout = waitTimetimeout;
    port->writeTimeoutMultiplier = multiplier;
}

static void serial_set_dtr(struct SerialIO *port, bool state)
{
    if (!port->isOpen || port->fd < 0)
        return;

    int status;
    ioctl(port->fd, TIOCMGET, &status);

    if (state)
    {
        status |= TIOCM_DTR;
    }
    else
    {
        status &= ~TIOCM_DTR;
    }

    ioctl(port->fd, TIOCMSET, &status);
}

static void serial_set_rts(struct SerialIO *port, bool state)
{
    if (!port->isOpen || port->fd < 0)
        return;

    int status;
    ioctl(port->fd, TIOCMGET, &status);

    if (state)
    {
        status |= TIOCM_RTS;
    }
    else
    {
        status &= ~TIOCM_RTS;
    }

    ioctl(port->fd, TIOCMSET, &status);
}

static bool serial_get_cts_status(struct SerialIO *port)
{
    if (!port->isOpen || port->fd < 0)
        return false;

    int status;
    ioctl(port->fd, TIOCMGET, &status);
    return (status & TIOCM_CTS) != 0;
}

static void serial_purge_buffers(struct SerialIO *port)
{
    if (!port->isOpen || port->fd < 0)
        return;

    tcflush(port->fd, TCIOFLUSH);
}

static int serial_get_bytes_waiting(struct SerialIO *port)
{
    if (!port->isOpen || port->fd < 0)
        return 0;

    int waiting;
    if (ioctl(port->fd, TIOCINQ, &waiting) < 0)
        return 0;

    return (unsigned int)waiting;
}

// Helper function implementations
static DiagnosticResponse attempt_to_sync(char *versionString, struct SerialIO *port)
{
    arduino_log("[DEBUG] Starting sync attempt\n");
    char buffer[10];

    // Send 'Version' Request
    buffer[0] = SPECIAL_ABORT_CHAR;
    buffer[1] = COMMAND_RESET; // Reset
    buffer[2] = COMMAND_VERSION;

    arduino_log("[DEBUG] Sending initial sync command\n");
    int size = serial_write(port, buffer, 3);
    if (size != 3)
    {
        // Couldn't write to device
        arduino_log("[DEBUG] Failed to send sync command, only wrote %d/3 bytes\n", size);
        serial_close_port(port);
        return DIAGNOSTIC_RESPONSE_PORT_ERROR;
    }

    memset(buffer, 0, sizeof(buffer));
    int counterNoData = 0;
    int counterData = 0;
    int bytesRead = 0;

    // Keep a rolling buffer looking for the 1Vxxx response
    arduino_log("[DEBUG] Waiting for version response (timeout: 8 seconds)\n");
    long long startTime = get_time_ms();
    for (;;)
    {
        long long timePassed = (get_time_ms() - startTime) / 1000;

        // Timeout after 8 seconds.
        if (timePassed >= 8)
        {
            arduino_log("[DEBUG] Timeout waiting for version response\n");
            return DIAGNOSTIC_RESPONSE_ERROR_READING_VERSION; // Don't close port here
        }

        size = serial_read(port, &buffer[4], 1);
        bytesRead += size;

        // Was something read?
        if (size > 0)
        {
            if (buffer[0] == '1' && buffer[1] == 'V' && (buffer[2] >= '1' && buffer[2] <= '9') && (buffer[3] == ',' || buffer[3] == '.') && (buffer[4] >= '0' && buffer[4] <= '9'))
            {

                // Success
                serial_purge_buffers(port);
                sleep_ms(1);
                serial_purge_buffers(port);
                strcpy(versionString, &buffer[1]);
                return DIAGNOSTIC_RESPONSE_OK;
            }
            else
            {
                if (bytesRead)
                    bytesRead--;
            }

            // Move backwards
            for (int a = 0; a < 4; a++)
            {
                buffer[a] = buffer[a + 1];
            }

            if (counterData++ > 2048)
            {
                arduino_log("[DEBUG] Too much data received without valid version\n");
                return DIAGNOSTIC_RESPONSE_ERROR_MALFORMED_VERSION; // Don't close port here
            }
        }
        else
        {
            sleep_ms(1);
            if (counterNoData++ > 120)
            {
                arduino_log("[DEBUG] No data received for too long\n");
                return DIAGNOSTIC_RESPONSE_ERROR_READING_VERSION; // Don't close port here
            }
            if (counterNoData % 7 == 6 && bytesRead == 0)
            {
                // Give it a kick
                arduino_log("[DEBUG] Sending version request kick\n");
                buffer[0] = COMMAND_VERSION;
                size = serial_write(port, buffer, 1);
                if (size != 1)
                {
                    // Couldn't write to device
                    arduino_log("[DEBUG] Failed to send version kick\n");
                    return DIAGNOSTIC_RESPONSE_PORT_ERROR; // Don't close port here
                }
            }
        }
    }
}

static DiagnosticResponse internal_open_port(const char *portName, bool enableCTSflowcontrol,
                                             bool triggerReset, char *versionString, struct SerialIO *port)
{
    arduino_log("[DEBUG] Internal open port: %s (CTS: %s, Reset: %s)\n",
           portName, enableCTSflowcontrol ? "enabled" : "disabled",
           triggerReset ? "enabled" : "disabled");

    SerialResponse response = serial_open_port(port, portName);
    switch (response)
    {
    case SERIAL_RESPONSE_IN_USE:
        arduino_log("[DEBUG] Port in use\n");
        return DIAGNOSTIC_RESPONSE_PORT_IN_USE;
    case SERIAL_RESPONSE_NOT_FOUND:
        arduino_log("[DEBUG] Port not found\n");
        return DIAGNOSTIC_RESPONSE_PORT_NOT_FOUND;
    case SERIAL_RESPONSE_OK:
        arduino_log("[DEBUG] Port opened successfully\n");
        break;
    default:
        arduino_log("[DEBUG] Port open error\n");
        return DIAGNOSTIC_RESPONSE_PORT_ERROR;
    }

    arduino_log("[DEBUG] Trying baud rate: %d\n", 2000000);

    // Configure the port
    SerialConfiguration config;
    config.baudRate = 2000000;
    config.ctsFlowControl = enableCTSflowcontrol;

    if (serial_configure_port(port, &config) != SERIAL_RESPONSE_OK)
    {
        arduino_log("[DEBUG] Port configuration failed for baud rate %d\n", 2000000);
        return DIAGNOSTIC_RESPONSE_PORT_ERROR;
    }

    //  serial_set_buffer_sizes(port, 16, 16);
    serial_set_read_timeout(port, 10, 250);
    serial_set_write_timeout(port, 2000, 200);

    // Try to get the version with this baud rate
    arduino_log("[DEBUG] Testing sync at baud rate %d\n", 2000000);
    DiagnosticResponse diagResponse = attempt_to_sync(versionString, port);

    if (diagResponse == DIAGNOSTIC_RESPONSE_OK)
    {
        arduino_log("[DEBUG] Success at baud rate %d!\n", 2000000);
        return diagResponse;
    }

    arduino_log("[DEBUG] Failed at baud rate %d, trying next...\n", 2000000);

    // All baud rates failed, try reset sequence if allowed
    if (triggerReset)
    {
        arduino_log("[DEBUG] All baud rates failed, attempting reset sequence with 2M baud\n");

        // Configure back to 2M baud for reset
        SerialConfiguration config;
        config.baudRate = 2000000;
        config.ctsFlowControl = enableCTSflowcontrol;
        serial_configure_port(port, &config);

        serial_set_dtr(port, false);
        serial_set_rts(port, false);
        sleep_ms(10);
        serial_set_dtr(port, true);
        serial_set_rts(port, true);
        sleep_ms(10);
        serial_close_port(port);
        sleep_ms(150);

        // Now re-connect and try again
        arduino_log("[DEBUG] Reopening port after reset\n");
        if (serial_open_port(port, portName) != SERIAL_RESPONSE_OK)
        {
            arduino_log("[DEBUG] Failed to reopen port after reset\n");
            return DIAGNOSTIC_RESPONSE_PORT_ERROR;
        }

        serial_close_port(port);
        return DIAGNOSTIC_RESPONSE_ERROR_READING_VERSION;
    }
    else
    {
        arduino_log("[DEBUG] All baud rates failed and reset disabled\n");
        serial_close_port(port);
        return DIAGNOSTIC_RESPONSE_ERROR_READING_VERSION;
    }
}

static DiagnosticResponse run_command(ArduinoInterfacePrivate *priv, char command, char parameter, char *actualResponse)
{
    unsigned char response;

    // Pause for I/O
    sleep_ms(1);

    // Send the command
    if (serial_write(&priv->comPort, &command, 1) != 1)
    {
        return DIAGNOSTIC_RESPONSE_SEND_FAILED;
    }

    // Only send the parameter if its not NULL
    if (parameter != '\0')
    {
        if (serial_write(&priv->comPort, &parameter, 1) != 1)
        {
            return DIAGNOSTIC_RESPONSE_SEND_PARAMETER_FAILED;
        }
    }

    // And read the response
    if (!device_read(priv, &response, 1, true))
    {
        return DIAGNOSTIC_RESPONSE_READ_RESPONSE_FAILED;
    }

    if (actualResponse)
    {
        *actualResponse = response;
    }

    // Evaluate the response
    switch (response)
    {
    case '1':
        priv->lastError = DIAGNOSTIC_RESPONSE_OK;
        break;
    case '0':
        priv->lastError = DIAGNOSTIC_RESPONSE_ERROR;
        break;
    default:
        priv->lastError = DIAGNOSTIC_RESPONSE_STATUS_ERROR;
        break;
    }
    return priv->lastError;
}

static bool device_read(ArduinoInterfacePrivate *priv, void *target, unsigned int numBytes, bool failIfNotAllRead)
{
    unsigned long read = serial_read(&priv->comPort, target, numBytes);

    if (read < numBytes)
    {
        if (failIfNotAllRead)
            return false;

        // Clear the unread bytes
        char *target2 = (char *)target + read;
        memset(target2, 0, numBytes - read);
    }

    return true;
}

static void apply_comm_timeouts(ArduinoInterfacePrivate *priv, bool shortTimeouts)
{
    if (shortTimeouts)
    {
        serial_set_read_timeout(&priv->comPort, 5, 12);
    }
    else
    {
        serial_set_read_timeout(&priv->comPort, 2000, 200);
    }
    serial_set_write_timeout(&priv->comPort, 2000, 200);
}

// Public API Implementation
ArduinoInterface *arduino_interface_create(void)
{
    ArduinoInterface *interface = malloc(sizeof(ArduinoInterface));
    if (!interface)
        return NULL;

    ArduinoInterfacePrivate *priv = malloc(sizeof(ArduinoInterfacePrivate));
    if (!priv)
    {
        free(interface);
        return NULL;
    }

    // Initialize private data
    memset(priv, 0, sizeof(ArduinoInterfacePrivate));
    priv->comPort.fd = -1;
    priv->comPort.isOpen = false;
    priv->abortStreaming = true;
    priv->version.major = 0;
    priv->version.minor = 0;
    priv->version.fullControlMod = false;
    priv->lastError = DIAGNOSTIC_RESPONSE_OK;
    priv->lastCommand = LAST_COMMAND_GET_VERSION;
    priv->inWriteMode = false;
    priv->isWriteProtected = false;
    priv->diskInDrive = false;
    priv->isHDMode = false;
    priv->abortSignalled = false;
    priv->isStreaming = false;

    interface->private_data = priv;
    return interface;
}

void arduino_interface_destroy(ArduinoInterface *interface)
{
    if (!interface)
        return;

    ArduinoInterfacePrivate *priv = (ArduinoInterfacePrivate *)interface->private_data;
    if (priv)
    {
        arduino_interface_close_port(interface);
        free(priv);
    }
    free(interface);
}

DiagnosticResponse arduino_interface_open_port(ArduinoInterface *interface, const char *portName, bool enableCTSflowcontrol)
{
    if (!interface || !interface->private_data)
    {
        return DIAGNOSTIC_RESPONSE_ERROR;
    }

    ArduinoInterfacePrivate *priv = (ArduinoInterfacePrivate *)interface->private_data;
    priv->lastCommand = LAST_COMMAND_OPEN_PORT;

    // Close existing port
    arduino_interface_close_port(interface);

    // Quickly force streaming to be aborted
    priv->abortStreaming = true;

    char versionString[32];
    priv->lastError = internal_open_port(portName, enableCTSflowcontrol, true, versionString, &priv->comPort);
    if (priv->lastError != DIAGNOSTIC_RESPONSE_OK)
    {
        return priv->lastError;
    }

    // Clear any redundant data in buffer
    char buffer[2];
    int counter = 0;
    while (serial_get_bytes_waiting(&priv->comPort))
    {
        int size = serial_read(&priv->comPort, buffer, 1);
        if (size < 1)
        {
            if (counter++ >= 5)
                break;
        }
    }

    arduino_log("[DEBUG] Firmware version string: %s\n", versionString);
    // Parse version string
    priv->version.major = versionString[1] - '0';
    priv->version.minor = versionString[3] - '0';
    priv->version.fullControlMod = versionString[2] == ',';

    // Check features
    priv->version.deviceFlags1 = 0;
    priv->version.deviceFlags2 = 0;
    priv->version.buildNumber = 0;

    if ((priv->version.major > 1) || ((priv->version.major == 1) && (priv->version.minor >= 9)))
    {
        priv->lastError = run_command(priv, COMMAND_CHECK_FEATURES, '\0', NULL);
        if (priv->lastError != DIAGNOSTIC_RESPONSE_OK)
        {
            return priv->lastError;
        }

        if (!device_read(priv, &priv->version.deviceFlags1, 1, false))
        {
            priv->lastError = DIAGNOSTIC_RESPONSE_ERROR_READING_VERSION;
            return priv->lastError;
        }
        if (!device_read(priv, &priv->version.deviceFlags2, 1, false))
        {
            priv->lastError = DIAGNOSTIC_RESPONSE_ERROR_READING_VERSION;
            return priv->lastError;
        }
        if (!device_read(priv, &priv->version.buildNumber, 1, false))
        {
            priv->lastError = DIAGNOSTIC_RESPONSE_ERROR_READING_VERSION;
            return priv->lastError;
        }
        arduino_log("[DEBUG] Device Flags1: 0x%02X, Flags2: 0x%02X, Build Number: %d\n",
               priv->version.deviceFlags1, priv->version.deviceFlags2, priv->version.buildNumber);
    }

    // Switch to normal timeouts
    apply_comm_timeouts(priv, false);

    return priv->lastError;
}

void arduino_interface_close_port(ArduinoInterface *interface)
{
    if (!interface || !interface->private_data)
        return;

    if (!arduino_interface_is_open(interface))
        return;

    // Force the drive to power down
    arduino_interface_enable_reading(interface, false, false, false);

    ArduinoInterfacePrivate *priv = (ArduinoInterfacePrivate *)interface->private_data;
    serial_close_port(&priv->comPort);
}

bool arduino_interface_is_open(const ArduinoInterface *interface)
{
    if (!interface || !interface->private_data)
        return false;

    ArduinoInterfacePrivate *priv = (ArduinoInterfacePrivate *)interface->private_data;
    return priv->comPort.isOpen;
}

LastCommand arduino_interface_get_last_failed_command(const ArduinoInterface *interface)
{
    if (!interface || !interface->private_data)
        return LAST_COMMAND_OPEN_PORT;

    ArduinoInterfacePrivate *priv = (ArduinoInterfacePrivate *)interface->private_data;
    return priv->lastCommand;
}

DiagnosticResponse arduino_interface_get_last_error(const ArduinoInterface *interface)
{
    if (!interface || !interface->private_data)
        return DIAGNOSTIC_RESPONSE_ERROR;

    ArduinoInterfacePrivate *priv = (ArduinoInterfacePrivate *)interface->private_data;
    return priv->lastError;
}

FirmwareVersion arduino_interface_get_firmware_version(const ArduinoInterface *interface)
{
    FirmwareVersion version = {0, 0, false, 0, 0, 0};
    if (!interface || !interface->private_data)
        return version;

    ArduinoInterfacePrivate *priv = (ArduinoInterfacePrivate *)interface->private_data;
    return priv->version;
}

const char *arduino_interface_get_last_error_str(const ArduinoInterface *interface)
{
    if (!interface || !interface->private_data)
        return "Invalid interface\n";

    ArduinoInterfacePrivate *priv = (ArduinoInterfacePrivate *)interface->private_data;

    switch (priv->lastError)
    {
    case DIAGNOSTIC_RESPONSE_OK:
        return "Last command completed successfully.\n";
    case DIAGNOSTIC_RESPONSE_PORT_IN_USE:
        return "The specified port is currently in use by another application.\n";
    case DIAGNOSTIC_RESPONSE_PORT_NOT_FOUND:
        return "The specified port was not found.\n";
    case DIAGNOSTIC_RESPONSE_ACCESS_DENIED:
        return "The operating system denied access to the specified port.\n";
    case DIAGNOSTIC_RESPONSE_COMPORT_CONFIG_ERROR:
        return "We were unable to configure the port.\n";
    case DIAGNOSTIC_RESPONSE_BAUD_RATE_NOT_SUPPORTED:
        return "The port does not support the 2M baud rate required by this application.\n";
    case DIAGNOSTIC_RESPONSE_ERROR_READING_VERSION:
        return "An error occurred attempting to read the version of the sketch running on the Arduino.\n";
    case DIAGNOSTIC_RESPONSE_ERROR_MALFORMED_VERSION:
        return "The Arduino returned an unexpected string when version was requested.\n";
    case DIAGNOSTIC_RESPONSE_PORT_ERROR:
        return "An unknown error occurred attempting to open access to the specified port.\n";
    case DIAGNOSTIC_RESPONSE_OLD_FIRMWARE:
        return "The Arduino/DrawBridge is running an older version of the firmware/sketch. Please re-upload.\n";
    default:
        return "Unknown error.\n";
    }
}

DiagnosticResponse arduino_interface_test_cts(ArduinoInterface *interface)
{
    ArduinoInterfacePrivate *priv = (ArduinoInterfacePrivate *)interface->private_data;

    for (int a = 1; a <= 10; a++)
    {
        // Port opened.  We need to check what happens as the pin is toggled
        priv->lastError = run_command(priv, COMMAND_DIAGNOSTICS, a & 1 ? '1' : '2', NULL);
        if (priv->lastError != DIAGNOSTIC_RESPONSE_OK)
        {
            arduino_log("[DEBUG] Failed to send diagnostics command\n");
            priv->lastCommand = LAST_COMMAND_RUN_DIAGNOSTICS;
            serial_close_port(&priv->comPort);
            return priv->lastError;
        }
        usleep(1000); // Wait 1ms for the Arduino to process the command

        bool ctsStatus = serial_get_cts_status(&priv->comPort);
        arduino_log("[DEBUG] CTS status for toggle %d: %s\n", a, ctsStatus ? "HIGH" : "LOW");

        // This doesn't actually run a command, this switches the CTS line back to its default setting
        priv->lastError = run_command(priv, COMMAND_DIAGNOSTICS, '\0', NULL);

        if (ctsStatus ^ ((a & 1) != 0))
        {
            arduino_log("[DEBUG] CTS status did not match expected value\n");
            // If we get here then the CTS value isn't what it should be
            serial_close_port(&priv->comPort);
            priv->lastError = DIAGNOSTIC_RESPONSE_CTS_FAILURE;
            return priv->lastError;
        }
        // Pass.  Try the other state
        usleep(1000); // Wait 1ms for the Arduino to process the command
    }
    return DIAGNOSTIC_RESPONSE_OK;
}

DiagnosticResponse arduino_interface_check_if_disk_is_write_protected(ArduinoInterface *interface, bool forceCheck)
{
    ArduinoInterfacePrivate *priv = (ArduinoInterfacePrivate *)interface->private_data;

    priv->lastCommand = LAST_COMMAND_CHECK_DISK_WRITE_PROTECTED;

    if (!forceCheck)
    {
        return priv->isWriteProtected ? DIAGNOSTIC_RESPONSE_WRITE_PROTECTED : DIAGNOSTIC_RESPONSE_OK;
    }

    priv->lastError = arduino_interface_check_for_disk(interface, true);
    if (priv->lastError == DIAGNOSTIC_RESPONSE_STATUS_ERROR || priv->lastError == DIAGNOSTIC_RESPONSE_OK)
    {
        // Disk is present, check write protection status
        if (priv->isWriteProtected)
            return DIAGNOSTIC_RESPONSE_WRITE_PROTECTED;
    }

    return priv->lastError;
}

DiagnosticResponse arduino_interface_check_for_disk(ArduinoInterface *interface, bool forceCheck)
{
    ArduinoInterfacePrivate *priv = (ArduinoInterfacePrivate *)interface->private_data;

    // Test manually
    priv->lastCommand = LAST_COMMAND_CHECK_DISK_IN_DRIVE;

    if (!forceCheck)
    {
        return priv->diskInDrive ? DIAGNOSTIC_RESPONSE_OK : DIAGNOSTIC_RESPONSE_NO_DISK_IN_DRIVE;
    }

    char response;
    priv->lastError = run_command(priv, COMMAND_CHECKDISKEXISTS, '\0', &response);
    if (priv->lastError == DIAGNOSTIC_RESPONSE_STATUS_ERROR || priv->lastError == DIAGNOSTIC_RESPONSE_OK)
    {
        if (response == '#')
        {
            priv->lastError = DIAGNOSTIC_RESPONSE_NO_DISK_IN_DRIVE;
            priv->diskInDrive = false;
        }
        else
        {
            if (response == '1')
                priv->diskInDrive = true;
            else
            {
                priv->lastError = DIAGNOSTIC_RESPONSE_READ_RESPONSE_FAILED;
                return priv->lastError;
            }
        }

        // Also read the write protect status
        if (!device_read(priv, &response, 1, true))
        {
            priv->lastError = DIAGNOSTIC_RESPONSE_READ_RESPONSE_FAILED;
            return priv->lastError;
        }

        if (response == '1' || response == '#')
            priv->isWriteProtected = response == '1';

        sleep_ms(1); // Give the Arduino a moment to recover
    }
    return priv->lastError;
}

DiagnosticResponse arduino_interface_enable_reading(ArduinoInterface *interface, bool enable, bool reset, bool dontWait)
{
    ArduinoInterfacePrivate *priv = (ArduinoInterfacePrivate *)interface->private_data;

    priv->inWriteMode = false;
    if (enable)
    {
        priv->lastCommand = LAST_COMMAND_ENABLE_MOTOR;

        // Enable the device
        priv->lastError = run_command(priv, dontWait ? COMMAND_ENABLE_NOWAIT : COMMAND_ENABLE, '\0', NULL);
        if (priv->lastError != DIAGNOSTIC_RESPONSE_OK)
            return priv->lastError;

        // Reset?
        if (reset)
        {
            priv->lastError = arduino_interface_find_track0(interface);
            if (priv->lastError != DIAGNOSTIC_RESPONSE_OK)
                return priv->lastError;

            // Lets know where we are
            return arduino_interface_select_surface(interface, DISK_SURFACE_UPPER);
        }
        priv->lastError = DIAGNOSTIC_RESPONSE_OK;
        priv->inWriteMode = priv->version.fullControlMod;
        return priv->lastError;
    }
    else
    {
        priv->lastCommand = LAST_COMMAND_DISABLE_MOTOR;

        // Disable the device
        priv->lastError = run_command(priv, COMMAND_DISABLE, '\0', NULL);

        return priv->lastError;
    }
}

DiagnosticResponse arduino_interface_find_track0(ArduinoInterface *interface)
{
    ArduinoInterfacePrivate *priv = (ArduinoInterfacePrivate *)interface->private_data;

    priv->lastCommand = LAST_COMMAND_REWIND;

    // And rewind to the first track
    char status = '0';
    priv->lastError = run_command(priv, COMMAND_REWIND, '\0', &status);
    if (priv->lastError != DIAGNOSTIC_RESPONSE_OK)
    {
        if (status == '#')
            return DIAGNOSTIC_RESPONSE_REWIND_FAILURE;
    }
    return priv->lastError;
}

DiagnosticResponse arduino_interface_select_surface(ArduinoInterface *interface, DiskSurface side)
{
    ArduinoInterfacePrivate *priv = (ArduinoInterfacePrivate *)interface->private_data;

    priv->lastCommand = LAST_COMMAND_SELECT_SURFACE;

    priv->lastError = run_command(priv, side == DISK_SURFACE_UPPER ? COMMAND_HEAD0 : COMMAND_HEAD1, '\0', NULL);

    return priv->lastError;
}

DiagnosticResponse arduino_interface_test_index_pulse(ArduinoInterface *interface)
{
    ArduinoInterfacePrivate *priv = (ArduinoInterfacePrivate *)interface->private_data;

    priv->lastCommand = LAST_COMMAND_RUN_DIAGNOSTICS;

    // Port opened.  We need to check what happens as the pin is toggled
    priv->lastError = run_command(priv, COMMAND_DIAGNOSTICS, '3', NULL);

    return priv->lastError;
}

DiagnosticResponse arduino_interface_measure_drive_rpm(ArduinoInterface *interface, float *rpm)
{
    ArduinoInterfacePrivate *priv = (ArduinoInterfacePrivate *)interface->private_data;

    priv->lastCommand = LAST_COMMAND_MEASURE_RPM;

    // Query the RPM
    priv->lastError = run_command(priv, COMMAND_TEST_RPM, '\0', NULL);
    if (priv->lastError != DIAGNOSTIC_RESPONSE_OK)
        return priv->lastError;

    // Now we read bytes until we get an '\n' or a max of 10
    char buffer[11];
    int index = 0;
    int failCount = 0;
    memset(buffer, 0, sizeof buffer);

    // Read RPM
    while (index < 10)
    {
        if (device_read(priv, &buffer[index], 1, false))
        {
            if (buffer[index] == '\n')
            {
                buffer[index] = '\0';
                break;
            }
            index++;
        }
        else if (failCount++ > 10)
            break;
    }

    // And output it as a float
    *rpm = (float)atof(buffer);

    if (*rpm < 10)
        priv->lastError = DIAGNOSTIC_RESPONSE_NO_DISK_IN_DRIVE;

    return priv->lastError;
}

DiagnosticResponse arduino_interface_select_track(ArduinoInterface *interface, unsigned char trackIndex)
{
    ArduinoInterfacePrivate *priv = (ArduinoInterfacePrivate *)interface->private_data;

    priv->lastCommand = LAST_COMMAND_GOTO_TRACK;

    if (trackIndex > 83)
    {
        priv->lastError = DIAGNOSTIC_RESPONSE_TRACK_RANGE_ERROR;
        return priv->lastError; // no chance, it can't be done.
    }

    // And send the command and track.  This is sent as ASCII text as a result of terminal testing.  Easier to see whats going on
    char buf[8];
    sprintf(buf, "%c%02i", COMMAND_GOTOTRACK, trackIndex);

    // Send track number.
    if (!serial_write(&priv->comPort, buf, (unsigned int)strlen(buf)))
    {
        priv->lastError = DIAGNOSTIC_RESPONSE_SEND_FAILED;
        return priv->lastError;
    }

    // Get result
    char result;
    if (!device_read(priv, &result, 1, true))
    {
        priv->lastError = DIAGNOSTIC_RESPONSE_READ_RESPONSE_FAILED;
        return priv->lastError;
    }

    switch (result)
    {
    case '2':
        priv->lastError = DIAGNOSTIC_RESPONSE_OK;
        break; // already at track.  No op needed.  V1.8 only
    case '1':
        priv->lastError = DIAGNOSTIC_RESPONSE_OK;
        break;
    case '0':
        priv->lastError = DIAGNOSTIC_RESPONSE_SELECT_TRACK_ERROR;
        break;
    default:
        priv->lastError = DIAGNOSTIC_RESPONSE_STATUS_ERROR;
        break;
    }

    return priv->lastError;
}

bool arduino_interface_is_disk_in_drive(const ArduinoInterface *interface)
{
    ArduinoInterfacePrivate *priv = (ArduinoInterfacePrivate *)interface->private_data;
    return priv->diskInDrive;
}

DiagnosticResponse arduino_interface_check_disk_capacity(ArduinoInterface *interface, bool *isHD)
{
    ArduinoInterfacePrivate *priv = (ArduinoInterfacePrivate *)interface->private_data;

    priv->lastCommand = LAST_COMMAND_CHECK_DENSITY;

    if ((priv->version.deviceFlags1 & FLAGS_DENSITYDETECT_ENABLED) == 0)
    {
        *isHD = false;
        return DIAGNOSTIC_RESPONSE_OK;
    }

    // Query the density
    priv->lastError = run_command(priv, COMMAND_CHECK_DENSITY, '\0', NULL);
    if (priv->lastError != DIAGNOSTIC_RESPONSE_OK)
    {
        return priv->lastError;
    }

    // And read the type
    char status;
    if (!device_read(priv, &status, 1, true))
    {
        priv->lastError = DIAGNOSTIC_RESPONSE_READ_RESPONSE_FAILED;
        return priv->lastError;
    }

    // 'x' means no disk in drive
    switch (status)
    {
    case 'x':
        // m_diskInDrive = false;
        // We're not going to update the disk in drive flag, just use it as an OK error
        priv->lastError = DIAGNOSTIC_RESPONSE_NO_DISK_IN_DRIVE;
        break;

    case 'H':
        priv->diskInDrive = true;
        *isHD = true;
        priv->lastError = DIAGNOSTIC_RESPONSE_OK;
        break;

    case 'D':
        priv->diskInDrive = true;
        isHD = false;
        priv->lastError = DIAGNOSTIC_RESPONSE_OK;
        break;
    }

    return priv->lastError;
}

DiagnosticResponse arduino_interface_set_disk_capacity(ArduinoInterface *interface, bool switchToHD_Disk)
{
    ArduinoInterfacePrivate *priv = (ArduinoInterfacePrivate *)interface->private_data;

    priv->lastCommand = LAST_COMMAND_SWITCH_DISK_MODE;

    priv->lastError = run_command(priv, switchToHD_Disk ? COMMAND_SWITCHTO_HD : COMMAND_SWITCHTO_DD, '\0', NULL);

    if (priv->lastError == DIAGNOSTIC_RESPONSE_OK)
        priv->isHDMode = switchToHD_Disk;

    return priv->lastError;
}

DiagnosticResponse arduino_interface_test_data_pulse(ArduinoInterface *interface)
{
    ArduinoInterfacePrivate *priv = (ArduinoInterfacePrivate *)interface->private_data;
    priv->lastCommand = LAST_COMMAND_RUN_DIAGNOSTICS;

    // Port opened.  We need to check what happens as the pin is toggled
    priv->lastError = run_command(priv, COMMAND_DIAGNOSTICS, '4', NULL);

    return priv->lastError;
}

DiagnosticResponse arduino_interface_read_current_track(ArduinoInterface *interface, void *trackData, int dataLength, bool readFromIndexPulse)
{
    ArduinoInterfacePrivate *priv = (ArduinoInterfacePrivate *)interface->private_data;

    priv->lastCommand = LAST_COMMAND_READ_TRACK;

    // Length must be one of the two types
    if (dataLength == RAW_TRACKDATA_LENGTH_DD && priv->isHDMode)
    {
        priv->lastError = DIAGNOSTIC_RESPONSE_MEDIA_TYPE_MISMATCH;
        return priv->lastError;
    }
    if (dataLength == RAW_TRACKDATA_LENGTH_HD && !priv->isHDMode)
    {
        priv->lastError = DIAGNOSTIC_RESPONSE_MEDIA_TYPE_MISMATCH;
        return priv->lastError;
    }

    RawTrackDataHD *tmp = (RawTrackDataHD *)malloc(sizeof(RawTrackDataHD));
    if (!tmp)
    {
        priv->lastError = DIAGNOSTIC_RESPONSE_ERROR;
        return priv->lastError;
    }

    if (priv->isHDMode)
    {
        priv->lastCommand = LAST_COMMAND_READ_TRACK_STREAM;

        priv->lastError = run_command(priv, COMMAND_READTRACKSTREAM, '\0', NULL);
        // Allow command retry
        if (priv->lastError != DIAGNOSTIC_RESPONSE_OK)
        {
            // Clear the buffer
            priv->lastError = run_command(priv, COMMAND_READTRACKSTREAM, '\0', NULL);
            if (priv->lastError != DIAGNOSTIC_RESPONSE_OK)
            {
                free(tmp);
                return priv->lastError;
            }
        }

        // Number of times we failed to read anything
        int readFail = 0;

        // Buffer to read into
        unsigned char tempReadBuffer[64] = {0};

        // Sliding window for abort
        char slidingWindow[5] = {0, 0, 0, 0, 0};
        bool timeout = false;

        unsigned int writePosition = 0;
        priv->isStreaming = true;
        priv->abortStreaming = false;
        priv->abortSignalled = false;

        // We know what this is, but the A
        apply_comm_timeouts(priv, true);

        while (priv->isStreaming)
        {

            // More efficient to read several bytes in one go
            unsigned long bytesAvailable = serial_get_bytes_waiting(&priv->comPort);
            if (bytesAvailable < 1)
                bytesAvailable = 1;
            if (bytesAvailable > sizeof tempReadBuffer)
                bytesAvailable = sizeof tempReadBuffer;
            unsigned long bytesRead = serial_read(&priv->comPort, tempReadBuffer, priv->abortSignalled ? 1 : bytesAvailable);

            for (size_t a = 0; a < bytesRead; a++)
            {
                if (priv->abortSignalled)
                {
                    // Make space
                    for (int s = 0; s < 4; s++)
                        slidingWindow[s] = slidingWindow[s + 1];
                    // Append the new byte
                    slidingWindow[4] = tempReadBuffer[a];

                    // Watch the sliding window for the pattern we need
                    if (slidingWindow[0] == 'X' && slidingWindow[1] == 'Y' && slidingWindow[2] == 'Z' && slidingWindow[3] == SPECIAL_ABORT_CHAR && slidingWindow[4] == '1')
                    {
                        priv->isStreaming = false;
                        serial_purge_buffers(&priv->comPort);
                        priv->lastError = timeout ? DIAGNOSTIC_RESPONSE_NO_DISK_IN_DRIVE : DIAGNOSTIC_RESPONSE_OK;
                        apply_comm_timeouts(priv, false);
                        bytesRead = 0;
                    }
                }
                else
                {
                    // HD Mode
                    unsigned char tmp2;
                    unsigned char outputByte = 0;

                    for (int i = 6; i >= 0; i -= 2)
                    {
                        // Convert to other format
                        tmp2 = tempReadBuffer[a] >> i & 0x03;
                        if (tmp2 == 3)
                            tmp2 = 0;

                        outputByte <<= 2;
                        outputByte |= tmp2 + 1;
                    }

                    (*tmp)[writePosition] = outputByte;
                    writePosition++;
                    if (writePosition >= (size_t)dataLength)
                    {
                        arduino_interface_abort_read_streaming(interface);
                    }
                }
            }
            if (bytesRead < 1)
            {
                readFail++;
                if (readFail > 30)
                {
                    priv->abortStreaming = false; // force the 'abort' command to be sent
                    arduino_interface_abort_read_streaming(interface);
                    priv->lastError = DIAGNOSTIC_RESPONSE_READ_RESPONSE_FAILED;
                    priv->isStreaming = false;
                    free(tmp);
                    apply_comm_timeouts(priv, false);
                    // Force a check for disk
                    arduino_interface_check_for_disk(interface, true);
                    return priv->lastError;
                }
                else
                    sleep_ms(1);
            }
        }
    }
    else
    {

        priv->lastError = run_command(priv, COMMAND_READTRACK, '\0', NULL);

        // Allow command retry
        if (priv->lastError != DIAGNOSTIC_RESPONSE_OK)
        {
            // Clear the buffer
            device_read(priv, tmp, dataLength, false);
            priv->lastError = run_command(priv, COMMAND_READTRACK, '\0', NULL);
            if (priv->lastError != DIAGNOSTIC_RESPONSE_OK)
            {
                free(tmp);
                return priv->lastError;
            }
        }

        unsigned char signalPulse = readFromIndexPulse ? 1 : 0;
        if (!serial_write(&priv->comPort, &signalPulse, 1))
        {
            priv->lastError = DIAGNOSTIC_RESPONSE_SEND_PARAMETER_FAILED;
            free(tmp);
            return priv->lastError;
        }

        // Keep reading until he hit dataLength or a null byte is received
        int bytePos = 0;
        int readFail = 0;
        for (;;)
        {
            unsigned char value;
            if (device_read(priv, &value, 1, true))
            {
                if (value == 0)
                    break;
                else if (bytePos < dataLength)
                    (*tmp)[bytePos++] = value;
            }
            else
            {
                readFail++;
                if (readFail > 4)
                {
                    free(tmp);
                    priv->lastError = DIAGNOSTIC_RESPONSE_READ_RESPONSE_FAILED;
                    return priv->lastError;
                }
            }
        }
    }

    unpack(*tmp, (unsigned char *)trackData, dataLength);
    free(tmp);

    priv->lastError = DIAGNOSTIC_RESPONSE_OK;
    return priv->lastError;
}

void unpack(const unsigned char *data, unsigned char *output, const int maxLength)
{
    int pos = 0;
    size_t index = 0;
    int p2 = 0; // This should be 'bit' for clarity
    memset(output, 0, maxLength);
    while (pos < maxLength)
    {
        // Each byte contains four pairs of bits that identify an MFM sequence to be encoded

        for (int b = 6; b >= 0; b -= 2)
        {
            switch ((data[index] >> b) & 3)
            {
            case 0:
                // This can't happen, its invalid data but we account for 4 '0' bits
                writeBit(output, &pos, &p2, 0, maxLength); // Note: &pos, &p2
                writeBit(output, &pos, &p2, 0, maxLength);
                writeBit(output, &pos, &p2, 0, maxLength);
                writeBit(output, &pos, &p2, 0, maxLength);
                break;
            case 1: // This is an '01'
                writeBit(output, &pos, &p2, 0, maxLength);
                writeBit(output, &pos, &p2, 1, maxLength);
                break;
            case 2: // This is an '001'
                writeBit(output, &pos, &p2, 0, maxLength);
                writeBit(output, &pos, &p2, 0, maxLength);
                writeBit(output, &pos, &p2, 1, maxLength);
                break;
            case 3: // this is an '0001'
                writeBit(output, &pos, &p2, 0, maxLength);
                writeBit(output, &pos, &p2, 0, maxLength);
                writeBit(output, &pos, &p2, 0, maxLength);
                writeBit(output, &pos, &p2, 1, maxLength);
                break;
            }
        }
        index++;
        if (index >= (size_t)maxLength)
            return;
    }
    // There will be left-over data
}

void writeBit(unsigned char *output, int *pos, int *bit, int value, const int maxLength)
{
    if (*pos >= maxLength)
        return;

    output[*pos] <<= 1;
    output[*pos] |= value;
    (*bit)++;
    if (*bit >= 8)
    {
        (*pos)++;
        *bit = 0;
    }
}

int readBit(const unsigned char* buffer, const unsigned int maxLength, int* pos, int* bit) {
	if (*pos >= (int)maxLength) {
		(*bit)--;
		if (*bit < 0) {
			*bit = 7;
			(*pos)++;
		}
		return *bit & 1 ? 0 : 1;
	}

	int ret = buffer[*pos] >> *bit & 1;
	(*bit)--;
	if (*bit < 0) {
		*bit = 7;
		(*pos)++;
	}

	return ret;
}

bool arduino_interface_abort_read_streaming(ArduinoInterface *interface)
{
    ArduinoInterfacePrivate *priv = (ArduinoInterfacePrivate *)interface->private_data;

    if (!priv->isStreaming)
        return true;

    // Prevent two things doing this at once, and thus ending with two bytes being written
    // std::lock_guard<std::mutex> lock(m_protectAbort);

    if (!priv->abortStreaming)
    {
        // We know what this is, but the Arduino doesn't
        unsigned char command = SPECIAL_ABORT_CHAR;

        priv->abortSignalled = true;

        // Send a byte.  This triggers the 'abort' on the Arduino
        if (!serial_write(&priv->comPort, &command, 1))
        {
            return false;
        }
    }
    priv->abortStreaming = true;
    return true;
}

DiagnosticResponse arduino_interface_enable_writing(ArduinoInterface *interface, bool enable, bool reset)
{
    ArduinoInterfacePrivate *priv = (ArduinoInterfacePrivate *)interface->private_data;

    if (enable)
    {
        priv->lastCommand = LAST_COMMAND_ENABLE_WRITE;

        // Enable the device
        priv->lastError = run_command(priv, COMMAND_ENABLEWRITE, '\0', NULL);
        if (priv->lastError == DIAGNOSTIC_RESPONSE_ERROR)
        {
            priv->lastError = DIAGNOSTIC_RESPONSE_WRITE_PROTECTED;
            return priv->lastError;
        }
        if (priv->lastError != DIAGNOSTIC_RESPONSE_OK)
            return priv->lastError;
        priv->inWriteMode = true;

        // Reset?
        if (reset)
        {
            // And rewind to the first track
            priv->lastError = arduino_interface_find_track0(interface);
            if (priv->lastError != DIAGNOSTIC_RESPONSE_OK)
                return priv->lastError;

            // Lets know where we are
            return arduino_interface_select_surface(interface, DISK_SURFACE_UPPER);
        }
        priv->lastError = DIAGNOSTIC_RESPONSE_OK;
        return priv->lastError;
    }
    else
    {
        priv->lastCommand = LAST_COMMAND_DISABLE_MOTOR;

        // Disable the device
        priv->lastError = run_command(priv, COMMAND_DISABLE, '\0', NULL);
        if (priv->lastError != DIAGNOSTIC_RESPONSE_OK)
            return priv->lastError;

        priv->inWriteMode = false;

        return priv->lastError;
    }
}

DiagnosticResponse arduino_interface_write_current_track_precomp(ArduinoInterface *interface, const unsigned char *mfmData, unsigned short numBytes, bool writeFromIndexPulse, bool usePrecomp)
{
    ArduinoInterfacePrivate *priv = (ArduinoInterfacePrivate *)interface->private_data;

    priv->lastCommand = LAST_COMMAND_WRITE_TRACK;

    if (priv->isHDMode)
        return write_current_track_hd(priv, mfmData, numBytes, writeFromIndexPulse);

    // First step is we need to re-encode the supplied buffer into a packed format with precomp pre-calculated.
    // Each nybble looks like: xxyy
    // where xx is: 0=no precomp, 1=-ve, 2=+ve
    //		 yy is: 0: 4us,		1: 6us,		2: 8us,		3: 10us

    // *4 is a worse case situation, ie: if everything was a 01.  The +128 is for any extra padding
    const unsigned int maxOutSize = numBytes * 4 + 16;
    unsigned char *outputBuffer = (unsigned char *)malloc(maxOutSize);

    if (!outputBuffer)
    {
        priv->lastError = DIAGNOSTIC_RESPONSE_SEND_PARAMETER_FAILED;
        return priv->lastError;
    }

    // Original data was written from MSB downto LSB
    int pos = 0;
    int bit = 7;
    unsigned int outputPos = 0;
    unsigned char sequence = 0xAA; // start at 10101010
    unsigned char *output = outputBuffer;
    int lastCount = 2;

    // Re-encode the data into our format and apply precomp.  The +8 is to ensure there's some padding around the edge which will come out as 010101 etc
    while (pos < numBytes)
    {
        *output = 0;

        for (int i = 0; i < 2; i++)
        {
            int b, count = 0;

            // See how many zero bits there are before we hit a 1
            do
            {
                b = readBit(mfmData, numBytes, &pos, &bit);
                sequence = ((sequence << 1) & 0x7F) | b;
                count++;
            } while ((sequence & 0x08) == 0 && pos < numBytes + 8);

            // Validate range
            if (count < 2)
                count = 2; // <2 would be a 11 sequence, not allowed
            if (count > 5)
                count = 5; // max we support 01, 001, 0001, 00001

            // Write to stream. Based on the rules above we apply some precomp
            int precomp = 0;
            if (usePrecomp)
            {
                const unsigned char BitSeq5 = (sequence & 0x3E); // extract these bits 00111110 - bit 3 is the ACTUAL bit
                // The actual idea is that the magnetic fields will repel each other, so we push them closer hoping they will land in the correct spot!
                switch (BitSeq5)
                {
                case 0x28: // xx10100x
                    precomp = PRECOMP_ERLY;
                    break;
                case 0x0A: // xx00101x
                    precomp = PRECOMP_LATE;
                    break;
                default:
                    precomp = PRECOMP_NONE;
                    break;
                }
            }
            else
                precomp = PRECOMP_NONE;

            *output |= ((lastCount - 2) | precomp) << (i * 4);
            lastCount = count;
        }

        output++;
        outputPos++;
        if (outputPos >= maxOutSize)
        {
            // should never happen
            free(outputBuffer);
            priv->lastError = DIAGNOSTIC_RESPONSE_SEND_PARAMETER_FAILED;
            return priv->lastError;
        }
    }

    priv->lastError = internal_write_track(priv, outputBuffer, outputPos, writeFromIndexPulse, true);

    free(outputBuffer);

    return priv->lastError;
}

static DiagnosticResponse internal_write_track(ArduinoInterfacePrivate *priv, const unsigned char *data, int numBytes, bool writeFromIndexPulse, bool usePrecomp)
{
    priv->lastCommand = LAST_COMMAND_WRITE_TRACK;

    priv->lastError = run_command(priv, priv->isHDMode ? COMMAND_WRITETRACK : usePrecomp ? COMMAND_WRITETRACKPRECOMP
                                                                                         : COMMAND_WRITETRACK,
                                  '\0', NULL);
    if (priv->lastError != DIAGNOSTIC_RESPONSE_OK)
        return priv->lastError;

    unsigned char chr;
    if (!device_read(priv, &chr, 1, true))
    {
        priv->lastError = DIAGNOSTIC_RESPONSE_READ_RESPONSE_FAILED;
        return priv->lastError;
    }

    // 'N' means NO Writing, aka write protected
    if (chr == 'N')
    {
        priv->lastError = DIAGNOSTIC_RESPONSE_WRITE_PROTECTED;
        return priv->lastError;
    }
    if (chr != 'Y')
    {
        priv->lastError = DIAGNOSTIC_RESPONSE_STATUS_ERROR;
        return priv->lastError;
    }

    // HD doesn't need the length as the data is null terminated
    if (!priv->isHDMode)
    {
        // Now we send the number of bytes we're planning on transmitting
        unsigned char b = numBytes >> 8;
        if (!serial_write(&priv->comPort, &b, 1))
        {
            priv->lastError = DIAGNOSTIC_RESPONSE_SEND_PARAMETER_FAILED;
            return priv->lastError;
        }
        b = numBytes & 0xFF;
        if (!serial_write(&priv->comPort, &b, 1))
        {
            priv->lastError = DIAGNOSTIC_RESPONSE_SEND_PARAMETER_FAILED;
            return priv->lastError;
        }
    }

    // Explain if we want index pulse sync writing (slower and not required by normal AmigaDOS disks)
    unsigned char b = writeFromIndexPulse ? 1 : 0;
    if (!serial_write(&priv->comPort, &b, 1))
    {
        priv->lastError = DIAGNOSTIC_RESPONSE_SEND_PARAMETER_FAILED;
        return priv->lastError;
    }

    unsigned char response;
    if (!device_read(priv, &response, 1, true))
    {
        priv->lastError = DIAGNOSTIC_RESPONSE_READ_RESPONSE_FAILED;
        return priv->lastError;
    }

    if (response != '!')
    {
        priv->lastError = DIAGNOSTIC_RESPONSE_STATUS_ERROR;

        return priv->lastError;
    }

    if (!serial_write(&priv->comPort, (const void *)data, numBytes))
    {
        priv->lastError = DIAGNOSTIC_RESPONSE_SEND_DATA_FAILED;
        return priv->lastError;
    }

    if (!device_read(priv, &response, 1, true))
    {
        priv->lastError = DIAGNOSTIC_RESPONSE_TRACK_WRITE_RESPONSE_ERROR;
        return priv->lastError;
    }

    // If this is a '1' then the Arduino didn't miss a single bit!
    if (response != '1')
    {
        switch (response)
        {
        case 'X':
            priv->lastError = DIAGNOSTIC_RESPONSE_WRITE_TIMEOUT;
            break;
        case 'Y':
            priv->lastError = DIAGNOSTIC_RESPONSE_FRAMING_ERROR;
            break;
        case 'Z':
            priv->lastError = DIAGNOSTIC_RESPONSE_SERIAL_OVERRUN;
            break;
        default:
            priv->lastError = DIAGNOSTIC_RESPONSE_STATUS_ERROR;
            break;
        }
        return priv->lastError;
    }

    priv->lastError = DIAGNOSTIC_RESPONSE_OK;
    return priv->lastError;
}

static DiagnosticResponse write_current_track_hd(ArduinoInterfacePrivate *priv, const unsigned char *mfmData, unsigned short numBytes, bool writeFromIndexPulse)
{
    priv->lastCommand = LAST_COMMAND_WRITE_TRACK;

    // First step is we need to re-encode the supplied buffer into a packed format
    // Each nybble looks like: wwxxyyzz, each group is a code which is the transition time

    // *4 is a worse case situation, ie: if everything was a 01.  The +128 is for any extra padding
    const unsigned int maxOutSize = numBytes * 4 + 16;
    unsigned char *outputBuffer = (unsigned char *)malloc(maxOutSize);

    if (!outputBuffer)
    {
        priv->lastError = DIAGNOSTIC_RESPONSE_SEND_PARAMETER_FAILED;
        return priv->lastError;
    }

    // Original data was written from MSB down to LSB
    int pos = 0;
    int bit = 7;
    unsigned int outputPos = 0;
    unsigned char sequence = 0xAA; // start at 10101010
    unsigned char *output = outputBuffer;

    // Re-encode the data into our format.
    while (pos < numBytes)
    {
        *output = 0;

        for (int i = 0; i < 4; i++)
        {
            int b, count = 0;

            // See how many zero bits there are before we hit a 1
            do
            {
                b = readBit(mfmData, numBytes, &pos, &bit);
                sequence = ((sequence << 1) & 0x7F) | b;
                count++;
            } while ((sequence & 0x08) == 0 && pos < numBytes + 8);

            // Validate range
            if (count < 2)
                count = 2; // <2 would be a 11 sequence, not allowed
            if (count > 4)
                count = 4; // max we support 01, 001 and 0001
            switch (i)
            {
            case 1:
            case 3:
                *output |= (count - 1) << (i * 2);
                break;
            case 0:
                *output |= (count - 1) << (2 * 2);
                break;
            case 2:
                *output |= (count - 1) << (0 * 2);
                break;
            }
        }

        output++;
        outputPos++;
        if (outputPos >= maxOutSize - 1)
        {
            // should never happen
            free(outputBuffer);
            priv->lastError = DIAGNOSTIC_RESPONSE_SEND_PARAMETER_FAILED;
            return priv->lastError;
        }
    }

    // 0 means stop
    *output = 0;
    outputPos++;

    priv->lastError = internal_write_track(priv, outputBuffer, outputPos, writeFromIndexPulse, false);

    free(outputBuffer);

    return priv->lastError;
}
