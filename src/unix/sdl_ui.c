#ifdef USE_SDL2_LIB
#include <SDL.h>
#else
#include <SDL3/SDL.h>
#endif

#include <stdbool.h>
#include <stdint.h>

#include <86box/86box.h>
#include <86box/plat.h>
#include <86box/timer.h>
#include <86box/device.h>
#include <86box/fdd.h>
#include <86box/scsi.h>
#include <86box/scsi_device.h>
#include <86box/cdrom.h>
#include <86box/rdisk.h>
#include <86box/mo.h>
#include <86box/scsi_tape.h>
#include <86box/hdd.h>
#include <86box/thread.h>
#include <86box/network.h>
#include <86box/machine_status.h>
#include <86box/gpio.h>
#include <86box/ui.h>
#include <86box/version.h>

#include "sdl_osd.h"

int
ui_msgbox(int flags, char *message)
{
    return ui_msgbox_header(flags, NULL, message);
}

int
ui_msgbox_header(int flags, char *header, char *message)
{
    SDL_MessageBoxData       msgdata;
    SDL_MessageBoxButtonData msgbtn;

    if (!header) {
        if (flags & MBX_FATAL)
            header = "Fatal error";
        else if (flags & MBX_ERROR)
            header = "Error";
        else
            header = EMU_NAME;
    }

#ifdef USE_SDL2_LIB
    msgbtn.buttonid = 1;
#else
    msgbtn.buttonID = 1;
#endif
    msgbtn.text     = "OK";
    msgbtn.flags    = 0;
    memset(&msgdata, 0, sizeof(SDL_MessageBoxData));
    msgdata.numbuttons = 1;
    msgdata.buttons    = &msgbtn;
    int msgflags       = 0;
    if ((flags & MBX_ERROR) || (flags & MBX_FATAL))
        msgflags |= SDL_MESSAGEBOX_ERROR;
    else if (flags & MBX_WARNING)
        msgflags |= SDL_MESSAGEBOX_WARNING;
    else
        msgflags |= SDL_MESSAGEBOX_INFORMATION;
    msgdata.flags   = msgflags;
    int button      = 0;
    msgdata.title   = header;
    msgdata.message = message;
    SDL_ShowMessageBox(&msgdata, &button);
    return button;
}

void
ui_sb_update_icon_state(int tag, int state)
{
    osd_ui_sb_update_icon_state(tag, state);
}

void
ui_sb_update_icon(int tag, int active)
{
    /* The OSD does not track device state yet, so keep machine_status up to
       date here for the hard disks, which is what the GPIO activity LED is
       driven from. */
    if (((((unsigned int) tag) & 0xfffffff0) == SB_HDD) && ((tag & 0xf) < HDD_BUS_USB)) {
        machine_status.hdd[tag & 0xf].active = active > 0 ? true : false;
        gpio_hdd_activity();
    }

    osd_ui_sb_update_icon(tag, active);
}

void
ui_sb_update_icon_write(int tag, int active)
{
    if (((((unsigned int) tag) & 0xfffffff0) == SB_HDD) && ((tag & 0xf) < HDD_BUS_USB)) {
        machine_status.hdd[tag & 0xf].write_active = active > 0 ? true : false;
        gpio_hdd_activity();
    }

    osd_ui_sb_update_icon_write(tag, active);
}

void
ui_sb_update_icon_wp(int tag, int state)
{
    osd_ui_sb_update_icon_wp(tag, state);
}

/* The device code only ever raises the activity flags; clearing them again is
   the frontend's job, which the Qt status bar does from its 75 ms refresh
   timer. Nothing did it here, so the first disk access latched the HDD
   activity LED on for good. Runs on the emulation thread, the same one the
   device code raises the flags from. */
void
sdl_ui_activity_tick(void)
{
    static uint64_t last_tick = 0;
    const uint64_t  now       = SDL_GetTicks();

    if ((now - last_tick) < 75)
        return;
    last_tick = now;

    for (int i = 0; i < HDD_BUS_USB; i++) {
        if (machine_status.hdd[i].active)
            ui_sb_update_icon(SB_HDD | i, 0);
        if (machine_status.hdd[i].write_active)
            ui_sb_update_icon_write(SB_HDD | i, 0);
    }
}

void
ui_sb_update_tip(UNUSED(int arg))
{
    /* No-op. */
}

void
ui_sb_update_panes(void)
{
    /* No-op. */
}

void
ui_sb_update_text(void)
{
    /* No-op. */
}

void
ui_sb_set_text(UNUSED(char *wstr))
{
    /* No-op. */
}

void
ui_sb_bugui(UNUSED(char *str))
{
    /* No-op. */
}

void
ui_sb_set_ready(UNUSED(int ready))
{
    /* No-op. */
}

void
ui_sb_mt32lcd(UNUSED(char *str))
{
    /* No-op. */
}

extern void update_mouse_msg(void);
void
ui_hard_reset_completed(void)
{
    update_mouse_msg();
}

void
ui_update_force_interpreter(void)
{
    /* No-op. */
}
