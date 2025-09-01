//
// Created by nacho on 1/9/25.
//
#ifdef __cplusplus
extern "C" {
#endif

#ifndef INC_86BOX_QT_WRAPPER_H
#define INC_86BOX_QT_WRAPPER_H

extern int media_mount_floppy(int drive, const char* path);
extern int media_unmount_floppy(int drive);

#endif //INC_86BOX_QT_WRAPPER_H
#ifdef __cplusplus
}
#endif