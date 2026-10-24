#define _GNU_SOURCE
#include "nd_favorites.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>

#include "include/parson/parson.h"
#include "nd_api.h"
#include "nd_config.h"
#include "nd_log.h"

/* ------------------------------------------------------------------
 * Constants
 * ------------------------------------------------------------------ */

#define FAVORITES_FILENAME  "favorites.json"
#define FALLBACK_BASE_DIR   "/mnt/SDCARD/.userdata/shared/navidrome"
#define MAX_PENDING         ND_MAX_FAVORITES

/* ------------------------------------------------------------------
 * Module state (all guarded by g_lock)
 * ------------------------------------------------------------------ */

typedef struct {
    char id[ND_MAX_ID];
    bool star;          /* true = star, false = unstar */
} PendingOp;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static NdTrack   g_favs[ND_MAX_FAVORITES];
static int       g_fav_count = 0;
static PendingOp g_pending[MAX_PENDING];
static int       g_pending_count = 0;
static int       g_generation = 0;

static volatile bool g_sync_running = false;
static pthread_t     g_sync_thread;
static bool          g_sync_thread_started = false;

/* ------------------------------------------------------------------
 * Persistence
 * ------------------------------------------------------------------ */

static void favorites_path(char *out, int out_size)
{
    const char *base = getenv("SHARED_USERDATA_PATH");
    if (base && *base) {
        snprintf(out, out_size, "%s/navidrome/%s", base, FAVORITES_FILENAME);
    } else {
        snprintf(out, out_size, "%s/%s", FALLBACK_BASE_DIR, FAVORITES_FILENAME);
    }
}

/* Ensure the directory portion of a path exists (two levels). */
static void ensure_dir(const char *file_path)
{
    char dir[640];
    strncpy(dir, file_path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';

    char *slash = strrchr(dir, '/');
    if (!slash) return;
    *slash = '\0';

    char parent[640];
    strncpy(parent, dir, sizeof(parent) - 1);
    parent[sizeof(parent) - 1] = '\0';
    char *pslash = strrchr(parent, '/');
    if (pslash) {
        *pslash = '\0';
        mkdir(parent, 0755);
    }

    mkdir(dir, 0755);
}

/* Caller holds g_lock. */
static void favorites_save_locked(void)
{
    char path[640];
    favorites_path(path, sizeof(path));
    ensure_dir(path);

    JSON_Value  *root     = json_value_init_object();
    JSON_Object *root_obj = json_value_get_object(root);
    JSON_Value  *tracks_v = json_value_init_array();
    JSON_Array  *tracks_a = json_value_get_array(tracks_v);

    for (int i = 0; i < g_fav_count; i++) {
        const NdTrack *t = &g_favs[i];
        JSON_Value  *trk_v   = json_value_init_object();
        JSON_Object *trk_obj = json_value_get_object(trk_v);

        json_object_set_string(trk_obj, "track_id",     t->id);
        json_object_set_number(trk_obj, "track_number", (double)t->track_number);
        json_object_set_number(trk_obj, "duration_ms",  (double)t->duration_ms);
        json_object_set_string(trk_obj, "title",        t->title);
        json_object_set_string(trk_obj, "artist",       t->artist);
        json_object_set_string(trk_obj, "album",        t->album);
        json_object_set_string(trk_obj, "thumb",        t->thumb);
        json_object_set_string(trk_obj, "codec",        t->audio_codec);
        json_object_set_number(trk_obj, "bitrate_kbps", (double)t->audio_bitrate_kbps);

        json_array_append_value(tracks_a, trk_v);
    }
    json_object_set_value(root_obj, "tracks", tracks_v);

    JSON_Value *pend_v = json_value_init_array();
    JSON_Array *pend_a = json_value_get_array(pend_v);
    for (int i = 0; i < g_pending_count; i++) {
        JSON_Value  *pv = json_value_init_object();
        JSON_Object *po = json_value_get_object(pv);
        json_object_set_string(po, "id", g_pending[i].id);
        json_object_set_boolean(po, "star", g_pending[i].star);
        json_array_append_value(pend_a, pv);
    }
    json_object_set_value(root_obj, "pending", pend_v);

    char *json_str = json_serialize_to_string_pretty(root);
    json_value_free(root);
    if (!json_str) {
        ND_LOG_ERROR("[Favorites] Failed to serialise favorites\n");
        return;
    }

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        ND_LOG_ERROR("[Favorites] Cannot open favorites for write: %s\n", path);
        json_free_serialized_string(json_str);
        return;
    }

    size_t  len     = strlen(json_str);
    ssize_t written = write(fd, json_str, len);
    int     serr    = fsync(fd);
    close(fd);
    json_free_serialized_string(json_str);

    if (written != (ssize_t)len || serr != 0)
        ND_LOG_ERROR("[Favorites] Write/fsync error writing favorites\n");
}

