#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "nd_queue.h"
#include "nd_api.h"

/* Global singleton queue */
static NdQueue g_queue;

NdQueue *nd_queue_get(void)
{
    return &g_queue;
}

void nd_queue_set(const NdConfig *cfg,
                    const NdTrack *tracks, int count, int start_index)
{
    if (!tracks || count <= 0)
        return;

    if (count > ND_QUEUE_MAX_TRACKS)
        count = ND_QUEUE_MAX_TRACKS;

    memcpy(g_queue.tracks, tracks, count * sizeof(NdTrack));
    g_queue.count = count;
    g_queue.current_index = (start_index >= 0 && start_index < count) ? start_index : 0;
    g_queue.active = true;
    g_queue.shuffle = false;
    g_queue.repeat_mode = REPEAT_OFF;
    for (int i = 0; i < count; i++) g_queue.shuffle_order[i] = i;

    if (g_queue.tracks[g_queue.current_index].local_path[0] != '\0')
        g_queue.stream_url[0] = '\0';
    else
        nd_api_get_play_url(cfg, &g_queue.tracks[g_queue.current_index],
                            g_queue.stream_url, sizeof(g_queue.stream_url));
}

bool nd_queue_next(const NdConfig *cfg)
{
    if (!g_queue.active)
        return false;

    if (g_queue.current_index >= g_queue.count - 1) {
        if (g_queue.repeat_mode == REPEAT_ALL)
            g_queue.current_index = 0;
        else
            return false;
    } else {
        g_queue.current_index++;
    }

    const NdTrack *t = &g_queue.tracks[g_queue.shuffle_order[g_queue.current_index]];
    if (t->local_path[0] != '\0')
        g_queue.stream_url[0] = '\0';
    else
        nd_api_get_play_url(cfg, t,
                            g_queue.stream_url, sizeof(g_queue.stream_url));
    return true;
}

bool nd_queue_prev(const NdConfig *cfg)
{
    if (!g_queue.active || g_queue.current_index <= 0)
        return false;

    g_queue.current_index--;
    const NdTrack *t = &g_queue.tracks[g_queue.shuffle_order[g_queue.current_index]];
    if (t->local_path[0] != '\0')
        g_queue.stream_url[0] = '\0';
    else
        nd_api_get_play_url(cfg, t,
                            g_queue.stream_url, sizeof(g_queue.stream_url));
    return true;
}

bool nd_queue_has_next(void)
{
    return g_queue.active &&
           (g_queue.current_index < g_queue.count - 1 ||
            g_queue.repeat_mode == REPEAT_ALL);
}

bool nd_queue_has_prev(void)
{
    return g_queue.active && g_queue.current_index > 0;
}

void nd_queue_clear(void)
{
    memset(&g_queue, 0, sizeof(g_queue));
}

const NdTrack *nd_queue_current_track(void)
{
    if (!g_queue.active)
        return NULL;
    return &g_queue.tracks[g_queue.shuffle_order[g_queue.current_index]];
}

const NdTrack *nd_queue_peek(int offset)
{
    if (!g_queue.active || offset <= 0) return NULL;
    int idx = g_queue.current_index + offset;
    if (idx >= g_queue.count) return NULL;
    return &g_queue.tracks[g_queue.shuffle_order[idx]];
}

void nd_queue_toggle_shuffle(void)
{
    static bool seeded = false;
    if (!seeded) {
        srand((unsigned)time(NULL));
        seeded = true;
    }

    if (!g_queue.active || g_queue.count <= 0)
        return;

    int count = g_queue.count;

    if (!g_queue.shuffle) {
        /* Fisher-Yates shuffle */
        for (int i = count - 1; i > 0; i--) {
            int j = rand() % (i + 1);
            int tmp = g_queue.shuffle_order[i];
            g_queue.shuffle_order[i] = g_queue.shuffle_order[j];
            g_queue.shuffle_order[j] = tmp;
        }
        /* Keep the currently-playing track at current_index */
        int cur = g_queue.current_index;
        for (int i = 0; i < count; i++) {
            if (g_queue.shuffle_order[i] == cur) {
                /* Swap position i with current_index */
                int tmp = g_queue.shuffle_order[i];
                g_queue.shuffle_order[i] = g_queue.shuffle_order[cur];
                g_queue.shuffle_order[cur] = tmp;
                break;
            }
        }
        g_queue.shuffle = true;
    } else {
        /* Save the real track index we're currently at */
        int real_idx = g_queue.shuffle_order[g_queue.current_index];
        char saved_id[ND_MAX_ID];
        nd_id_copy(saved_id, g_queue.tracks[real_idx].id);

        /* Restore identity order */
        for (int i = 0; i < count; i++)
            g_queue.shuffle_order[i] = i;

        /* Find where the current track now lives */
        for (int i = 0; i < count; i++) {
            if (nd_id_eq(g_queue.tracks[i].id, saved_id)) {
                g_queue.current_index = i;
                break;
            }
        }
        g_queue.shuffle = false;
    }
}

void nd_queue_cycle_repeat(void)
{
    g_queue.repeat_mode = (RepeatMode)((g_queue.repeat_mode + 1) % 3);
}
