#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <signal.h>
#include <msettings.h>

#include "api.h"
#include "utils.h"
#include "config.h"

#include "nd_log.h"
#include "nd_config.h"
#include "nd_net.h"
#include "nd_art.h"
#include "player.h"
#include "ui_fonts.h"
#include "module_auth.h"
#include "module_browse.h"
#include "module_player.h"
#include "module_settings.h"
#include "nd_queue.h"
#include "nd_downloads.h"
#include "nd_favorites.h"

/* Global quit flag, set by signal handler */
static volatile bool g_quit = false;

/* Global application config — accessible to future modules via nd_config_get() */
static NdConfig g_config;

/* Provide read-only access to the global config for other modules */
const NdConfig *nd_config_get(void)
{
    return &g_config;
}

/* Provide mutable access to the global config — used by module_auth after save */
NdConfig *nd_config_get_mutable(void)
{
    return &g_config;
}

static void sig_handler(int sig) {
    switch (sig) {
    case SIGINT:
    case SIGTERM:
        g_quit = true;
        break;
    default:
        break;
    }
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    nd_log_init();

    /* --- Platform / SDL init (mirror musicplayer.c sequence) --- */
    SDL_Surface *screen = GFX_init(MODE_MAIN);
    PWR_pinToCores(CPU_CORE_PERFORMANCE);

    InitSettings();
    PAD_init();
    PWR_init();
    PWR_setCPUSpeed(CPU_SPEED_POWERSAVE);
    PWR_disableAutosleep();
    WIFI_init();
    Fonts_load();
    nd_art_init();
    if (Player_init() != 0) {
        ND_LOG_ERROR("[Main] Player_init failed\n");
        nd_log_flush();
        return EXIT_FAILURE;
    }

    nd_downloads_init();

    nd_favorites_init();

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);      /* prevent crash when writing to dead sockets */

    /*
     * Load config from $SHARED_USERDATA_PATH/navidrome/config.json.
     * Fallback path is /mnt/SDCARD/.userdata/shared/navidrome/config.json.
     */
    memset(&g_config, 0, sizeof(g_config));
    nd_config_load(&g_config);

    /*
     * Determine starting module.
     * If the config has a server URL and username, go straight to browse.
     * Otherwise begin at auth.
     */
    bool has_token = nd_config_is_valid(&g_config);
    if (has_token) nd_favorites_sync_async();  /* no-op when offline */
    AppModule current = has_token ? MODULE_BROWSE : MODULE_AUTH;

    /* --- Main module loop --- */
    while (!g_quit) {
        AppModule next;

        switch (current) {
        case MODULE_AUTH:
            next = module_auth_run(screen);
            if (next == MODULE_BROWSE) nd_favorites_sync_async();
            break;
        case MODULE_BROWSE:
            next = module_browse_run(screen);
            break;
        case MODULE_PLAYER:
            next = module_player_run(screen);
            break;
        case MODULE_SETTINGS:
            next = module_settings_run(screen);
            break;
        case MODULE_QUIT:
        default:
            g_quit = true;
            next = MODULE_QUIT;
            break;
        }

        if (next == MODULE_QUIT) {
            g_quit = true;
        } else {
            current = next;
        }
    }

    /* --- Clean shutdown --- */
    PWR_enableAutosleep();
    nd_downloads_quit();
    nd_favorites_quit();
    nd_art_cleanup();
    Player_quit();
    Fonts_unload();
    PWR_quit();
    PAD_quit();
    GFX_quit();
    nd_log_flush();
    return EXIT_SUCCESS;
}