static void copy_field(char *dst, size_t size, const char *src)
{
    if (!src) { dst[0] = '\0'; return; }
    strncpy(dst, src, size - 1);
    dst[size - 1] = '\0';
}

static void favorites_load(void)
{
    char path[640];
    favorites_path(path, sizeof(path));

    JSON_Value *root = json_parse_file(path);
    if (!root) {
        ND_LOG("[Favorites] No favorites file found, starting empty\n");
        return;
    }

    JSON_Object *root_obj = json_value_get_object(root);
    JSON_Array  *tracks_a = root_obj ? json_object_get_array(root_obj, "tracks") : NULL;

    g_fav_count = 0;
    int count = tracks_a ? (int)json_array_get_count(tracks_a) : 0;
    for (int i = 0; i < count && g_fav_count < ND_MAX_FAVORITES; i++) {
        JSON_Object *o = json_array_get_object(tracks_a, i);
        if (!o) continue;
        const char *id = json_object_get_string(o, "track_id");
        if (!id || !*id) continue;  /* skips legacy numeric ids */

        NdTrack *t = &g_favs[g_fav_count];
        memset(t, 0, sizeof(*t));
        nd_id_copy(t->id, id);
        t->track_number       = (int)json_object_get_number(o, "track_number");
        t->duration_ms        = (int)json_object_get_number(o, "duration_ms");
        t->audio_bitrate_kbps = (int)json_object_get_number(o, "bitrate_kbps");
        copy_field(t->title,       sizeof(t->title),       json_object_get_string(o, "title"));
        copy_field(t->artist,      sizeof(t->artist),      json_object_get_string(o, "artist"));
        copy_field(t->album,       sizeof(t->album),       json_object_get_string(o, "album"));
        copy_field(t->thumb,       sizeof(t->thumb),       json_object_get_string(o, "thumb"));
        copy_field(t->audio_codec, sizeof(t->audio_codec), json_object_get_string(o, "codec"));
        g_fav_count++;
    }

    JSON_Array *pend_a = root_obj ? json_object_get_array(root_obj, "pending") : NULL;
    g_pending_count = 0;
    int np = pend_a ? (int)json_array_get_count(pend_a) : 0;
    for (int i = 0; i < np && g_pending_count < MAX_PENDING; i++) {
        JSON_Object *o = json_array_get_object(pend_a, i);
        const char *id = o ? json_object_get_string(o, "id") : NULL;
        if (!id || !*id) continue;
        nd_id_copy(g_pending[g_pending_count].id, id);
        g_pending[g_pending_count].star = json_object_get_boolean(o, "star") == 1;
        g_pending_count++;
    }

    json_value_free(root);
    ND_LOG("[Favorites] Loaded %d favorites, %d pending\n", g_fav_count, g_pending_count);
}

/* ------------------------------------------------------------------
 * Server push
 * ------------------------------------------------------------------ */

/* Caller holds g_lock. A newer op on the same id replaces the old one. */
static void pending_add_locked(const char *id, bool star)
{
    for (int i = 0; i < g_pending_count; i++) {
        if (nd_id_eq(g_pending[i].id, id)) {
            g_pending[i].star = star;
            return;
        }
    }
    if (g_pending_count >= MAX_PENDING) return;
    nd_id_copy(g_pending[g_pending_count].id, id);
    g_pending[g_pending_count].star = star;
    g_pending_count++;
}

