#pragma once

#include "nd_models.h"
#include <stdbool.h>

#define ND_MAX_FAVORITES 512

/*
 * Favorites = Navidrome "starred" songs, mirrored locally so they work offline.
 *
 * - Toggling while online stars/unstars on the server right away (background).
 * - Toggling while offline is queued and pushed on the next sync.
 * - nd_favorites_sync_async() pushes queued changes, then replaces the local
 *   list with the server's starred songs.
 */

void nd_favorites_init(void);
void nd_favorites_quit(void);

/* Add if not present, remove if present. Returns true if now favorited. */
bool nd_favorites_toggle(const NdTrack *t);

bool nd_favorites_contains(const char *track_id);

/* Copy current list (insertion order) into out[0..max-1]. Returns count written. */
int  nd_favorites_get(NdTrack *out, int max);

int  nd_favorites_count(void);

/* Start a background two-way sync with the server. No-op if offline, not
 * signed in, or a sync is already running. */
void nd_favorites_sync_async(void);

/* Incremented each time a sync changes the list (lets UIs refresh). */
int  nd_favorites_generation(void);
