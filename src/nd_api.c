#define _GNU_SOURCE
#include "nd_api.h"
#include "nd_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <strings.h>

#include "include/parson/parson.h"
#include "mbedtls/md5.h"
#include "nd_log.h"

#define ND_API_VERSION  "1.16.1"
#define ND_CLIENT_NAME  "NavidromeMusicPak"

/* Response buffer: getArtists returns the whole library in one response. */
#define RESP_BUF_SIZE (4 * 1024 * 1024)
/* getAlbumList2 maximum page size */
#define ALBUM_PAGE_SIZE 500
/* Small responses (ping, scrobble, star) */
#define SMALL_BUF_SIZE (16 * 1024)

/* ------------------------------------------------------------------
 * URL / auth helpers
 * ------------------------------------------------------------------ */

void nd_api_urlencode(const char *src, char *dst, int dst_size)
{
    static const char hex[] = "0123456789ABCDEF";
    int o = 0;
    if (!dst || dst_size <= 0) return;
    for (const unsigned char *p = (const unsigned char *)(src ? src : ""); *p; p++) {
        unsigned char c = *p;
        bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                     (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                     c == '.' || c == '~';
        if (plain) {
            if (o + 1 >= dst_size) break;
            dst[o++] = (char)c;
        } else {
            if (o + 3 >= dst_size) break;
            dst[o++] = '%';
            dst[o++] = hex[c >> 4];
            dst[o++] = hex[c & 15];
        }
    }
    dst[o] = '\0';
}

/* Fresh random salt per request: hex of 8 random-ish bytes. */
static void make_salt(char out[17])
{
    static volatile unsigned int s_counter = 0;
    unsigned int n = __sync_add_and_fetch(&s_counter, 1);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t x = ((uint64_t)ts.tv_sec << 32) ^ (uint64_t)ts.tv_nsec ^
                 ((uint64_t)n * 0x9E3779B97F4A7C15ULL) ^ ((uint64_t)getpid() << 16);
    /* splitmix64 */
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    x ^= x >> 31;
    snprintf(out, 17, "%016llx", (unsigned long long)x);
}

static void md5_hex(const char *in, char out[33])
{
    unsigned char digest[16];
    mbedtls_md5((const unsigned char *)in, strlen(in), digest);
    for (int i = 0; i < 16; i++)
        snprintf(out + i * 2, 3, "%02x", digest[i]);
}

int nd_api_build_url(const NdConfig *cfg, const char *endpoint, const char *query,
                     char *out_url, int out_url_size)
{
    if (!cfg || !endpoint || !out_url || out_url_size <= 0) return -1;

    char salt[17];
    make_salt(salt);

    char salted[ND_MAX_STR + 20];
    snprintf(salted, sizeof(salted), "%s%s", cfg->password, salt);
    char token[33];
    md5_hex(salted, token);

    char user_enc[ND_MAX_STR * 3];
    nd_api_urlencode(cfg->username, user_enc, sizeof(user_enc));

    /* Strip a trailing slash from the server URL so we don't emit "//rest". */
    int base_len = (int)strlen(cfg->server_url);
    while (base_len > 0 && cfg->server_url[base_len - 1] == '/') base_len--;

    int n = snprintf(out_url, out_url_size,
                     "%.*s/rest/%s?u=%s&t=%s&s=%s&v=" ND_API_VERSION
                     "&c=" ND_CLIENT_NAME "&f=json%s%s",
                     base_len, cfg->server_url, endpoint, user_enc, token, salt,
                     (query && *query) ? "&" : "", query ? query : "");
    if (n < 0 || n >= out_url_size) {
        ND_LOG_ERROR("[NdAPI] URL too long for endpoint: %s\n", endpoint);
        out_url[0] = '\0';
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------
 * Request helper.
 * Fetches {endpoint}, checks the subsonic-response status, and returns the
 * parsed root (caller frees with json_value_free) plus the inner
 * "subsonic-response" object via *out_resp. On failure, writes a reason to
 * err (if non-NULL) and returns NULL.
 * ------------------------------------------------------------------ */
static JSON_Value *api_get(const NdConfig *cfg, const char *endpoint,
                           const char *query, int buf_size, int timeout_sec,
                           JSON_Object **out_resp, char *err, int err_size)
{
    if (err && err_size > 0) err[0] = '\0';

    char url[2048];
    if (nd_api_build_url(cfg, endpoint, query, url, sizeof(url)) != 0) {
        if (err) snprintf(err, err_size, "Server URL too long");
        return NULL;
    }

    uint8_t *buf = (uint8_t *)malloc(buf_size);
    if (!buf) return NULL;

    NdNetOptions opts = {
        .method      = ND_HTTP_GET,
        .body        = NULL,
        .token       = NULL,
        .timeout_sec = timeout_sec,
        .no_persist  = true
    };

    int n = nd_net_fetch(url, buf, buf_size, &opts);
    if (n <= 0) {
        /* Log endpoint only: the full URL carries the auth token */
        ND_LOG_ERROR("[NdAPI] fetch failed: %s\n", endpoint);
        if (err) snprintf(err, err_size, "Cannot reach server");
        free(buf);
        return NULL;
    }
    if (n >= buf_size) n = buf_size - 1;
    buf[n] = '\0';

    JSON_Value *root = json_parse_string((const char *)buf);
    free(buf);
    if (!root) {
        ND_LOG_ERROR("[NdAPI] JSON parse failed: %s\n", endpoint);
        if (err) snprintf(err, err_size, "Not a Subsonic server (bad response)");
        return NULL;
    }

    JSON_Object *resp = json_object_get_object(json_value_get_object(root),
                                               "subsonic-response");
    if (!resp) {
        ND_LOG_ERROR("[NdAPI] missing subsonic-response: %s\n", endpoint);
        if (err) snprintf(err, err_size, "Not a Subsonic server");
        json_value_free(root);
        return NULL;
    }

    const char *status = json_object_get_string(resp, "status");
    if (!status || strcmp(status, "ok") != 0) {
        JSON_Object *e = json_object_get_object(resp, "error");
        const char *msg = e ? json_object_get_string(e, "message") : NULL;
        int code = e ? (int)json_object_get_number(e, "code") : 0;
        ND_LOG_ERROR("[NdAPI] %s failed: code=%d %s\n", endpoint, code,
                     msg ? msg : "(no message)");
        if (err) {
            if (code == 40 || code == 41 || code == 44)
                snprintf(err, err_size, "Wrong username or password");
            else
                snprintf(err, err_size, "%s", msg ? msg : "Server error");
        }
        json_value_free(root);
        return NULL;
    }

    if (out_resp) *out_resp = resp;
    return root;
}

/* Fire-and-forget GET; returns 0 if the server answered "ok". */
static int api_call_simple(const NdConfig *cfg, const char *endpoint, const char *query)
{
    JSON_Object *resp = NULL;
    JSON_Value *root = api_get(cfg, endpoint, query, SMALL_BUF_SIZE, 5, &resp, NULL, 0);
    if (!root) return -1;
    json_value_free(root);
    return 0;
}

static void copy_str(char *dst, size_t dst_size, const char *src)
{
    if (!src) { dst[0] = '\0'; return; }
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

/* Subsonic ids are strings in Navidrome but numbers in some servers. */
static void get_id(JSON_Object *o, const char *key, char out[ND_MAX_ID])
{
    JSON_Value *v = json_object_get_value(o, key);
    out[0] = '\0';
    if (!v) return;
    if (json_value_get_type(v) == JSONString)
        nd_id_copy(out, json_value_get_string(v));
    else if (json_value_get_type(v) == JSONNumber)
        snprintf(out, ND_MAX_ID, "%.0f", json_value_get_number(v));
}

static void parse_album(JSON_Object *item, NdAlbum *al)
{
    memset(al, 0, sizeof(*al));
    get_id(item, "id", al->id);
    get_id(item, "artistId", al->artist_id);
    const char *title = json_object_get_string(item, "name");
    if (!title) title = json_object_get_string(item, "title");
    copy_str(al->title, sizeof(al->title), title);
    copy_str(al->artist, sizeof(al->artist), json_object_get_string(item, "artist"));
    copy_str(al->thumb, sizeof(al->thumb), json_object_get_string(item, "coverArt"));
    double year = json_object_get_number(item, "year");
    if (year > 0) snprintf(al->year, sizeof(al->year), "%d", (int)year);
}

static void parse_song(JSON_Object *item, NdTrack *t)
{
    memset(t, 0, sizeof(*t));
    get_id(item, "id", t->id);
    t->duration_ms  = (int)json_object_get_number(item, "duration") * 1000;
    t->track_number = (int)json_object_get_number(item, "track");
    copy_str(t->title,  sizeof(t->title),  json_object_get_string(item, "title"));
    copy_str(t->artist, sizeof(t->artist), json_object_get_string(item, "artist"));
    copy_str(t->album,  sizeof(t->album),  json_object_get_string(item, "album"));
    copy_str(t->thumb,  sizeof(t->thumb),  json_object_get_string(item, "coverArt"));
    copy_str(t->audio_codec, sizeof(t->audio_codec), json_object_get_string(item, "suffix"));
    t->audio_bitrate_kbps = (int)json_object_get_number(item, "bitRate");
}

static void folder_query(int section_id, char *out, int out_size)
{
    if (section_id > 0) snprintf(out, out_size, "musicFolderId=%d", section_id);
    else out[0] = '\0';
}

/* ------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------ */

int nd_api_ping(const NdConfig *cfg, char *err, int err_size)
{
    if (!cfg) return -1;
    JSON_Object *resp = NULL;
    JSON_Value *root = api_get(cfg, "ping", NULL, SMALL_BUF_SIZE, 10, &resp, err, err_size);
    if (!root) return -1;
    json_value_free(root);
    return 0;
}

int nd_api_get_libraries(const NdConfig *cfg, NdLibrary libs[], int *count)
{
    if (!cfg || !libs || !count) return -1;
    *count = 0;

    JSON_Object *resp = NULL;
    JSON_Value *root = api_get(cfg, "getMusicFolders", NULL, SMALL_BUF_SIZE, 15,
                               &resp, NULL, 0);
    if (!root) return -1;

    JSON_Array *arr = json_object_dotget_array(resp, "musicFolders.musicFolder");
    size_t n = arr ? json_array_get_count(arr) : 0;
    for (size_t i = 0; i < n && *count < ND_MAX_LIBRARIES; i++) {
        JSON_Object *f = json_array_get_object(arr, i);
        if (!f) continue;
        NdLibrary *lib = &libs[*count];
        memset(lib, 0, sizeof(*lib));
        char id[ND_MAX_ID];
        get_id(f, "id", id);
        lib->section_id = atoi(id);
        copy_str(lib->title, sizeof(lib->title), json_object_get_string(f, "name"));
        copy_str(lib->type, sizeof(lib->type), "music");
        (*count)++;
    }

    json_value_free(root);
    return 0;
}

int nd_api_get_artists(const NdConfig *cfg, int section_id,
                       int offset, int max_count,
                       NdArtist artists[], NdPage *page)
{
    if (!cfg || !artists || !page) return -1;
    memset(page, 0, sizeof(*page));
    page->offset = offset;
    if (max_count <= 0) return 0;

    char q[64];
    folder_query(section_id, q, sizeof(q));

    /* getArtists always returns the whole list; we slice [offset, offset+max_count)
     * out of it so callers can page through it like any other endpoint. */
    JSON_Object *resp = NULL;
    JSON_Value *root = api_get(cfg, "getArtists", q, RESP_BUF_SIZE, 30, &resp, NULL, 0);
    if (!root) return -1;

    JSON_Array *index = json_object_dotget_array(resp, "artists.index");
    size_t ni = index ? json_array_get_count(index) : 0;
    int seen = 0, loaded = 0;
    for (size_t i = 0; i < ni; i++) {
        JSON_Object *idx = json_array_get_object(index, i);
        if (!idx) continue;
        const char *letter = json_object_get_string(idx, "name");
        JSON_Array *arr = json_object_get_array(idx, "artist");
        size_t na = arr ? json_array_get_count(arr) : 0;
        for (size_t j = 0; j < na; j++) {
            JSON_Object *item = json_array_get_object(arr, j);
            if (!item) continue;
            int pos = seen++;
            if (pos < offset || loaded >= max_count) continue;
            NdArtist *a = &artists[loaded];
            memset(a, 0, sizeof(*a));
            get_id(item, "id", a->id);
            const char *name = json_object_get_string(item, "name");
            copy_str(a->title, sizeof(a->title), name);
            copy_str(a->thumb, sizeof(a->thumb), json_object_get_string(item, "coverArt"));
            /* Sort key: server index letter first so L1/R1 jumps follow the
             * server's grouping (which ignores articles like "The"). */
            snprintf(a->title_sort, sizeof(a->title_sort), "%s %s",
                     letter ? letter : "", name ? name : "");
            loaded++;
        }
    }

    page->count = loaded;
    page->total = seen;
    json_value_free(root);
    return 0;
}

int nd_api_get_albums(const NdConfig *cfg, const char *artist_id,
                      NdAlbum albums[], int *count)
{
    if (!cfg || !albums || !count || nd_id_empty(artist_id)) return -1;
    *count = 0;

    char enc[ND_MAX_ID * 3], q[ND_MAX_ID * 3 + 8];
    nd_api_urlencode(artist_id, enc, sizeof(enc));
    snprintf(q, sizeof(q), "id=%s", enc);

    JSON_Object *resp = NULL;
    JSON_Value *root = api_get(cfg, "getArtist", q, RESP_BUF_SIZE, 15, &resp, NULL, 0);
    if (!root) return -1;

    JSON_Array *arr = json_object_dotget_array(resp, "artist.album");
    size_t n = arr ? json_array_get_count(arr) : 0;
    for (size_t i = 0; i < n && *count < ND_MAX_ARTIST_ALBUMS; i++) {
        JSON_Object *item = json_array_get_object(arr, i);
        if (!item) continue;
        parse_album(item, &albums[*count]);
        if (nd_id_empty(albums[*count].artist_id))
            nd_id_copy(albums[*count].artist_id, artist_id);
        (*count)++;
    }

    json_value_free(root);
    return 0;
}

int nd_api_get_tracks(const NdConfig *cfg, const char *album_id,
                      NdTrack tracks[], int *count)
{
    if (!cfg || !tracks || !count || nd_id_empty(album_id)) return -1;
    *count = 0;

    char enc[ND_MAX_ID * 3], q[ND_MAX_ID * 3 + 8];
    nd_api_urlencode(album_id, enc, sizeof(enc));
    snprintf(q, sizeof(q), "id=%s", enc);

    JSON_Object *resp = NULL;
    JSON_Value *root = api_get(cfg, "getAlbum", q, RESP_BUF_SIZE, 15, &resp, NULL, 0);
    if (!root) return -1;

    JSON_Array *arr = json_object_dotget_array(resp, "album.song");
    size_t n = arr ? json_array_get_count(arr) : 0;
    for (size_t i = 0; i < n && *count < ND_MAX_TRACKS; i++) {
        JSON_Object *item = json_array_get_object(arr, i);
        if (!item) continue;
        parse_song(item, &tracks[*count]);
        (*count)++;
    }

    json_value_free(root);
    return 0;
}

/* Shared getAlbumList2 fetch. */
static int album_list(const NdConfig *cfg, const char *type_query, int section_id,
                      int offset, int max_count, NdAlbum albums[], int *loaded)
{
    *loaded = 0;
    int size = max_count > ALBUM_PAGE_SIZE ? ALBUM_PAGE_SIZE : max_count;

    char folder[64], q[256];
    folder_query(section_id, folder, sizeof(folder));
    snprintf(q, sizeof(q), "%s&size=%d&offset=%d%s%s", type_query, size, offset,
             folder[0] ? "&" : "", folder);

    JSON_Object *resp = NULL;
    JSON_Value *root = api_get(cfg, "getAlbumList2", q, RESP_BUF_SIZE, 20, &resp, NULL, 0);
    if (!root) return -1;

    JSON_Array *arr = json_object_dotget_array(resp, "albumList2.album");
    size_t n = arr ? json_array_get_count(arr) : 0;
    for (size_t i = 0; i < n && *loaded < size; i++) {
        JSON_Object *item = json_array_get_object(arr, i);
        if (!item) continue;
        parse_album(item, &albums[*loaded]);
        (*loaded)++;
    }

    json_value_free(root);
    return 0;
}

int nd_api_get_all_albums(const NdConfig *cfg, int section_id,
                          int offset, int max_count,
                          NdAlbum albums[], NdPage *page)
{
    if (!cfg || !albums || !page) return -1;
    memset(page, 0, sizeof(*page));
    page->offset = offset;
    if (max_count <= 0) return 0;

    /* fromYear > toYear sorts newest first */
    int size = max_count > ALBUM_PAGE_SIZE ? ALBUM_PAGE_SIZE : max_count;
    int loaded = 0;
    if (album_list(cfg, "type=byYear&fromYear=3000&toYear=0", section_id,
                   offset, size, albums, &loaded) != 0)
        return -1;

    page->count = loaded;
    /* No total from the server. A full page means "probably more": report a
     * whole extra page so the caller grows its buffer by a page at a time.
     * The short final page then gives the exact total. */
    page->total = offset + loaded + (loaded == size ? ALBUM_PAGE_SIZE : 0);
    return 0;
}

int nd_api_get_recent_albums(const NdConfig *cfg, int section_id,
                             int max_count, NdAlbum albums[], int *count)
{
    if (!cfg || !albums || !count) return -1;
    *count = 0;
    if (max_count <= 0) return 0;
    return album_list(cfg, "type=newest", section_id, 0, max_count, albums, count);
}

int nd_api_get_starred_tracks(const NdConfig *cfg, NdTrack tracks[],
                              int max_count, int *count)
{
    if (!cfg || !tracks || !count) return -1;
    *count = 0;

    JSON_Object *resp = NULL;
    JSON_Value *root = api_get(cfg, "getStarred2", NULL, RESP_BUF_SIZE, 20, &resp, NULL, 0);
    if (!root) return -1;

    JSON_Array *arr = json_object_dotget_array(resp, "starred2.song");
    size_t n = arr ? json_array_get_count(arr) : 0;
    for (size_t i = 0; i < n && *count < max_count; i++) {
        JSON_Object *item = json_array_get_object(arr, i);
        if (!item) continue;
        parse_song(item, &tracks[*count]);
        (*count)++;
    }

    json_value_free(root);
    return 0;
}

static void stream_url(const NdConfig *cfg, const NdTrack *track, const char *fmt_query,
                       char *out_url, int out_url_size)
{
    char enc[ND_MAX_ID * 3], q[ND_MAX_ID * 3 + 128];
    nd_api_urlencode(track->id, enc, sizeof(enc));
    /* Transcodes arrive chunked (no Content-Length); nd_net handles that. */
    snprintf(q, sizeof(q), "id=%s&%s", enc, fmt_query);
    nd_api_build_url(cfg, "stream", q, out_url, out_url_size);
}

void nd_api_get_stream_url(const NdConfig *cfg, const NdTrack *track,
                           char *out_url, int out_url_size)
{
    if (!cfg || !track || !out_url || out_url_size <= 0) return;
    /* format=raw overrides any per-player transcoding set in the Navidrome UI */
    stream_url(cfg, track, "format=raw", out_url, out_url_size);
}

void nd_api_get_transcode_url(const NdConfig *cfg, const NdTrack *track,
                              int bitrate_kbps, char *out_url, int out_url_size)
{
    if (!cfg || !track || !out_url || out_url_size <= 0) return;
    char fmt[64];
    snprintf(fmt, sizeof(fmt), "format=opus&maxBitRate=%d", bitrate_kbps);
    stream_url(cfg, track, fmt, out_url, out_url_size);
}

/* Formats the device decoders handle (see Player_detectFormat). */
static bool suffix_playable(const char *suffix)
{
    static const char *const ok[] = {
        "mp3", "flac", "ogg", "oga", "opus", "wav", "m4a", "mp4", "aac", NULL
    };
    for (int i = 0; ok[i]; i++)
        if (strcasecmp(suffix, ok[i]) == 0) return true;
    return false;
}

#define FALLBACK_TRANSCODE_KBPS 192

int nd_api_stream_bitrate(const NdConfig *cfg, const NdTrack *track)
{
    if (cfg && cfg->stream_bitrate_kbps > 0) return cfg->stream_bitrate_kbps;
    if (track && track->audio_codec[0] && !suffix_playable(track->audio_codec))
        return FALLBACK_TRANSCODE_KBPS;
    return 0;
}

int nd_api_download_bitrate(const NdConfig *cfg, const NdTrack *track)
{
    if (cfg && cfg->download_bitrate_kbps > 0) return cfg->download_bitrate_kbps;
    if (track && track->audio_codec[0] && !suffix_playable(track->audio_codec))
        return FALLBACK_TRANSCODE_KBPS;
    return 0;
}

void nd_api_track_ext(const NdTrack *track, char *out, int out_size)
{
    const char *s = (track && track->audio_codec[0]) ? track->audio_codec : "mp3";
    if (strcasecmp(s, "oga") == 0) s = "ogg";
    else if (strcasecmp(s, "mp4") == 0 || strcasecmp(s, "m4b") == 0) s = "m4a";
    int i = 0;
    for (; s[i] && i < out_size - 1; i++)
        out[i] = (char)((s[i] >= 'A' && s[i] <= 'Z') ? s[i] + 32 : s[i]);
    out[i] = '\0';
}

void nd_api_get_play_url(const NdConfig *cfg, const NdTrack *track,
                         char *out_url, int out_url_size)
{
    int kbps = nd_api_stream_bitrate(cfg, track);
    if (kbps > 0) nd_api_get_transcode_url(cfg, track, kbps, out_url, out_url_size);
    else          nd_api_get_stream_url(cfg, track, out_url, out_url_size);
}

void nd_api_get_cover_art_url(const NdConfig *cfg, const char *cover_id, int size,
                              char *out_url, int out_url_size)
{
    if (!out_url || out_url_size <= 0) return;
    out_url[0] = '\0';
    if (!cfg || nd_id_empty(cover_id)) return;
    char enc[ND_MAX_URL * 3], q[ND_MAX_URL * 3 + 32];
    nd_api_urlencode(cover_id, enc, sizeof(enc));
    if (size > 0) snprintf(q, sizeof(q), "id=%s&size=%d", enc, size);
    else          snprintf(q, sizeof(q), "id=%s", enc);
    nd_api_build_url(cfg, "getCoverArt", q, out_url, out_url_size);
}

static void scrobble(const NdConfig *cfg, const char *track_id, bool submission)
{
    if (!cfg || nd_id_empty(track_id)) return;
    char enc[ND_MAX_ID * 3], q[ND_MAX_ID * 3 + 64];
    nd_api_urlencode(track_id, enc, sizeof(enc));
    snprintf(q, sizeof(q), "id=%s&submission=%s", enc, submission ? "true" : "false");
    api_call_simple(cfg, "scrobble", q);
}

void nd_api_now_playing(const NdConfig *cfg, const char *track_id)
{
    scrobble(cfg, track_id, false);
}

void nd_api_scrobble(const NdConfig *cfg, const char *track_id)
{
    scrobble(cfg, track_id, true);
}

int nd_api_set_starred(const NdConfig *cfg, const char *track_id, bool starred)
{
    if (!cfg || nd_id_empty(track_id)) return -1;
    char enc[ND_MAX_ID * 3], q[ND_MAX_ID * 3 + 8];
    nd_api_urlencode(track_id, enc, sizeof(enc));
    snprintf(q, sizeof(q), "id=%s", enc);
    return api_call_simple(cfg, starred ? "star" : "unstar", q);
}
