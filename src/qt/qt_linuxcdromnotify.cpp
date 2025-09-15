/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Linux CD-ROM change notification implementation for QT
 *
 *
 *
 * Authors: GitHub Copilot
 *
 *          Copyright 2025
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#ifdef __linux__

#include "qt_linuxcdromnotify.hpp"
#include "qt_mediamenu.hpp"

#include <QDebug>
#include <QDir>
#include <QFileInfo>

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/cdrom.h>

extern "C" {
#include <86box/cdrom.h>
#include <86box/plat.h>
}

LinuxCDROMNotify::LinuxCDROMNotify(MainWindow *window)
    : QObject(window)
    , window(window)
    , inotify_fd(-1)
    , notifier(nullptr)
    , periodic_timer(nullptr)
{
    // Initialize inotify
    inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inotify_fd == -1) {
        qWarning() << "LinuxCDROMNotify: Failed to initialize inotify:" << strerror(errno);
        return;
    }

    // Set up socket notifier for inotify events
    notifier = new QSocketNotifier(inotify_fd, QSocketNotifier::Read, this);
    connect(notifier, &QSocketNotifier::activated, this, &LinuxCDROMNotify::onInotifyEvent);

    // Set up monitoring for existing CD-ROM devices
    setupCDROMMonitoring();

    qDebug() << "LinuxCDROMNotify: Initialized successfully";
}

LinuxCDROMNotify::~LinuxCDROMNotify()
{
    if (inotify_fd != -1) {
        for (const auto &device : monitored_devices) {
            if (device.watch_fd != -1) {
                inotify_rm_watch(inotify_fd, device.watch_fd);
            }
        }
        close(inotify_fd);
    }
}

std::unique_ptr<LinuxCDROMNotify> LinuxCDROMNotify::Register(MainWindow *window)
{
    return std::unique_ptr<LinuxCDROMNotify>(new LinuxCDROMNotify(window));
}

void LinuxCDROMNotify::setupCDROMMonitoring()
{
    monitored_devices.clear();
    for (int i = 0; i < CDROM_NUM; i++) {
        if (cdrom[i].bus_type != 0) {  // Drive is configured
            QString device_path = QString::fromLocal8Bit(cdrom[i].ioctl_dev_path);
            if (!device_path.isEmpty()) {
                addCDROMDevice(device_path, i);
            }
        }
    }
}

void LinuxCDROMNotify::onInotifyEvent()
{
    char buffer[4096];
    ssize_t length = read(inotify_fd, buffer, sizeof(buffer));

    if (length == -1) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            qWarning() << "LinuxCDROMNotify: Error reading inotify events:" << strerror(errno);
        }
        return;
    }

    qDebug() << "LinuxCDROMNotify: inotify event received, checking CD-ROM changes";

    // Process all inotify events
    ssize_t i = 0;
    while (i < length) {
        struct inotify_event *event = (struct inotify_event *) &buffer[i];
        
        // Find which device this event corresponds to
        for (const auto &device : monitored_devices) {
            if (device.watch_fd == event->wd) {
                qDebug() << "LinuxCDROMNotify: Event for" << device.path;
                processCDROMChange(device.path, device.cdrom_id);
                break;
            }
        }

        i += sizeof(struct inotify_event) + event->len;
    }
}

void LinuxCDROMNotify::processCDROMChange(const QString &path, int cdrom_id)
{
    if (cdrom_id < 0 || cdrom_id >= CDROM_NUM) {
        return;
    }

    cdrom_t *dev = &cdrom[cdrom_id];
    if (dev->bus_type == 0) {
        return;  // Drive not configured
    }

    int fd = open(path.toLocal8Bit().constData(), O_RDONLY | O_NONBLOCK);
    if (fd == -1) {
        qDebug() << "LinuxCDROMNotify: Failed to open device" << path;
        return;
    }

    int status = ioctl(fd, CDROM_DRIVE_STATUS, CDSL_CURRENT);
    
    if (status == CDS_TRAY_OPEN){
        qDebug() << "LinuxCDROMNotify: Tray open for CD-ROM" << cdrom_id;
        close(fd);
        MediaMenu::ptr->cdromEject(cdrom_id);
        return;
    }

    if (status == CDS_DISC_OK) {
        qDebug() << "LinuxCDROMNotify: Disc present for CD-ROM" << cdrom_id;
        close(fd);
        QString device_path = QString::fromLocal8Bit(dev->image_path);
        if (device_path.isEmpty()) {
            qDebug() << "LinuxCDROMNotify: Mounting CD-ROM" << cdrom_id;
            MediaMenu::ptr->cdromMount(cdrom_id, path);
            return;
        }
    }

    switch (status) {
        case CDS_NO_INFO:       qDebug() << "LinuxCDROMNotify: No info for CD-ROM" << cdrom_id; break;
        case CDS_NO_DISC:       qDebug() << "LinuxCDROMNotify: No disc in drive for CD-ROM" << cdrom_id; break;
        case CDS_TRAY_OPEN:     qDebug() << "LinuxCDROMNotify: Tray open for CD-ROM" << cdrom_id; break;
        case CDS_DRIVE_NOT_READY: qDebug() << "LinuxCDROMNotify: Drive not ready for CD-ROM" << cdrom_id; break;
        case CDS_DISC_OK:       qDebug() << "LinuxCDROMNotify: Disc present for CD-ROM" << cdrom_id; break;
        default:                qDebug() << "LinuxCDROMNotify: Unknown status for CD-ROM" << cdrom_id; break;
    }
}

bool LinuxCDROMNotify::addCDROMDevice(const QString &path, int cdrom_id)
{
    // Check if already monitoring this device
    for (const auto &device : monitored_devices) {
        if (device.path == path) {
            return true;  // Already monitoring
        }
    }

    // Add new device to monitoring
    CDROMDevice device;
    device.path = path;
    device.watch_fd = inotify_add_watch(inotify_fd, path.toLocal8Bit().constData(), IN_ATTRIB);
    device.last_check = 0;
    device.last_capacity = 0;
    device.last_device_id = 0;
    device.cdrom_id = cdrom_id;

    if (device.watch_fd != -1) {
        monitored_devices.append(device);
        qDebug() << "LinuxCDROMNotify: Added monitoring for" << path;
        return true;
    } else {
        qWarning() << "LinuxCDROMNotify: Failed to add inotify watch for" << path << ":" << strerror(errno);
        return false;
    }
}

#endif // __linux__
