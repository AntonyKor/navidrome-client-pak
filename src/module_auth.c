#include <stdio.h>
#include <string.h>

#include "api.h"
#include "nd_log.h"
#include "defines.h"
#include "module_auth.h"
#include "module_common.h"
#include "nd_api.h"
#include "nd_config.h"
#include "nd_models.h"
#include "ui_fonts.h"
#include "ui_keyboard.h"
#include "ui_utils.h"

/* =========================================================================
 * Login form: Server URL / Username / Password / Connect
 * ========================================================================= */

enum {
    FIELD_SERVER = 0,
    FIELD_USER,
    FIELD_PASS,
    FIELD_CONNECT,
    FIELD_COUNT
};

static void render_loading_screen(SDL_Surface *screen, const char *msg)
{
    SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 0x12, 0x12, 0x12));
    SDL_Surface *surf = TTF_RenderUTF8_Blended(Fonts_getMedium(), msg, COLOR_WHITE);
    if (surf) {
        SDL_BlitSurface(surf, NULL, screen,
                        &(SDL_Rect){(screen->w - surf->w) / 2, (screen->h - surf->h) / 2});
        SDL_FreeSurface(surf);
    }
    GFX_flip(screen);
}

static void render_form(SDL_Surface *screen, int show_setting, int selected,
                        const NdConfig *form, const char *error_msg)
{
    SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 0x12, 0x12, 0x12));
    render_screen_header(screen, "Sign in to Navidrome", show_setting);

    ListLayout layout = calc_list_layout(screen);

    char pass_mask[64];
    int plen = (int)strlen(form->password);
    if (plen > 20) plen = 20;
    for (int i = 0; i < plen; i++) pass_mask[i] = '*';
    pass_mask[plen] = '\0';

    char labels[FIELD_COUNT][ND_MAX_URL + 32];
    snprintf(labels[FIELD_SERVER], sizeof(labels[0]), "Server: %s",
             form->server_url[0] ? form->server_url : "(not set)");
    snprintf(labels[FIELD_USER], sizeof(labels[0]), "Username: %s",
             form->username[0] ? form->username : "(not set)");
    snprintf(labels[FIELD_PASS], sizeof(labels[0]), "Password: %s",
             form->password[0] ? pass_mask : "(not set)");
    snprintf(labels[FIELD_CONNECT], sizeof(labels[0]), "Connect");

    for (int i = 0; i < FIELD_COUNT; i++) {
        bool sel = (i == selected);
        char truncated[ND_MAX_URL + 32];
        MenuItemPos pos = render_menu_item_pill(screen, &layout, labels[i], truncated,
                                                i, sel, 0);
        render_list_item_text(screen, NULL, truncated, Fonts_getMedium(),
                              pos.text_x, pos.text_y,
                              layout.max_width - SCALE1(BUTTON_PADDING * 2), sel);
    }

    int y = layout.list_y + FIELD_COUNT * layout.item_h + SCALE1(PADDING);
    if (error_msg && *error_msg) {
        SDL_Surface *err = TTF_RenderUTF8_Blended(Fonts_getSmall(), error_msg,
                                                  (SDL_Color){0xE5, 0x45, 0x45, 0xFF});
        if (err) {
            SDL_BlitSurface(err, NULL, screen, &(SDL_Rect){SCALE1(PADDING * 2), y});
            SDL_FreeSurface(err);
        }
    } else {
        SDL_Surface *hint = TTF_RenderUTF8_Blended(Fonts_getSmall(),
            "Example: http://192.168.1.10:4533", COLOR_LIGHT_TEXT);
        if (hint) {
            SDL_BlitSurface(hint, NULL, screen, &(SDL_Rect){SCALE1(PADDING * 2), y});
            SDL_FreeSurface(hint);
        }
    }

    GFX_blitButtonGroup((char*[]){"A", selected == FIELD_CONNECT ? "CONNECT" : "EDIT",
                                  "B", nd_config_is_valid(nd_config_get_mutable()) ? "BACK" : "QUIT", NULL},
                        1, screen, 1);
}

/* Normalize the server URL: add http:// if no scheme, drop trailing slashes. */
static void normalize_url(char *url, int size)
{
    /* trim spaces */
    char *start = url;
    while (*start == ' ') start++;
    if (start != url) memmove(url, start, strlen(start) + 1);
    int len = (int)strlen(url);
    while (len > 0 && (url[len - 1] == ' ' || url[len - 1] == '/')) url[--len] = '\0';
    if (len == 0) return;

    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        char tmp[ND_MAX_URL];
        snprintf(tmp, sizeof(tmp), "http://%s", url);
        strncpy(url, tmp, size - 1);
        url[size - 1] = '\0';
    }
}

