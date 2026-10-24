#ifndef ND_MODELS_H
#define ND_MODELS_H

#include <stdbool.h>
#include <string.h>

#define ND_MAX_STR         256
#define ND_MAX_URL         512
#define ND_MAX_ID           64  /* Subsonic ids are strings (Navidrome: ~22 chars) */
#define ND_MAX_ITEMS       50000  /* sanity ceiling for dynamic lists */
#define ND_MAX_ARTIST_ALBUMS 500  /* static per-artist album array */
#define ND_MAX_TRACKS        500  /* static per-album track array */
#define ND_MAX_LIBRARIES      16  /* static music folder list */
#define ND_MAX_OFFLINE_ITEMS 5000 /* offline artist/album array cap */

/* String id helpers. Ids are plain NUL-terminated strings; "" means "none". */
static inline bool nd_id_eq(const char *a, const char *b)
{
    return a && b && strcmp(a, b) == 0;
}
static inline bool nd_id_empty(const char *a) { return !a || a[0] == '\0'; }
static inline void nd_id_copy(char *dst, const char *src)
{
    if (!src) { dst[0] = '\0'; return; }
    strncpy(dst, src, ND_MAX_ID - 1);
    dst[ND_MAX_ID - 1] = '\0';
}

typedef struct {
    char server_url[ND_MAX_URL];   /* e.g. http://192.168.1.10:4533 (no trailing slash) */
    char username[ND_MAX_STR];
    char password[ND_MAX_STR];     /* stored in config.json; sent only as md5(password+salt) */
    char server_name[ND_MAX_STR];  /* display only */
    bool offline_mode;             /* true = offline browse; persisted to config.json */
    int  screen_timeout;           /* seconds before screen sleeps; 0 = disabled */
    int  library_id;               /* Subsonic musicFolderId; 0 = all folders */
    char library_name[ND_MAX_STR]; /* display name of the selected music folder */
    int  stream_bitrate_kbps;      /* 0 = Original; 96/128/192/256/320 = transcode to Opus */
    int  download_bitrate_kbps;    /* same, for offline downloads */
    bool pocket_lock_enabled;      /* true = MENU+SELECT required to wake; false = any button */
    int  preload_count;            /* 0 = disabled; 1–10 = tracks to preload ahead */
} NdConfig;

typedef struct {
    int  section_id;                  /* Subsonic musicFolder id */
    char title[ND_MAX_STR];
    char type[32];
} NdLibrary;

typedef struct {
    char id[ND_MAX_ID];
    char title[ND_MAX_STR];
    char title_sort[ND_MAX_STR];   /* index letter + name; used for L1/R1 group jumps */
    char thumb[ND_MAX_URL];        /* Subsonic coverArt id */
} NdArtist;

typedef struct {
    char id[ND_MAX_ID];
    char artist_id[ND_MAX_ID];
    char title[ND_MAX_STR];
    char artist[ND_MAX_STR];
    char year[8];
    char thumb[ND_MAX_URL];        /* Subsonic coverArt id */
} NdAlbum;

typedef struct {
    char id[ND_MAX_ID];
    int  duration_ms;
    int  track_number;
    char title[ND_MAX_STR];
    char artist[ND_MAX_STR];
    char album[ND_MAX_STR];
    char media_key[ND_MAX_URL];    /* unused for Subsonic; kept for manifest compatibility */
    char thumb[ND_MAX_URL];        /* Subsonic coverArt id */
    char local_path[768];          /* non-empty = offline: skip download, play this file */
    char audio_codec[32];          /* file suffix, e.g. "flac", "mp3", "opus", "m4a" */
    int  audio_bitrate_kbps;       /* 0 if unknown */
} NdTrack;

/* Generic paginated result */
typedef struct {
    int total;
    int offset;
    int count;
} NdPage;

#endif /* ND_MODELS_H */
