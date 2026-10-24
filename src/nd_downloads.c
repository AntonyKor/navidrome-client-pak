#define _GNU_SOURCE
#include "nd_downloads.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <time.h>

#include "include/parson/parson.h"
#include "nd_api.h"
#include "nd_net.h"
#include "nd_log.h"
#include "nd_favorites.h"
#include "nd_config.h"

/* ------------------------------------------------------------------
 * Constants
 * ------------------------------------------------------------------ */

#define MANIFEST_FILENAME         "downloads.json"
#define FALLBACK_BASE_DIR         "/mnt/SDCARD/.userdata/shared/navidrome"
#define DL_QUEUE_MAX              8
#define MANIFEST_ALBUMS_CAP_INIT  128
#define MANIFEST_TRACKS_CAP_INIT  2048
#define MANIFEST_TRACKS_PER_ALBUM 64   /* per-album sanity cap in worker */

/* ------------------------------------------------------------------
 * Internal types
 * ------------------------------------------------------------------ */

/* One track as stored in the manifest. */
typedef struct {
    char track_id[ND_MAX_ID];
    int  track_number;
    int  duration_ms;
    int  bitrate_kbps;   /* download bitrate (kbps); 0 if unknown */
    char title[ND_MAX_STR];
    char artist[ND_MAX_STR];
    char album[ND_MAX_STR];
    char thumb[ND_MAX_URL];
    char local_path[768];
    char media_key[ND_MAX_URL];
} ManifestTrack;

/* One album as stored in the manifest. */
typedef struct {
    char album_id[ND_MAX_ID];
    char artist_id[ND_MAX_ID];
    char album_title[ND_MAX_STR];
    char artist_name[ND_MAX_STR];
    char thumb[ND_MAX_URL];
    char year[8];
    int  track_start;   /* index into g_tracks[] */
    int  track_count;
} ManifestAlbum;

/* One entry in the pending download queue. */
typedef struct {
    NdConfig cfg;
    char album_id[ND_MAX_ID];
    char album_title[ND_MAX_STR];
    char artist_id[ND_MAX_ID];
    char artist_name[ND_MAX_STR];
    char thumb[ND_MAX_URL];
    char year[8];
} QueueEntry;

/* ------------------------------------------------------------------
 * Module state
 * ------------------------------------------------------------------ */

static pthread_mutex_t  g_mutex   = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   g_cond    = PTHREAD_COND_INITIALIZER;
static pthread_t        g_worker;
static volatile bool    g_stop    = false;
static bool             g_started = false;

/* Manifest: downloaded albums */
static ManifestAlbum *g_albums     = NULL;
static int            g_albums_cap = 0;
static int            g_album_count = 0;

/* Flat track pool: album N's tracks are at g_tracks[album.track_start .. track_start+track_count-1] */
static ManifestTrack *g_tracks     = NULL;
static int            g_tracks_cap = 0;
static int            g_track_count = 0;

/* Download queue (ring buffer, FIFO) */
static QueueEntry g_queue[DL_QUEUE_MAX];
static int        g_queue_head  = 0;
static int        g_queue_tail  = 0;
static int        g_queue_count = 0;

/* Currently downloading album_id ("" = none) */
static char g_active_album_id[ND_MAX_ID] = "";
static int g_active_completed = 0;
static int g_active_total     = 0;

/* Favorites sync slot */
typedef struct {
    NdConfig  cfg;
    NdTrack tracks[ND_MAX_FAVORITES];
    int       count;
} FavSyncSlot;

static FavSyncSlot g_fav_sync_slot;
static bool        g_fav_sync_pending = false;
static bool        g_fav_sync_active  = false;

/* ------------------------------------------------------------------
 * Path helpers
 * ------------------------------------------------------------------ */

static void manifest_path(char *out, int out_size)
{
    const char *base = getenv("SHARED_USERDATA_PATH");
    if (base && *base)
        snprintf(out, out_size, "%s/navidrome/%s", base, MANIFEST_FILENAME);
    else
        snprintf(out, out_size, "%s/%s", FALLBACK_BASE_DIR, MANIFEST_FILENAME);
}

static void downloads_base_dir(char *out, int out_size)
{
    const char *base = getenv("SHARED_USERDATA_PATH");
    if (base && *base)
        snprintf(out, out_size, "%s/navidrome/downloads", base);
    else
        snprintf(out, out_size, "%s/downloads", FALLBACK_BASE_DIR);
}

