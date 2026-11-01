#ifdef USE_SDL2_LIB
#include <SDL.h>
#else
#include <SDL3/SDL.h>
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>

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

void
ui_progress_wait(UNUSED(const char *message), int total, int (*poll)(void *arg), void *arg)
{
    while (poll(arg) < total)
        SDL_Delay(50);
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

    /* Yes returns 1, No 0, as the default. */
    SDL_MessageBoxButtonData yesno[2];
    if (flags & MBX_QUESTION_YN) {
        memset(yesno, 0, sizeof(yesno));
#ifdef USE_SDL2_LIB
        yesno[0].buttonid = 0;
        yesno[1].buttonid = 1;
#else
        yesno[0].buttonID = 0;
        yesno[1].buttonID = 1;
#endif
        yesno[0].text      = "No";
        yesno[0].flags     = SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT;
        yesno[1].text      = "Yes";
        msgdata.numbuttons = 2;
        msgdata.buttons    = yesno;
    }
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

/* snprintf returns the length it would have written, even when truncated. */
static void
ui_msg_append(char *message, size_t size, size_t *len, const char *format, ...)
{
    va_list args;
    int     written;

    if (*len >= size - 1)
        return;

    va_start(args, format);
    written = vsnprintf(message + *len, size - *len, format, args);
    va_end(args);

    if (written > 0)
        *len = ((size_t) written < size - *len) ? *len + written : size - 1;
}

int
ui_confirm_unsupported_hardware(const ui_unsupported_hardware_t *items, int count, int machine_missing)
{
    char   message[2048];
    size_t len = 0;

    if (count <= 0)
        return 1;

    message[0] = '\0';
    ui_msg_append(message, sizeof(message), &len, "%s\n\n", plat_get_string(STRING_UNSUPPORTED_TEXT));
    /* Show six names in full; for larger lists show five and the remainder. */
    const int listed = count <= 6 ? count : 5;
    for (int i = 0; i < listed; i++) {
        ui_msg_append(message, sizeof(message), &len, plat_get_string(items[i].kind), items[i].name);
        ui_msg_append(message, sizeof(message), &len, "\n");
    }
    if (count > listed) {
        ui_msg_append(message, sizeof(message), &len, plat_get_string(STRING_UNSUPPORTED_OTHERS), count - listed);
        ui_msg_append(message, sizeof(message), &len, "\n");
    }

    const int explanation = !machine_missing ? STRING_UNSUPPORTED_REMOVE :
                            count > 1 ? STRING_UNSUPPORTED_REPLACE_REMOVE : STRING_UNSUPPORTED_REPLACE;
    ui_msg_append(message, sizeof(message), &len, "\n%s\n\n%s", plat_get_string(explanation),
                  plat_get_string(STRING_UNSUPPORTED_CONTINUE));

    return ui_msgbox_header(MBX_WARNING | MBX_QUESTION_YN, plat_get_string(STRING_UNSUPPORTED_TITLE), message) == 1;
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