/* =========================================================================
 * Public entry point
 * ========================================================================= */

AppModule module_auth_run(SDL_Surface *screen)
{
    NdConfig *cfg = nd_config_get_mutable();

    /* Work on a copy so a failed attempt doesn't clobber the saved config. */
    NdConfig form = *cfg;
    if (form.server_url[0] == '\0') strcpy(form.server_url, "http://");

    int selected = !form.username[0] ? FIELD_SERVER
                 : !form.password[0] ? FIELD_PASS   /* e.g. after Sign Out */
                 : FIELD_CONNECT;
    char error_msg[160] = "";
    int dirty = 1;
    int show_setting = 0;

    while (1) {
        GFX_startFrame();
        PAD_poll();
        SDL_Delay(16);

        ModuleCommon_PWR_update(&dirty, &show_setting);

        GlobalInputResult global = ModuleCommon_handleGlobalInput(screen, &show_setting, 0);
        if (global.should_quit) return MODULE_QUIT;
        if (global.input_consumed) { GFX_sync(); continue; }

        if (PAD_justRepeated(BTN_UP)) {
            selected = (selected + FIELD_COUNT - 1) % FIELD_COUNT;
            dirty = 1;
        } else if (PAD_justRepeated(BTN_DOWN)) {
            selected = (selected + 1) % FIELD_COUNT;
            dirty = 1;
        } else if (PAD_justPressed(BTN_B)) {
            /* Already signed in (came here from Settings): go back */
            return nd_config_is_valid(cfg) ? MODULE_BROWSE : MODULE_QUIT;
        } else if (PAD_justPressed(BTN_A)) {
            switch (selected) {
            case FIELD_SERVER:
                if (UIKeyboard_run(screen, "Server URL", form.server_url,
                                   sizeof(form.server_url), false))
                    selected = FIELD_USER;
                break;
            case FIELD_USER:
                if (UIKeyboard_run(screen, "Username", form.username,
                                   sizeof(form.username), false))
                    selected = FIELD_PASS;
                break;
            case FIELD_PASS:
                if (UIKeyboard_run(screen, "Password", form.password,
                                   sizeof(form.password), true))
                    selected = FIELD_CONNECT;
                break;
            case FIELD_CONNECT: {
                normalize_url(form.server_url, sizeof(form.server_url));
                if (strlen(form.server_url) <= 8 || form.username[0] == '\0') {
                    snprintf(error_msg, sizeof(error_msg),
                             "Enter a server URL and username first");
                    break;
                }
                render_loading_screen(screen, "Connecting...");
                char err[128] = "";
                if (nd_api_ping(&form, err, sizeof(err)) != 0) {
                    snprintf(error_msg, sizeof(error_msg), "%s",
                             err[0] ? err : "Connection failed");
                    ND_LOG_ERROR("[Auth] ping failed: %s\n", error_msg);
                    break;
                }
                /* Signed in: keep user settings, store credentials.
                 * A different server has different music folders. */
                if (strcmp(cfg->server_url, form.server_url) != 0) {
                    cfg->library_id      = 0;
                    cfg->library_name[0] = '\0';
                }
                strncpy(cfg->server_url, form.server_url, sizeof(cfg->server_url) - 1);
                strncpy(cfg->username, form.username, sizeof(cfg->username) - 1);
                strncpy(cfg->password, form.password, sizeof(cfg->password) - 1);
                cfg->server_url[sizeof(cfg->server_url) - 1] = '\0';
                cfg->username[sizeof(cfg->username) - 1]     = '\0';
                cfg->password[sizeof(cfg->password) - 1]     = '\0';
                snprintf(cfg->server_name, sizeof(cfg->server_name), "%s",
                         strstr(form.server_url, "://") ? strstr(form.server_url, "://") + 3
                                                        : form.server_url);
                cfg->offline_mode = false;
                if (cfg->preload_count == 0) cfg->preload_count = 2;
                if (cfg->screen_timeout == 0) cfg->screen_timeout = 30;
                nd_config_save(cfg);
                ND_LOG("[Auth] Signed in to %s as %s\n", cfg->server_url, cfg->username);
                return MODULE_BROWSE;
            }
            }
            dirty = 1;
        }

        if (dirty) {
            render_form(screen, show_setting, selected, &form, error_msg);
            GFX_flip(screen);
            dirty = 0;
        } else {
            GFX_sync();
        }
    }
}