/* Caller holds g_lock. */
static void pending_remove_locked(const char *id, bool star)
{
    for (int i = 0; i < g_pending_count; i++) {
        if (nd_id_eq(g_pending[i].id, id) && g_pending[i].star == star) {
            memmove(&g_pending[i], &g_pending[i + 1],
                    (g_pending_count - i - 1) * sizeof(PendingOp));
            g_pending_count--;
            return;
        }
    }
}

typedef struct {
    NdConfig cfg;
    char     id[ND_MAX_ID];
    bool     star;
} StarCtx;

static void *star_worker(void *arg)
{
    StarCtx *ctx = (StarCtx *)arg;
    if (nd_api_set_starred(&ctx->cfg, ctx->id, ctx->star) == 0) {
        pthread_mutex_lock(&g_lock);
        pending_remove_locked(ctx->id, ctx->star);
        favorites_save_locked();
        pthread_mutex_unlock(&g_lock);
    }
    free(ctx);
    return NULL;
}

static void push_star_async(const char *id, bool star)
{
    const NdConfig *cfg = nd_config_get_mutable();
    if (!cfg || cfg->offline_mode || !nd_config_is_valid(cfg)) return;

    StarCtx *ctx = (StarCtx *)malloc(sizeof(*ctx));
    if (!ctx) return;
    ctx->cfg = *cfg;
    nd_id_copy(ctx->id, id);
    ctx->star = star;

    pthread_t t;
    if (pthread_create(&t, NULL, star_worker, ctx) != 0) {
        free(ctx);
        return;
    }
    pthread_detach(t);
}

/* ------------------------------------------------------------------
 * Two-way sync
 * ------------------------------------------------------------------ */

static void *sync_worker(void *arg)
{
    NdConfig *cfg = (NdConfig *)arg;

    /* 1. Push queued offline changes */
    PendingOp ops[MAX_PENDING];
    pthread_mutex_lock(&g_lock);
    int nops = g_pending_count;
    memcpy(ops, g_pending, nops * sizeof(PendingOp));
    pthread_mutex_unlock(&g_lock);

    for (int i = 0; i < nops; i++) {
        if (nd_api_set_starred(cfg, ops[i].id, ops[i].star) == 0) {
            pthread_mutex_lock(&g_lock);
            pending_remove_locked(ops[i].id, ops[i].star);
            pthread_mutex_unlock(&g_lock);
        }
    }

    /* 2. Pull the server's starred songs */
    NdTrack *server = (NdTrack *)malloc(sizeof(NdTrack) * ND_MAX_FAVORITES);
    int n = 0;
    if (server && nd_api_get_starred_tracks(cfg, server, ND_MAX_FAVORITES, &n) == 0) {
        pthread_mutex_lock(&g_lock);
        /* Keep anything still pending (push failed) consistent with the user's intent */
        int out = 0;
        for (int i = 0; i < n; i++) {
            bool pending_unstar = false;
            for (int p = 0; p < g_pending_count; p++)
                if (!g_pending[p].star && nd_id_eq(g_pending[p].id, server[i].id))
                    pending_unstar = true;
            if (!pending_unstar) server[out++] = server[i];
        }
        for (int p = 0; p < g_pending_count && out < ND_MAX_FAVORITES; p++) {
            if (!g_pending[p].star) continue;
            bool on_server = false;
            for (int i = 0; i < out; i++)
                if (nd_id_eq(server[i].id, g_pending[p].id)) { on_server = true; break; }
            if (on_server) continue;
            for (int i = 0; i < g_fav_count; i++) {
                if (nd_id_eq(g_favs[i].id, g_pending[p].id)) {
                    server[out++] = g_favs[i];
                    break;
                }
            }
        }
        memcpy(g_favs, server, out * sizeof(NdTrack));
        g_fav_count = out;
        g_generation++;
        favorites_save_locked();
        pthread_mutex_unlock(&g_lock);
        ND_LOG("[Favorites] Synced: %d starred songs\n", out);
    } else {
        pthread_mutex_lock(&g_lock);
        favorites_save_locked();  /* persist any pending ops that were pushed */
        pthread_mutex_unlock(&g_lock);
        ND_LOG_ERROR("[Favorites] Sync: could not fetch starred songs\n");
    }

    free(server);
    free(cfg);
    g_sync_running = false;
    return NULL;
}

