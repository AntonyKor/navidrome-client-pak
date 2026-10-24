#ifndef ND_DOWNLOADS_H
#define ND_DOWNLOADS_H

#include <stdbool.h>
#include "nd_models.h"
#include "nd_config.h"

typedef enum {
    DL_STATUS_NONE        = 0,  /* not in queue and not downloaded */
    DL_STATUS_QUEUED      = 1,  /* in queue, not yet started */
    DL_STATUS_DOWNLOADING = 2,  /* currently downloading */
    DL_STATUS_DONE        = 3,  /* fully downloaded and in manifest */
} DlStatus;

/* Call once at startup. Loads manifest from disk. Starts worker thread. */
void nd_downloads_init(void);

/* Call at shutdown. Signals worker to stop, joins thread. */
void nd_downloads_quit(void);

/*
 * Queue an album for download. No-op if already downloaded (DL_STATUS_DONE)
 * or queue is full. cfg provides server_url + credentials; album_id and
 * album_title identify the album; artist_id/artist_name come from the browse
 * module; thumb is the album coverArt id.
 */
void nd_downloads_queue_album(const NdConfig *cfg,
                                const char *album_id,
                                const char *album_title,
                                const char *artist_id,
                                const char *artist_name,
                                const char *thumb,
                                const char *year);

/* Returns the download status for the given album id. */
DlStatus nd_downloads_album_status(const char *album_id);

/* Returns true if any album is currently being downloaded. */
bool nd_downloads_is_active(void);

/*
 * If album_id is the currently-downloading album, sets *completed and *total
 * and returns true. Otherwise returns false (caller should not use the output
 * values). Safe to call from any thread.
 */
bool nd_downloads_album_progress(const char *album_id, int *completed, int *total);

/*
 * Offline browse query functions. All return data from the in-memory manifest.
 * Caller provides output arrays; returns count of items written.
 * Max items written is capped at out_max.
 * These are safe to call from the main thread at any time.
 */

/* Unique artists in the manifest, sorted by name. */
int nd_downloads_get_artists(NdArtist *out, int out_max);

/* Albums for a given artist_id (from manifest). */
int nd_downloads_get_albums_for_artist(const char *artist_id,
                                         NdAlbum *out, int out_max);

/*
 * Tracks for a given album_id (from manifest).
 * Populates NdTrack fields including local_path.
 */
int nd_downloads_get_tracks_for_album(const char *album_id,
                                        NdTrack *out, int out_max);

/*
 * All downloaded albums, sorted by year descending (albums without a year
 * sort to the end). Returns count of items written, capped at out_max.
 */
int nd_downloads_get_all_albums(NdAlbum *out, int out_max);

/*
 * Delete a downloaded album: removes all track files, the album directory,
 * the manifest entry, and saves the updated manifest. No-op if album_id not found.
 * Safe to call from the main thread (locks g_mutex internally).
 */
void nd_downloads_delete_album(const char *album_id);

/*
 * All downloaded tracks whose id appears in the favorites list,
 * drawn from every album in the manifest (not just the favorites-sync album).
 * Safe to call from the main thread. Returns count written, capped at out_max.
 */
int nd_downloads_get_favorite_tracks(NdTrack *out, int out_max);

/* Reserved album_id for the favorites sync manifest entry */
#define ND_FAVORITES_SYNC_ALBUM_ID "__favorites__"

/* Initiate a bidirectional favorites sync (async, non-blocking).
 * - Deletes files for tracks currently in the favorites album that are no
 *   longer in favs[].
 * - Downloads tracks in favs[] whose file does not yet exist on disk.
 * - No-ops if a sync is already in progress. */
void nd_downloads_sync_favorites(const NdConfig *cfg,
                                   const NdTrack *favs, int fav_count);

#endif /* ND_DOWNLOADS_H */