/* Ids become path components: keep only filename-safe characters. */
static void safe_name(const char *id, char *out, int out_size)
{
    int i = 0;
    for (; id && id[i] && i < out_size - 1; i++) {
        char c = id[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_';
        out[i] = ok ? c : '_';
    }
    out[i] = '\0';
}

static void album_dir_path(const char *album_id, char *out, int out_size)
{
    char base[512], name[ND_MAX_ID];
    downloads_base_dir(base, sizeof(base));
    safe_name(album_id, name, sizeof(name));
    snprintf(out, out_size, "%s/%s", base, name);
}

static void track_local_path(const char *album_id, const char *track_id, const char *ext,
                              char *out, int out_size)
{
    char dir[640], name[ND_MAX_ID];
    album_dir_path(album_id, dir, sizeof(dir));
    safe_name(track_id, name, sizeof(name));
    snprintf(out, out_size, "%s/%s.%s", dir, name, ext);
}

/* ------------------------------------------------------------------
 * Directory creation (adapted from nd_config.c ensure_config_dir)
 * ------------------------------------------------------------------ */

static void ensure_parent_dirs(const char *file_path)
{
    char dir[640];
    strncpy(dir, file_path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';

    char *slash = strrchr(dir, '/');
    if (!slash) return;
    *slash = '\0';

    /* Create grandparent directory first (best-effort) */
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

/* ------------------------------------------------------------------
 * Track download helpers
 * ------------------------------------------------------------------ */

/* File extension a downloaded track will have ("opus" when transcoded). */
static void download_ext(const NdConfig *cfg, const NdTrack *t, char *ext, int ext_size)
{
    if (nd_api_download_bitrate(cfg, t) > 0)
        snprintf(ext, ext_size, "opus");
    else
        nd_api_track_ext(t, ext, ext_size);
}

/*
 * Download one track (original or Opus transcode) to local_path via a .tmp
 * file. Returns 0 on success, -1 on failure or cancellation.
 */
static int download_track(const NdConfig *cfg, const NdTrack *t,
                          const char *local_path, volatile bool *should_cancel)
{
    int kbps = nd_api_download_bitrate(cfg, t);
    char url[2048];
    if (kbps > 0) nd_api_get_transcode_url(cfg, t, kbps, url, sizeof(url));
    else          nd_api_get_stream_url(cfg, t, url, sizeof(url));

    ensure_parent_dirs(local_path);

    NdNetOptions opts;
    memset(&opts, 0, sizeof(opts));
    opts.method      = ND_HTTP_GET;
    opts.token       = NULL;
    opts.timeout_sec = 300;
    opts.no_persist  = true;

    char tmp_path[768 + 4];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", local_path);

    int ret = nd_net_download_file(url, tmp_path, NULL, should_cancel, &opts);
    if (ret <= 0) {
        ND_LOG_ERROR("[Downloads] download failed for track %s (%s)\n", t->id, t->title);
        remove(tmp_path);
        return -1;
    }
    if (rename(tmp_path, local_path) != 0) {
        ND_LOG_ERROR("[Downloads] rename failed: %s\n", local_path);
        remove(tmp_path);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------
 * Grow helpers — caller must hold g_mutex (or call before worker starts)
 * ------------------------------------------------------------------ */

/* Returns false on allocation failure. */
static bool grow_albums(int needed)
{
    if (needed <= g_albums_cap) return true;
    int new_cap = g_albums_cap * 2;
    if (new_cap < needed) new_cap = needed;
    ManifestAlbum *p = realloc(g_albums, new_cap * sizeof(ManifestAlbum));
    if (!p) return false;
    g_albums     = p;
    g_albums_cap = new_cap;
    return true;
}

static bool grow_tracks(int needed)
{
    if (needed <= g_tracks_cap) return true;
    int new_cap = g_tracks_cap * 2;
    if (new_cap < needed) new_cap = needed;
    ManifestTrack *p = realloc(g_tracks, new_cap * sizeof(ManifestTrack));
    if (!p) return false;
    g_tracks     = p;
    g_tracks_cap = new_cap;
    return true;
}

/* ------------------------------------------------------------------
 * Manifest persistence — caller must hold g_mutex
 * ------------------------------------------------------------------ */

static void manifest_save_locked(void)
{
    char path[640];
    manifest_path(path, sizeof(path));
    ensure_parent_dirs(path);

    JSON_Value  *root     = json_value_init_object();
    JSON_Object *root_obj = json_value_get_object(root);
    JSON_Value  *albums_v = json_value_init_array();
    JSON_Array  *albums_a = json_value_get_array(albums_v);

    for (int i = 0; i < g_album_count; i++) {
        const ManifestAlbum *ma = &g_albums[i];

        JSON_Value  *alb_v   = json_value_init_object();
        JSON_Object *alb_obj = json_value_get_object(alb_v);

        json_object_set_string(alb_obj, "album_id",    ma->album_id);
        json_object_set_string(alb_obj, "album_title", ma->album_title);
        json_object_set_string(alb_obj, "artist_id",   ma->artist_id);
        json_object_set_string(alb_obj, "artist_name", ma->artist_name);
        json_object_set_string(alb_obj, "thumb",       ma->thumb);
        json_object_set_string(alb_obj, "year",        ma->year);

        JSON_Value *tracks_v = json_value_init_array();
        JSON_Array *tracks_a = json_value_get_array(tracks_v);

        for (int j = 0; j < ma->track_count; j++) {
            const ManifestTrack *mt = &g_tracks[ma->track_start + j];

            JSON_Value  *trk_v   = json_value_init_object();
            JSON_Object *trk_obj = json_value_get_object(trk_v);

            json_object_set_string(trk_obj, "track_id",     mt->track_id);
            json_object_set_string(trk_obj, "title",        mt->title);
            json_object_set_string(trk_obj, "artist",       mt->artist);
            json_object_set_string(trk_obj, "album",        mt->album);
            json_object_set_number(trk_obj, "track_number", (double)mt->track_number);
            json_object_set_number(trk_obj, "duration_ms",  (double)mt->duration_ms);
            json_object_set_number(trk_obj, "bitrate_kbps", (double)mt->bitrate_kbps);
            json_object_set_string(trk_obj, "thumb",        mt->thumb);
            json_object_set_string(trk_obj, "local_path",   mt->local_path);
            json_object_set_string(trk_obj, "media_key",    mt->media_key);

            json_array_append_value(tracks_a, trk_v);
        }

        json_object_set_value(alb_obj, "tracks", tracks_v);
        json_array_append_value(albums_a, alb_v);
    }

    json_object_set_value(root_obj, "albums", albums_v);

    char *json_str = json_serialize_to_string_pretty(root);
    json_value_free(root);
    if (!json_str) {
        ND_LOG_ERROR("[Downloads] Failed to serialise manifest\n");
        return;
    }

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        ND_LOG_ERROR("[Downloads] Cannot open manifest for write: %s\n", path);
        json_free_serialized_string(json_str);
        return;
    }

    size_t  len     = strlen(json_str);
    ssize_t written = write(fd, json_str, len);
    int     serr    = fsync(fd);
    close(fd);
    json_free_serialized_string(json_str);

    if (written != (ssize_t)len || serr != 0)
        ND_LOG_ERROR("[Downloads] Write/fsync error writing manifest\n");
}

/* ------------------------------------------------------------------
 * Manifest load (called once at init, no lock needed yet)
 * ------------------------------------------------------------------ */

static bool album_fully_downloaded(const ManifestAlbum *ma)
{
    if (ma->track_count == 0) return false;
    for (int i = 0; i < ma->track_count; i++) {
        if (access(g_tracks[ma->track_start + i].local_path, F_OK) != 0)
            return false;
    }
    return true;
}

static void manifest_load(void)
{
    char path[640];
    manifest_path(path, sizeof(path));

    JSON_Value *root = json_parse_file(path);
    if (!root) {
        ND_LOG("[Downloads] No manifest at %s; starting empty\n", path);
        return;
    }

    JSON_Object *root_obj = json_value_get_object(root);
    if (!root_obj) {
        json_value_free(root);
        return;
    }

    JSON_Array *albums_a = json_object_get_array(root_obj, "albums");
    if (!albums_a) {
        json_value_free(root);
        return;
    }

    size_t album_cnt = json_array_get_count(albums_a);
    for (size_t i = 0; i < album_cnt; i++) {
        JSON_Object *alb_obj = json_array_get_object(albums_a, i);
        if (!alb_obj) continue;

        ManifestAlbum ma;
        memset(&ma, 0, sizeof(ma));

        const char *s;
        if (!(s = json_object_get_string(alb_obj, "album_id")) || !*s) continue;
        nd_id_copy(ma.album_id, s);
        nd_id_copy(ma.artist_id, json_object_get_string(alb_obj, "artist_id"));
        if ((s = json_object_get_string(alb_obj, "album_title")))
            strncpy(ma.album_title, s, sizeof(ma.album_title) - 1);
        if ((s = json_object_get_string(alb_obj, "artist_name")))
            strncpy(ma.artist_name, s, sizeof(ma.artist_name) - 1);
        if ((s = json_object_get_string(alb_obj, "thumb")))
            strncpy(ma.thumb, s, sizeof(ma.thumb) - 1);
        /* year: optional — old manifests without it load as empty string */
        s = json_object_get_string(alb_obj, "year");
        if (s) strncpy(ma.year, s, sizeof(ma.year) - 1);

        JSON_Array *tracks_a = json_object_get_array(alb_obj, "tracks");
        int pending_track_start = g_track_count;
        int pending_track_count = 0;

        if (tracks_a) {
            size_t trk_cnt = json_array_get_count(tracks_a);
            for (size_t j = 0; j < trk_cnt && pending_track_count < MANIFEST_TRACKS_PER_ALBUM; j++) {
                JSON_Object *trk_obj = json_array_get_object(tracks_a, j);
                if (!trk_obj) continue;

                ManifestTrack mt;
                memset(&mt, 0, sizeof(mt));

                nd_id_copy(mt.track_id, json_object_get_string(trk_obj, "track_id"));
                if (!mt.track_id[0]) continue;
                mt.track_number = (int)json_object_get_number(trk_obj, "track_number");
                mt.duration_ms  = (int)json_object_get_number(trk_obj, "duration_ms");
                mt.bitrate_kbps = (int)json_object_get_number(trk_obj, "bitrate_kbps");

                if ((s = json_object_get_string(trk_obj, "title")))
                    strncpy(mt.title, s, sizeof(mt.title) - 1);
                if ((s = json_object_get_string(trk_obj, "artist")))
                    strncpy(mt.artist, s, sizeof(mt.artist) - 1);
                if ((s = json_object_get_string(trk_obj, "album")))
                    strncpy(mt.album, s, sizeof(mt.album) - 1);
                if ((s = json_object_get_string(trk_obj, "thumb")))
                    strncpy(mt.thumb, s, sizeof(mt.thumb) - 1);
                if ((s = json_object_get_string(trk_obj, "local_path")))
                    strncpy(mt.local_path, s, sizeof(mt.local_path) - 1);
                s = json_object_get_string(trk_obj, "media_key");
                if (s) strncpy(mt.media_key, s, sizeof(mt.media_key) - 1);

                if (!grow_tracks(g_track_count + 1)) break;
                g_tracks[g_track_count++] = mt;
                pending_track_count++;
            }
        }

        ma.track_start = pending_track_start;
        ma.track_count = pending_track_count;

        /* Favorites album: keep only tracks whose files are present */
        if (nd_id_eq(ma.album_id, ND_FAVORITES_SYNC_ALBUM_ID)) {
            /* Filter g_tracks in-place: remove entries whose file is missing */
            int write = pending_track_start;
            for (int j = pending_track_start; j < pending_track_start + pending_track_count; j++) {
                if (access(g_tracks[j].local_path, F_OK) == 0) {
                    if (write != j)
                        g_tracks[write] = g_tracks[j];
                    write++;
                }
            }
            pending_track_count = write - pending_track_start;
            g_track_count = write;
            /* Skip album_fully_downloaded check — add even if empty */
            if (grow_albums(g_album_count + 1)) {
                ma.track_start = pending_track_start;
                ma.track_count = pending_track_count;
                g_albums[g_album_count++] = ma;
            } else {
                /* OOM: roll back tracks */
                g_track_count = pending_track_start;
            }
            continue;  /* skip the normal album_fully_downloaded path below */
        }

        /* Discard albums with any missing files (partial previous download) */
        if (!album_fully_downloaded(&ma)) {
            ND_LOG("[Downloads] Discarding partial album %s (%s)\n",
                     ma.album_id, ma.album_title);
            /* Roll back appended tracks */
            g_track_count = pending_track_start;
            continue;
        }

        if (!grow_albums(g_album_count + 1)) break;
        g_albums[g_album_count++] = ma;
    }

    json_value_free(root);
    ND_LOG("[Downloads] Manifest loaded: %d complete album(s)\n", g_album_count);
}

/* ------------------------------------------------------------------
 * Background worker thread
 * ------------------------------------------------------------------ */

static void *download_worker(void *arg)
{
    (void)arg;

    while (1) {
        pthread_mutex_lock(&g_mutex);
        while (g_queue_count == 0 && !g_fav_sync_pending && !g_stop)
            pthread_cond_wait(&g_cond, &g_mutex);

        if (g_stop) {
            pthread_mutex_unlock(&g_mutex);
            break;
        }

        /* Prefer album queue; handle fav-sync only when queue is empty */
        if (g_queue_count > 0) {
            /* Dequeue FIFO */
            QueueEntry entry = g_queue[g_queue_head];
            g_queue_head  = (g_queue_head + 1) % DL_QUEUE_MAX;
            g_queue_count--;
            nd_id_copy(g_active_album_id, entry.album_id);

            pthread_mutex_unlock(&g_mutex);

            const NdConfig *cfg = &entry.cfg;

            /* Fetch track listing */
            NdTrack *tracks = malloc(sizeof(NdTrack) * ND_MAX_TRACKS);
            int      track_count = 0;
            if (!tracks || nd_api_get_tracks(cfg, entry.album_id, tracks, &track_count) != 0) {
                ND_LOG_ERROR("[Downloads] nd_api_get_tracks failed for album %s\n",
                               entry.album_id);
                free(tracks);
                pthread_mutex_lock(&g_mutex);
                if (nd_id_eq(g_active_album_id, entry.album_id))
                    g_active_album_id[0] = '\0';
                pthread_mutex_unlock(&g_mutex);
                continue;
            }

            ND_LOG("[Downloads] Album %s: %d tracks to download\n",
                     entry.album_id, track_count);

            int limit = track_count < MANIFEST_TRACKS_PER_ALBUM ? track_count : MANIFEST_TRACKS_PER_ALBUM;

            pthread_mutex_lock(&g_mutex);
            g_active_completed = 0;
            g_active_total     = limit;
            pthread_mutex_unlock(&g_mutex);

            /* Heap-allocate local track buffer for this album's download */
            ManifestTrack *work_tracks = malloc(MANIFEST_TRACKS_PER_ALBUM * sizeof(ManifestTrack));
            if (!work_tracks) {
                ND_LOG_ERROR("[Downloads] OOM allocating work_tracks for album %s\n",
                               entry.album_id);
                free(tracks);
                pthread_mutex_lock(&g_mutex);
                if (nd_id_eq(g_active_album_id, entry.album_id)) g_active_album_id[0] = '\0';
                pthread_mutex_unlock(&g_mutex);
                continue;
            }
            int work_track_count = 0;

            for (int i = 0; i < limit; i++) {
                /* Check stop flag between tracks */
                pthread_mutex_lock(&g_mutex);
                bool should_stop = g_stop;
                pthread_mutex_unlock(&g_mutex);
                if (should_stop) break;

                const NdTrack *t = &tracks[i];
                int dl_bitrate = nd_api_download_bitrate(cfg, t);

                char ext[16];
                download_ext(cfg, t, ext, sizeof(ext));

                char local_path[768];
                track_local_path(entry.album_id, t->id, ext, local_path, sizeof(local_path));

                ND_LOG("[Downloads] [%d/%d] %s -> %s\n",
                         i + 1, limit, t->title, local_path);

                if (download_track(cfg, t, local_path, &g_stop) != 0)
                    continue;  /* skip this track */

                /* Build manifest track */
                ManifestTrack mt;
                memset(&mt, 0, sizeof(mt));
                nd_id_copy(mt.track_id, t->id);
                mt.track_number = t->track_number;
                mt.duration_ms  = t->duration_ms;
                mt.bitrate_kbps = (dl_bitrate > 0) ? dl_bitrate : t->audio_bitrate_kbps;
                strncpy(mt.title,      t->title,     sizeof(mt.title) - 1);
                strncpy(mt.artist,     t->artist,    sizeof(mt.artist) - 1);
                strncpy(mt.album,      t->album,     sizeof(mt.album) - 1);
                strncpy(mt.thumb,      t->thumb,     sizeof(mt.thumb) - 1);
                strncpy(mt.local_path, local_path,   sizeof(mt.local_path) - 1);

                pthread_mutex_lock(&g_mutex);
                g_active_completed = work_track_count + 1;
                pthread_mutex_unlock(&g_mutex);

                if (work_track_count < MANIFEST_TRACKS_PER_ALBUM)
                    work_tracks[work_track_count++] = mt;
            }
            free(tracks);

            pthread_mutex_lock(&g_mutex);
            if (nd_id_eq(g_active_album_id, entry.album_id))
                g_active_album_id[0] = '\0';
            g_active_completed = 0;
            g_active_total     = 0;

            if (work_track_count > 0) {
                /* Find existing album or append new one */
                int idx = -1;
                for (int k = 0; k < g_album_count; k++) {
                    if (nd_id_eq(g_albums[k].album_id, entry.album_id)) { idx = k; break; }
                }

                if (idx >= 0) {
                    /* Replace existing — remove old tracks first */
                    int old_start = g_albums[idx].track_start;
                    int old_count = g_albums[idx].track_count;
                    if (old_count > 0) {
                        memmove(&g_tracks[old_start],
                                &g_tracks[old_start + old_count],
                                (g_track_count - old_start - old_count) * sizeof(ManifestTrack));
                        g_track_count -= old_count;
                        for (int k = 0; k < g_album_count; k++) {
                            if (g_albums[k].track_start > old_start)
                                g_albums[k].track_start -= old_count;
                        }
                    }
                    /* Append new tracks and update album */
                    if (grow_tracks(g_track_count + work_track_count)) {
                        g_albums[idx].track_start = g_track_count;
                        g_albums[idx].track_count = work_track_count;
                        memcpy(&g_tracks[g_track_count], work_tracks,
                               work_track_count * sizeof(ManifestTrack));
                        g_track_count += work_track_count;
                    }
                } else {
                    /* New album */
                    if (grow_albums(g_album_count + 1) &&
                        grow_tracks(g_track_count + work_track_count)) {
                        ManifestAlbum ma_new;
                        memset(&ma_new, 0, sizeof(ma_new));
                        nd_id_copy(ma_new.album_id,  entry.album_id);
                        nd_id_copy(ma_new.artist_id, entry.artist_id);
                        strncpy(ma_new.album_title, entry.album_title, sizeof(ma_new.album_title) - 1);
                        strncpy(ma_new.artist_name, entry.artist_name, sizeof(ma_new.artist_name) - 1);
                        strncpy(ma_new.thumb,       entry.thumb,       sizeof(ma_new.thumb) - 1);
                        strncpy(ma_new.year,        entry.year,        sizeof(ma_new.year) - 1);
                        ma_new.track_start = g_track_count;
                        ma_new.track_count = work_track_count;
                        memcpy(&g_tracks[g_track_count], work_tracks,
                               work_track_count * sizeof(ManifestTrack));
                        g_track_count += work_track_count;
                        g_albums[g_album_count++] = ma_new;
                    }
                }
                manifest_save_locked();
            }
            pthread_mutex_unlock(&g_mutex);
            free(work_tracks);

            ND_LOG("[Downloads] Album %s done (%d/%d tracks)\n",
                     entry.album_id, work_track_count, limit);

        } else {
            /* --- Favorites sync processing (queue is empty) --- */
            g_fav_sync_pending     = false;
            g_fav_sync_active      = true;
            nd_id_copy(g_active_album_id, ND_FAVORITES_SYNC_ALBUM_ID);
            g_active_completed     = 0;
            g_active_total         = g_fav_sync_slot.count;
            pthread_mutex_unlock(&g_mutex);

            for (int i = 0; i < g_fav_sync_slot.count; i++) {
                pthread_mutex_lock(&g_mutex);
                bool should_stop = g_stop;
                pthread_mutex_unlock(&g_mutex);
                if (should_stop) break;

                const NdTrack *t = &g_fav_sync_slot.tracks[i];

                const NdConfig *cfg = &g_fav_sync_slot.cfg;
                int dl_bitrate = nd_api_download_bitrate(cfg, t);
                char ext[16];
                download_ext(cfg, t, ext, sizeof(ext));

                char local_path[768];
                track_local_path(ND_FAVORITES_SYNC_ALBUM_ID,
                                 t->id, ext, local_path, sizeof(local_path));

                if (download_track(cfg, t, local_path, &g_stop) != 0) {
                    ND_LOG_ERROR("[Downloads] Fav sync: failed track %s (%s)\n",
                                   t->id, t->title);
                    continue;
                }

                /* Append track to favorites manifest album */
                pthread_mutex_lock(&g_mutex);

                ManifestTrack mt = {0};
                nd_id_copy(mt.track_id, t->id);
                mt.track_number = t->track_number;
                mt.duration_ms  = t->duration_ms;
                mt.bitrate_kbps = (dl_bitrate > 0) ? dl_bitrate : t->audio_bitrate_kbps;
                strncpy(mt.title,      t->title,      sizeof(mt.title) - 1);
                strncpy(mt.artist,     t->artist,     sizeof(mt.artist) - 1);
                strncpy(mt.album,      t->album,      sizeof(mt.album) - 1);
                strncpy(mt.thumb,      t->thumb,      sizeof(mt.thumb) - 1);
                strncpy(mt.local_path, local_path,    sizeof(mt.local_path) - 1);

                /* Find or create the favorites manifest album */
                int fav_idx = -1;
                for (int k = 0; k < g_album_count; k++) {
                    if (nd_id_eq(g_albums[k].album_id, ND_FAVORITES_SYNC_ALBUM_ID)) {
                        fav_idx = k; break;
                    }
                }
                if (fav_idx < 0) {
                    /* Create it */
                    if (grow_albums(g_album_count + 1)) {
                        ManifestAlbum ma_new = {0};
                        nd_id_copy(ma_new.album_id, ND_FAVORITES_SYNC_ALBUM_ID);
                        strncpy(ma_new.album_title, "Favorite Tracks",
                                sizeof(ma_new.album_title) - 1);
                        ma_new.track_start = g_track_count;
                        ma_new.track_count = 0;
                        g_albums[g_album_count++] = ma_new;
                        fav_idx = g_album_count - 1;
                    }
                }
                if (fav_idx >= 0 && grow_tracks(g_track_count + 1)) {
                    g_tracks[g_track_count++] = mt;
                    g_albums[fav_idx].track_count++;
                }
                g_active_completed = i + 1;
                manifest_save_locked();
                pthread_mutex_unlock(&g_mutex);
            }

            pthread_mutex_lock(&g_mutex);
            g_active_album_id[0] = '\0';
            g_active_completed = 0;
            g_active_total     = 0;
            g_fav_sync_active  = false;
            pthread_mutex_unlock(&g_mutex);
        }
    }

    return NULL;
}

/* ------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------ */

void nd_downloads_sync_favorites(const NdConfig *cfg,
                                   const NdTrack *favs, int fav_count)
{
    if (!cfg || !favs || fav_count < 0) return;

    pthread_mutex_lock(&g_mutex);

    if (g_fav_sync_pending || g_fav_sync_active) {
        pthread_mutex_unlock(&g_mutex);
        return;
    }

    /* Step A: delete removed tracks */
    int fav_album_idx = -1;
    for (int i = 0; i < g_album_count; i++) {
        if (nd_id_eq(g_albums[i].album_id, ND_FAVORITES_SYNC_ALBUM_ID)) {
            fav_album_idx = i;
            break;
        }
    }
    if (fav_album_idx >= 0) {
        ManifestAlbum *fa = &g_albums[fav_album_idx];
        for (int j = 0; j < fa->track_count; j++) {
            const char *tid = g_tracks[fa->track_start + j].track_id;
            bool still_fav = false;
            for (int k = 0; k < fav_count; k++) {
                if (nd_id_eq(favs[k].id, tid)) { still_fav = true; break; }
            }
            if (!still_fav)
                unlink(g_tracks[fa->track_start + j].local_path);
        }

        /* Remove the album entirely using the memmove + fixup pattern */
        int ts = fa->track_start;
        int tc = fa->track_count;
        if (tc > 0) {
            memmove(&g_tracks[ts], &g_tracks[ts + tc],
                    (g_track_count - ts - tc) * sizeof(ManifestTrack));
            g_track_count -= tc;
            for (int k = 0; k < g_album_count; k++) {
                if (g_albums[k].track_start > ts)
                    g_albums[k].track_start -= tc;
            }
        }
        if (fav_album_idx < g_album_count - 1)
            memmove(&g_albums[fav_album_idx], &g_albums[fav_album_idx + 1],
                    (g_album_count - fav_album_idx - 1) * sizeof(ManifestAlbum));
        g_album_count--;
    }

    /* Step B: rebuild manifest with surviving downloaded tracks */
    static ManifestTrack surviving[ND_MAX_FAVORITES];  /* ~1.5 MB: keep off the stack */
    int surviving_count = 0;

    for (int i = 0; i < fav_count && surviving_count < ND_MAX_FAVORITES; i++) {
        char ext[16];
        download_ext(cfg, &favs[i], ext, sizeof(ext));

        char local_path[768];
        track_local_path(ND_FAVORITES_SYNC_ALBUM_ID,
                         favs[i].id, ext, local_path, sizeof(local_path));

        if (access(local_path, F_OK) == 0) {
            ManifestTrack mt = {0};
            nd_id_copy(mt.track_id, favs[i].id);
            mt.track_number = favs[i].track_number;
            mt.duration_ms  = favs[i].duration_ms;
            strncpy(mt.title,      favs[i].title,      sizeof(mt.title) - 1);
            strncpy(mt.artist,     favs[i].artist,     sizeof(mt.artist) - 1);
            strncpy(mt.album,      favs[i].album,      sizeof(mt.album) - 1);
            strncpy(mt.thumb,      favs[i].thumb,      sizeof(mt.thumb) - 1);
            strncpy(mt.local_path, local_path,         sizeof(mt.local_path) - 1);
            surviving[surviving_count++] = mt;
        }
    }

    if (surviving_count > 0) {
        if (grow_albums(g_album_count + 1) &&
            grow_tracks(g_track_count + surviving_count)) {
            ManifestAlbum ma_new = {0};
            nd_id_copy(ma_new.album_id, ND_FAVORITES_SYNC_ALBUM_ID);
            strncpy(ma_new.album_title, "Favorite Tracks",
                    sizeof(ma_new.album_title) - 1);
            ma_new.track_start = g_track_count;
            ma_new.track_count = surviving_count;
            memcpy(&g_tracks[g_track_count], surviving,
                   surviving_count * sizeof(ManifestTrack));
            g_track_count += surviving_count;
            g_albums[g_album_count++] = ma_new;
        }
    }

    manifest_save_locked();

    /* Step C: build download list — tracks not yet on disk */
    memset(&g_fav_sync_slot, 0, sizeof(g_fav_sync_slot));
    g_fav_sync_slot.cfg = *cfg;

    for (int i = 0; i < fav_count && g_fav_sync_slot.count < ND_MAX_FAVORITES; i++) {
        char ext[16];
        download_ext(cfg, &favs[i], ext, sizeof(ext));

        char local_path[768];
        track_local_path(ND_FAVORITES_SYNC_ALBUM_ID,
                         favs[i].id, ext, local_path, sizeof(local_path));

        if (access(local_path, F_OK) != 0) {
            g_fav_sync_slot.tracks[g_fav_sync_slot.count++] = favs[i];
        }
    }

    g_fav_sync_pending = true;

    pthread_mutex_unlock(&g_mutex);
    pthread_cond_signal(&g_cond);
}

void nd_downloads_init(void)
{
    g_albums = malloc(MANIFEST_ALBUMS_CAP_INIT * sizeof(ManifestAlbum));
    g_albums_cap = g_albums ? MANIFEST_ALBUMS_CAP_INIT : 0;

    g_tracks = malloc(MANIFEST_TRACKS_CAP_INIT * sizeof(ManifestTrack));
    g_tracks_cap = g_tracks ? MANIFEST_TRACKS_CAP_INIT : 0;

    manifest_load();

    g_stop            = false;
    g_queue_head      = 0;
    g_queue_tail      = 0;
    g_queue_count     = 0;
    g_active_album_id[0] = '\0';

    if (pthread_create(&g_worker, NULL, download_worker, NULL) != 0) {
        ND_LOG_ERROR("[Downloads] Failed to create worker thread\n");
        return;
    }
    g_started = true;
    ND_LOG("[Downloads] Initialised\n");
}

void nd_downloads_quit(void)
{
    if (!g_started) return;
    pthread_mutex_lock(&g_mutex);
    g_stop = true;
    pthread_cond_signal(&g_cond);
    pthread_mutex_unlock(&g_mutex);
    pthread_join(g_worker, NULL);
    g_started = false;
    ND_LOG("[Downloads] Shutdown complete\n");
    free(g_albums); g_albums = NULL; g_albums_cap = 0; g_album_count = 0;
    free(g_tracks); g_tracks = NULL; g_tracks_cap = 0; g_track_count = 0;
}

void nd_downloads_queue_album(const NdConfig *cfg,
                                const char *album_id,
                                const char *album_title,
                                const char *artist_id,
                                const char *artist_name,
                                const char *thumb,
                                const char *year)
{
    if (!cfg || nd_id_empty(album_id)) return;

    pthread_mutex_lock(&g_mutex);

    /* No-op if fully downloaded */
    for (int i = 0; i < g_album_count; i++) {
        if (nd_id_eq(g_albums[i].album_id, album_id)) {
            pthread_mutex_unlock(&g_mutex);
            return;
        }
    }

    /* No-op if currently downloading */
    if (nd_id_eq(g_active_album_id, album_id)) {
        pthread_mutex_unlock(&g_mutex);
        return;
    }

    /* No-op if already in queue */
    for (int i = 0; i < g_queue_count; i++) {
        int idx = (g_queue_head + i) % DL_QUEUE_MAX;
        if (nd_id_eq(g_queue[idx].album_id, album_id)) {
            pthread_mutex_unlock(&g_mutex);
            return;
        }
    }

    /* No-op if queue full */
    if (g_queue_count >= DL_QUEUE_MAX) {
        ND_LOG("[Downloads] Queue full, dropping album %s\n", album_id);
        pthread_mutex_unlock(&g_mutex);
        return;
    }

    QueueEntry *e = &g_queue[g_queue_tail];
    memset(e, 0, sizeof(*e));
    e->cfg = *cfg;
    nd_id_copy(e->album_id, album_id);
    if (album_title)  strncpy(e->album_title,  album_title,  sizeof(e->album_title) - 1);
    nd_id_copy(e->artist_id, artist_id);
    if (artist_name)  strncpy(e->artist_name,  artist_name,  sizeof(e->artist_name) - 1);
    if (thumb)        strncpy(e->thumb,         thumb,        sizeof(e->thumb) - 1);
    if (year)         strncpy(e->year,          year,         sizeof(e->year) - 1);

    g_queue_tail  = (g_queue_tail + 1) % DL_QUEUE_MAX;
    g_queue_count++;

    pthread_cond_signal(&g_cond);
    pthread_mutex_unlock(&g_mutex);

    ND_LOG("[Downloads] Queued album %s (%s)\n", album_id,
             album_title ? album_title : "");
}

DlStatus nd_downloads_album_status(const char *album_id)
{
    pthread_mutex_lock(&g_mutex);

    /* Active download takes priority over manifest presence */
    if (nd_id_eq(g_active_album_id, album_id)) {
        pthread_mutex_unlock(&g_mutex);
        return DL_STATUS_DOWNLOADING;
    }

    /* Check manifest (DONE) */
    for (int i = 0; i < g_album_count; i++) {
        if (nd_id_eq(g_albums[i].album_id, album_id)) {
            pthread_mutex_unlock(&g_mutex);
            return DL_STATUS_DONE;
        }
    }

    /* Check queue */
    for (int i = 0; i < g_queue_count; i++) {
        int idx = (g_queue_head + i) % DL_QUEUE_MAX;
        if (nd_id_eq(g_queue[idx].album_id, album_id)) {
            pthread_mutex_unlock(&g_mutex);
            return DL_STATUS_QUEUED;
        }
    }

    pthread_mutex_unlock(&g_mutex);
    return DL_STATUS_NONE;
}

bool nd_downloads_is_active(void)
{
    pthread_mutex_lock(&g_mutex);
    bool active = (g_active_album_id[0] != '\0') || g_fav_sync_pending || g_fav_sync_active;
    pthread_mutex_unlock(&g_mutex);
    return active;
}

bool nd_downloads_album_progress(const char *album_id, int *completed, int *total)
{
    pthread_mutex_lock(&g_mutex);
    bool active = nd_id_eq(g_active_album_id, album_id);
    if (active) {
        *completed = g_active_completed;
        *total     = g_active_total;
    }
    pthread_mutex_unlock(&g_mutex);
    return active;
}

/* ------------------------------------------------------------------
 * Offline browse query functions
 * ------------------------------------------------------------------ */

int nd_downloads_get_artists(NdArtist *out, int out_max)
{
    if (!out || out_max <= 0) return 0;

    pthread_mutex_lock(&g_mutex);

    if (g_album_count == 0) {
        pthread_mutex_unlock(&g_mutex);
        return 0;
    }

    /* Collect unique (artist_id, artist_name) pairs */
    int   unique_count = 0;
    char (*artist_ids)[ND_MAX_ID] = malloc(g_album_count * sizeof(*artist_ids));
    char (*artist_names)[ND_MAX_STR] = malloc(g_album_count * sizeof(*artist_names));
    if (!artist_ids || !artist_names) {
        free(artist_ids);
        free(artist_names);
        pthread_mutex_unlock(&g_mutex);
        return 0;
    }

    for (int i = 0; i < g_album_count; i++) {
        if (nd_id_eq(g_albums[i].album_id, ND_FAVORITES_SYNC_ALBUM_ID)) continue;
        bool found = false;
        for (int j = 0; j < unique_count; j++) {
            if (nd_id_eq(artist_ids[j], g_albums[i].artist_id)) {
                found = true;
                break;
            }
        }
        if (!found && unique_count < g_album_count) {
            nd_id_copy(artist_ids[unique_count], g_albums[i].artist_id);
            strncpy(artist_names[unique_count], g_albums[i].artist_name,
                    ND_MAX_STR - 1);
            artist_names[unique_count][ND_MAX_STR - 1] = '\0';
            unique_count++;
        }
    }

    /* Insertion sort by name (list is small) */
    for (int i = 1; i < unique_count; i++) {
        char tmp_id[ND_MAX_ID];
        nd_id_copy(tmp_id, artist_ids[i]);
        char tmp_name[ND_MAX_STR];
        strncpy(tmp_name, artist_names[i], sizeof(tmp_name) - 1);
        tmp_name[sizeof(tmp_name) - 1] = '\0';

        int j = i - 1;
        while (j >= 0 && strcasecmp(artist_names[j], tmp_name) > 0) {
            nd_id_copy(artist_ids[j + 1], artist_ids[j]);
            strncpy(artist_names[j + 1], artist_names[j],
                    sizeof(artist_names[j + 1]) - 1);
            artist_names[j + 1][sizeof(artist_names[j + 1]) - 1] = '\0';
            j--;
        }
        nd_id_copy(artist_ids[j + 1], tmp_id);
        strncpy(artist_names[j + 1], tmp_name, sizeof(artist_names[j + 1]) - 1);
        artist_names[j + 1][sizeof(artist_names[j + 1]) - 1] = '\0';
    }

    int n = unique_count < out_max ? unique_count : out_max;
    for (int i = 0; i < n; i++) {
        memset(&out[i], 0, sizeof(out[i]));
        nd_id_copy(out[i].id, artist_ids[i]);
        strncpy(out[i].title, artist_names[i], sizeof(out[i].title) - 1);
    }

    free(artist_ids);
    free(artist_names);
    pthread_mutex_unlock(&g_mutex);
    return n;
}

int nd_downloads_get_albums_for_artist(const char *artist_id,
                                         NdAlbum *out, int out_max)
{
    if (!out || out_max <= 0) return 0;

    pthread_mutex_lock(&g_mutex);

    int n = 0;
    for (int i = 0; i < g_album_count && n < out_max; i++) {
        if (nd_id_eq(g_albums[i].album_id, ND_FAVORITES_SYNC_ALBUM_ID)) continue;
        if (!nd_id_eq(g_albums[i].artist_id, artist_id)) continue;
        memset(&out[n], 0, sizeof(out[n]));
        nd_id_copy(out[n].id, g_albums[i].album_id);
        nd_id_copy(out[n].artist_id, g_albums[i].artist_id);
        strncpy(out[n].title,  g_albums[i].album_title, sizeof(out[n].title) - 1);
        strncpy(out[n].artist, g_albums[i].artist_name, sizeof(out[n].artist) - 1);
        strncpy(out[n].thumb,  g_albums[i].thumb,       sizeof(out[n].thumb) - 1);
        strncpy(out[n].year,   g_albums[i].year,        sizeof(out[n].year) - 1);
        n++;
    }

    pthread_mutex_unlock(&g_mutex);
    return n;
}

static int cmp_album_year_desc(const void *a, const void *b)
{
    const NdAlbum *pa = (const NdAlbum *)a;
    const NdAlbum *pb = (const NdAlbum *)b;
    int ya = pa->year[0] ? atoi(pa->year) : 0;
    int yb = pb->year[0] ? atoi(pb->year) : 0;
    /* Descending: higher year first; albums with year 0 sort to end */
    return yb - ya;
}

int nd_downloads_get_all_albums(NdAlbum *out, int out_max)
{
    if (!out || out_max <= 0) return 0;

    pthread_mutex_lock(&g_mutex);

    int n = 0;
    for (int i = 0; i < g_album_count && n < out_max; i++) {
        if (nd_id_eq(g_albums[i].album_id, ND_FAVORITES_SYNC_ALBUM_ID)) continue;
        memset(&out[n], 0, sizeof(out[n]));
        nd_id_copy(out[n].id, g_albums[i].album_id);
        nd_id_copy(out[n].artist_id, g_albums[i].artist_id);
        strncpy(out[n].title,  g_albums[i].album_title, sizeof(out[n].title) - 1);
        strncpy(out[n].artist, g_albums[i].artist_name, sizeof(out[n].artist) - 1);
        strncpy(out[n].thumb,  g_albums[i].thumb,       sizeof(out[n].thumb) - 1);
        strncpy(out[n].year,   g_albums[i].year,        sizeof(out[n].year) - 1);
        n++;
    }

    pthread_mutex_unlock(&g_mutex);

    qsort(out, (size_t)n, sizeof(NdAlbum), cmp_album_year_desc);
    return n;
}

void nd_downloads_delete_album(const char *album_id)
{
    pthread_mutex_lock(&g_mutex);

    int idx = -1;
    for (int i = 0; i < g_album_count; i++) {
        if (nd_id_eq(g_albums[i].album_id, album_id)) { idx = i; break; }
    }
    if (idx < 0) { pthread_mutex_unlock(&g_mutex); return; }

    ManifestAlbum *ma = &g_albums[idx];

    /* Delete track files */
    for (int j = 0; j < ma->track_count; j++)
        remove(g_tracks[ma->track_start + j].local_path);

    /* Remove album directory (best-effort; may fail if non-empty) */
    char album_dir[640];
    album_dir_path(album_id, album_dir, sizeof(album_dir));
    rmdir(album_dir);

    int ts = ma->track_start;
    int tc = ma->track_count;

    /* Remove track pool entries */
    if (tc > 0) {
        memmove(&g_tracks[ts], &g_tracks[ts + tc],
                (g_track_count - ts - tc) * sizeof(ManifestTrack));
        g_track_count -= tc;
        /* Fix track_start for all albums that came after the deleted ones */
        for (int k = 0; k < g_album_count; k++) {
            if (g_albums[k].track_start > ts)
                g_albums[k].track_start -= tc;
        }
    }

    /* Remove album entry */
    if (idx < g_album_count - 1)
        memmove(&g_albums[idx], &g_albums[idx + 1],
                (g_album_count - idx - 1) * sizeof(ManifestAlbum));
    g_album_count--;

    manifest_save_locked();

    pthread_mutex_unlock(&g_mutex);
}

int nd_downloads_get_tracks_for_album(const char *album_id,
                                        NdTrack *out, int out_max)
{
    if (!out || out_max <= 0) return 0;

    pthread_mutex_lock(&g_mutex);

    for (int i = 0; i < g_album_count; i++) {
        if (!nd_id_eq(g_albums[i].album_id, album_id)) continue;

        const ManifestAlbum *ma = &g_albums[i];
        int n = ma->track_count < out_max ? ma->track_count : out_max;

        for (int j = 0; j < n; j++) {
            const ManifestTrack *mt = &g_tracks[ma->track_start + j];
            memset(&out[j], 0, sizeof(out[j]));
            nd_id_copy(out[j].id, mt->track_id);
            out[j].track_number = mt->track_number;
            out[j].duration_ms  = mt->duration_ms;
            strncpy(out[j].title,      mt->title,      sizeof(out[j].title) - 1);
            strncpy(out[j].artist,     mt->artist,     sizeof(out[j].artist) - 1);
            strncpy(out[j].album,      mt->album,      sizeof(out[j].album) - 1);
            strncpy(out[j].thumb,      mt->thumb,      sizeof(out[j].thumb) - 1);
            strncpy(out[j].local_path, mt->local_path, sizeof(out[j].local_path) - 1);
            strncpy(out[j].media_key,  mt->media_key,  sizeof(out[j].media_key) - 1);
            out[j].audio_bitrate_kbps = mt->bitrate_kbps;
            /* Derive codec from local_path extension (e.g. ".opus" -> "opus") */
            const char *dot = strrchr(mt->local_path, '.');
            if (dot && *(dot + 1))
                strncpy(out[j].audio_codec, dot + 1, sizeof(out[j].audio_codec) - 1);
        }
        pthread_mutex_unlock(&g_mutex);
        return n;
    }

    pthread_mutex_unlock(&g_mutex);
    return 0;
}

static void manifest_track_to_nd(int track_idx, NdTrack *out)
{
    const ManifestTrack *mt = &g_tracks[track_idx];
    memset(out, 0, sizeof(*out));
    nd_id_copy(out->id, mt->track_id);
    out->track_number = mt->track_number;
    out->duration_ms  = mt->duration_ms;
    strncpy(out->title,      mt->title,      sizeof(out->title) - 1);
    strncpy(out->artist,     mt->artist,     sizeof(out->artist) - 1);
    strncpy(out->album,      mt->album,      sizeof(out->album) - 1);
    strncpy(out->thumb,      mt->thumb,      sizeof(out->thumb) - 1);
    strncpy(out->local_path, mt->local_path, sizeof(out->local_path) - 1);
    strncpy(out->media_key,  mt->media_key,  sizeof(out->media_key) - 1);
    out->audio_bitrate_kbps = mt->bitrate_kbps;
    /* Derive codec from local_path extension (e.g. ".opus" -> "opus") */
    const char *dot = strrchr(mt->local_path, '.');
    if (dot && *(dot + 1))
        strncpy(out->audio_codec, dot + 1, sizeof(out->audio_codec) - 1);
}

int nd_downloads_get_favorite_tracks(NdTrack *out, int out_max)
{
    if (!out || out_max <= 0) return 0;

    pthread_mutex_lock(&g_mutex);
    int n = 0;

    /* Pass 1: favorites-sync album entries take priority */
    for (int a = 0; a < g_album_count && n < out_max; a++) {
        if (!nd_id_eq(g_albums[a].album_id, ND_FAVORITES_SYNC_ALBUM_ID)) continue;
        const ManifestAlbum *ma = &g_albums[a];
        for (int j = 0; j < ma->track_count && n < out_max; j++) {
            int ti = ma->track_start + j;
            if (!nd_favorites_contains(g_tracks[ti].track_id)) continue;
            manifest_track_to_nd(ti, &out[n++]);
        }
    }

    /* Pass 2: regular album entries — only if not already added from pass 1 */
    for (int a = 0; a < g_album_count && n < out_max; a++) {
        if (nd_id_eq(g_albums[a].album_id, ND_FAVORITES_SYNC_ALBUM_ID)) continue;
        const ManifestAlbum *ma = &g_albums[a];
        for (int j = 0; j < ma->track_count && n < out_max; j++) {
            int ti = ma->track_start + j;
            if (!nd_favorites_contains(g_tracks[ti].track_id)) continue;
            bool already = false;
            for (int k = 0; k < n; k++) {
                if (nd_id_eq(out[k].id, g_tracks[ti].track_id)) { already = true; break; }
            }
            if (!already) manifest_track_to_nd(ti, &out[n++]);
        }
    }

    pthread_mutex_unlock(&g_mutex);
    return n;
}