void nd_favorites_sync_async(void)
{
    const NdConfig *live = nd_config_get_mutable();
    if (!live || live->offline_mode || !nd_config_is_valid(live)) return;
    if (g_sync_running) return;

    if (g_sync_thread_started) {
        pthread_join(g_sync_thread, NULL);
        g_sync_thread_started = false;
    }

    NdConfig *cfg = (NdConfig *)malloc(sizeof(NdConfig));
    if (!cfg) return;
    *cfg = *live;

    g_sync_running = true;
    if (pthread_create(&g_sync_thread, NULL, sync_worker, cfg) != 0) {
        g_sync_running = false;
        free(cfg);
        return;
    }
    g_sync_thread_started = true;
}

/* ------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------ */

void nd_favorites_init(void)
{
    pthread_mutex_lock(&g_lock);
    g_fav_count = 0;
    g_pending_count = 0;
    favorites_load();
    pthread_mutex_unlock(&g_lock);
}

void nd_favorites_quit(void)
{
    if (g_sync_thread_started) {
        pthread_join(g_sync_thread, NULL);
        g_sync_thread_started = false;
    }
}

bool nd_favorites_toggle(const NdTrack *t)
{
    if (!t || nd_id_empty(t->id)) return false;

    bool now_fav;
    pthread_mutex_lock(&g_lock);

    int found = -1;
    for (int i = 0; i < g_fav_count; i++) {
        if (nd_id_eq(g_favs[i].id, t->id)) { found = i; break; }
    }

    if (found >= 0) {
        if (found < g_fav_count - 1)
            memmove(&g_favs[found], &g_favs[found + 1],
                    (g_fav_count - found - 1) * sizeof(NdTrack));
        g_fav_count--;
        now_fav = false;
    } else if (g_fav_count >= ND_MAX_FAVORITES) {
        ND_LOG_ERROR("[Favorites] Favorites list full (%d), cannot add\n", ND_MAX_FAVORITES);
        pthread_mutex_unlock(&g_lock);
        return false;
    } else {
        g_favs[g_fav_count] = *t;
        g_favs[g_fav_count].local_path[0] = '\0';  /* runtime-only */
        g_fav_count++;
        now_fav = true;
    }

    /* Queue first; the push removes it on success. Survives a failed push
     * or an offline toggle until the next sync. */
    pending_add_locked(t->id, now_fav);
    g_generation++;
    favorites_save_locked();
    pthread_mutex_unlock(&g_lock);

    push_star_async(t->id, now_fav);
    return now_fav;
}

bool nd_favorites_contains(const char *track_id)
{
    if (nd_id_empty(track_id)) return false;
    bool found = false;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_fav_count; i++) {
        if (nd_id_eq(g_favs[i].id, track_id)) { found = true; break; }
    }
    pthread_mutex_unlock(&g_lock);
    return found;
}

int nd_favorites_get(NdTrack *out, int max)
{
    if (!out || max <= 0) return 0;
    pthread_mutex_lock(&g_lock);
    int n = g_fav_count < max ? g_fav_count : max;
    memcpy(out, g_favs, n * sizeof(NdTrack));
    pthread_mutex_unlock(&g_lock);
    return n;
}

int nd_favorites_count(void)
{
    pthread_mutex_lock(&g_lock);
    int n = g_fav_count;
    pthread_mutex_unlock(&g_lock);
    return n;
}

int nd_favorites_generation(void)
{
    pthread_mutex_lock(&g_lock);
    int g = g_generation;
    pthread_mutex_unlock(&g_lock);
    return g;
}
