#ifndef ND_API_H
#define ND_API_H

#include <stdbool.h>
#include "nd_models.h"

/*
 * Subsonic / OpenSubsonic API client for Navidrome.
 *
 * Every request authenticates with u=<username>, t=md5(password+salt), s=<salt>
 * and asks for JSON (f=json). All functions return 0 on success, -1 on error,
 * and are safe to call from worker threads (no shared mutable state besides
 * the salt counter, which is atomic).
 */

/*
 * Check the server and credentials (GET /rest/ping).
 * On failure, a human-readable reason is written to err (may be NULL).
 */
int nd_api_ping(const NdConfig *cfg, char *err, int err_size);

/* List music folders (GET /rest/getMusicFolders). Sets *count. */
int nd_api_get_libraries(const NdConfig *cfg, NdLibrary libs[], int *count);

/*
 * Artists (GET /rest/getArtists), sliced to [offset, offset+max_count).
 * Subsonic returns the whole list in one response; page->total is its full
 * length. section_id is the music folder id (0 = all folders).
 */
int nd_api_get_artists(const NdConfig *cfg, int section_id,
                       int offset, int max_count,
                       NdArtist artists[], NdPage *page);

/* Albums for an artist (GET /rest/getArtist). */
int nd_api_get_albums(const NdConfig *cfg, const char *artist_id,
                      NdAlbum albums[], int *count);

/* Tracks for an album (GET /rest/getAlbum). */
int nd_api_get_tracks(const NdConfig *cfg, const char *album_id,
                      NdTrack tracks[], int *count);

/*
 * One page of all albums, newest release year first
 * (GET /rest/getAlbumList2?type=byYear). Subsonic gives no total count, so
 * page->total is offset + count, plus one more page (500) when the page came
 * back full (meaning "there may be more").
 */
int nd_api_get_all_albums(const NdConfig *cfg, int section_id,
                          int offset, int max_count,
                          NdAlbum albums[], NdPage *page);

/* Recently added albums (GET /rest/getAlbumList2?type=newest). */
int nd_api_get_recent_albums(const NdConfig *cfg, int section_id,
                             int max_count, NdAlbum albums[], int *count);

/* Starred songs (GET /rest/getStarred2). Sets *count. */
int nd_api_get_starred_tracks(const NdConfig *cfg, NdTrack tracks[],
                              int max_count, int *count);

/* Original-quality stream URL (stream?format=raw). */
void nd_api_get_stream_url(const NdConfig *cfg, const NdTrack *track,
                           char *out_url, int out_url_size);

/* Opus transcode stream URL (stream?format=opus&maxBitRate=kbps). */
void nd_api_get_transcode_url(const NdConfig *cfg, const NdTrack *track,
                              int bitrate_kbps, char *out_url, int out_url_size);

/*
 * Bitrate to stream this track at: the user's setting, or 192 kbps Opus when
 * the original format can't be decoded on the device (ALAC, WMA, APE, ...).
 * 0 means stream the original file.
 */
int nd_api_stream_bitrate(const NdConfig *cfg, const NdTrack *track);

/* Same idea for downloads, using cfg->download_bitrate_kbps. */
int nd_api_download_bitrate(const NdConfig *cfg, const NdTrack *track);

/* File extension the player should use for the original file ("mp3", "flac", ...). */
void nd_api_track_ext(const NdTrack *track, char *out, int out_size);

/* Stream URL honoring nd_api_stream_bitrate() (raw or Opus transcode). */
void nd_api_get_play_url(const NdConfig *cfg, const NdTrack *track,
                         char *out_url, int out_url_size);

/* Cover art URL for a coverArt id. size <= 0 means original size. */
void nd_api_get_cover_art_url(const NdConfig *cfg, const char *cover_id, int size,
                              char *out_url, int out_url_size);

/* Report "now playing" (scrobble?submission=false). Fire-and-forget. */
void nd_api_now_playing(const NdConfig *cfg, const char *track_id);

/* Record a finished play (scrobble?submission=true). Fire-and-forget. */
void nd_api_scrobble(const NdConfig *cfg, const char *track_id);

/* Star or unstar a song on the server. Fire-and-forget; returns 0 on success. */
int nd_api_set_starred(const NdConfig *cfg, const char *track_id, bool starred);

/*
 * Build a full authenticated URL: {server}/rest/{endpoint}?{auth}&{query}.
 * query may be NULL or "". Returns 0 on success, -1 if it did not fit.
 * Exposed for downloads/art code that streams directly to files.
 */
int nd_api_build_url(const NdConfig *cfg, const char *endpoint, const char *query,
                     char *out_url, int out_url_size);

/* Percent-encode src into dst (RFC 3986 unreserved chars pass through). */
void nd_api_urlencode(const char *src, char *dst, int dst_size);

#endif /* ND_API_H */
