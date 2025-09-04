//
// Created by nacho on 1/9/25.
//

#include <QString>
#include <QMetaObject>
#include <../include/qt_wrapper.h>
#include <qt_mediamenu.hpp>

extern "C" {
    int media_mount_floppy(int drive, const char* path) {
        // Use the static pointer to access the MediaMenu instance
        if (MediaMenu::ptr) {
            // Use Qt::BlockingQueuedConnection to wait for the operation to complete
            QMetaObject::invokeMethod(MediaMenu::ptr.get(), "floppyMount",
                Qt::BlockingQueuedConnection,
                Q_ARG(int, drive),
                Q_ARG(QString, QString(path)),
                Q_ARG(bool, false));

            return 0;
        }
        return -1; // Error: MediaMenu not initialized
    }

    int media_unmount_floppy(int drive) {
        // Use the static pointer to access the MediaMenu instance
        if (MediaMenu::ptr) {
            QMetaObject::invokeMethod(MediaMenu::ptr.get(), "floppyEject",
                Qt::BlockingQueuedConnection,
                Q_ARG(int, drive));
            return 0; // Success
        }
        return -1; // Error: MediaMenu not initialized
    }
}