#include <stdint.h>

#include "action.h"
#include "appevents.h"
#include "audio.h"
#include "config.h"
#include "iap.h"
#include "kernel.h"
#include "lang.h"
#include "metadata.h"
#include "misc.h"
#include "screen_access.h"
#include "settings.h"
#include "viewport.h"

static void iap_screen_draw(void)
{
    const unsigned char *title = str(LANG_ACCESSORY_CONNECTED);
    const unsigned char *subtitle = str(LANG_OK_TO_DISCONNECT);

    FOR_NB_SCREENS(i)
    {
        struct screen *screen = &screens[i];
        int title_w, title_h, subtitle_w, subtitle_h;
        int y;

        screen->set_viewport(NULL);
        screen->clear_display();
        screen->getstringsize(title, &title_w, &title_h);
        screen->getstringsize(subtitle, &subtitle_w, &subtitle_h);
        y = MAX(0, (screen->getheight() - title_h - subtitle_h) / 2);
        screen->putsxy(MAX(0, (screen->getwidth() - title_w) / 2), y, title);
        screen->putsxy(MAX(0, (screen->getwidth() - subtitle_w) / 2),
                       y + title_h, subtitle);
        screen->update();
    }
}

static void iap_screen_finish_seek(bool *seeking, long position)
{
    if (*seeking)
    {
        audio_ff_rewind(position);
        *seeking = false;
    }
}

static void iap_screen_seek(bool *seeking, long *position, int direction)
{
    struct mp3entry *id3 = audio_current_track();
    long step = global_settings.ff_rewind_min_step * 1000L;

    if (!id3 || !(audio_status() & AUDIO_STATUS_PLAY))
        return;

    if (!*seeking)
    {
        audio_pre_ff_rewind();
        *position = id3->elapsed;
        *seeking = true;
    }

    if (step < 1000)
        step = 1000;
    *position += direction * step;
    if (*position < 0)
        *position = 0;
    if ((unsigned long)*position > id3->length)
        *position = id3->length;
}

void gui_iap_screen_run(void)
{
    bool seeking = false;
    long seek_position = 0;

    if (!iap_remote_ui_active())
        return;

    push_current_activity(ACTIVITY_IAPSCREEN);
    FOR_NB_SCREENS(i)
    {
        viewportmanager_theme_enable(i, false, NULL);
        screens[i].scroll_stop();
        screens[i].backlight_on();
    }
    iap_screen_draw();

    while (iap_remote_ui_active())
    {
        int action = get_action(CONTEXT_IAP, HZ / 5);

        if (seeking && action != ACTION_WPS_SEEKFWD &&
            action != ACTION_WPS_SEEKBACK && action != ACTION_WPS_STOPSEEK &&
            action != ACTION_NONE)
            iap_screen_finish_seek(&seeking, seek_position);

        switch (action)
        {
            case ACTION_WPS_PLAY:
            {
                int status = audio_status();

                if (!(status & AUDIO_STATUS_PLAY))
                    iap_play_or_resume();
                else if (status & AUDIO_STATUS_PAUSE)
                    audio_resume();
                else
                    audio_pause();
                break;
            }

            case ACTION_WPS_VOLUP:
                adjust_volume(1);
                setvol();
                break;

            case ACTION_WPS_VOLDOWN:
                adjust_volume(-1);
                setvol();
                break;

            case ACTION_WPS_SKIPNEXT:
                audio_next();
                break;

            case ACTION_WPS_SKIPPREV:
                audio_prev();
                break;

            case ACTION_WPS_SEEKFWD:
                iap_screen_seek(&seeking, &seek_position, 1);
                break;

            case ACTION_WPS_SEEKBACK:
                iap_screen_seek(&seeking, &seek_position, -1);
                break;

            case ACTION_WPS_STOPSEEK:
                iap_screen_finish_seek(&seeking, seek_position);
                break;

            case ACTION_WPS_STOP:
                audio_stop();
                break;

            case SYS_IAP_UI_EXIT:
                goto done;

            case SYS_IAP_UI_ENTER:
            case ACTION_NONE:
                break;

            case ACTION_REDRAW:
                iap_screen_draw();
                break;

            default:
                if (IS_SYSEVENT(action))
                {
                    default_event_handler(action);
                    iap_screen_draw();
                }
                break;
        }
    }

done:
    iap_screen_finish_seek(&seeking, seek_position);
    FOR_NB_SCREENS(i)
        viewportmanager_theme_undo(i, false);
    pop_current_activity();
    send_event(GUI_EVENT_ACTIONUPDATE, NULL);
}
