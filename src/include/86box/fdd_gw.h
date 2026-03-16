/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Definitions for the GreaseWeazle USB floppy controller backend.
 *
 * Authors: <YOUR_NAME>
 *
 *          Copyright 2026 86Box contributors.
 */
#ifndef EMU_FLOPPY_GW_H
#define EMU_FLOPPY_GW_H

#ifdef __cplusplus
extern "C" {
#endif

extern void gw_load(int drive, char *fn);
extern void gw_close(int drive);
extern void gw_set_fdc(void *fdc);
extern int  gw_detect_devices(char devices[][256], int max_devices);

#ifdef __cplusplus
}
#endif

#endif /* EMU_FLOPPY_GW_H */
