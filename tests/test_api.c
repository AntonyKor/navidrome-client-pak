/* Host-side test for nd_api.c: stubs nd_net_fetch to serve JSON fixtures.
 * Build/run: tests/run_tests.sh */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "nd_api.h"
#include "nd_net.h"

static const char *g_fixture_dir;
static char g_last_url[4096];

static const char *fixture_for(const char *url)
{
    if (strstr(url, "/rest/ping"))            return "ping.json";
    if (strstr(url, "/rest/getMusicFolders")) return "folders.json";
    if (strstr(url, "/rest/getArtists"))      return "artists.json";
    if (strstr(url, "/rest/getArtist?"))      return "artist.json";
    if (strstr(url, "id=bogus"))              return "err.json";
    if (strstr(url, "/rest/getAlbum?"))       return "album.json";
    if (strstr(url, "type=byYear"))           return "byyear.json";
    if (strstr(url, "type=newest"))           return "newest.json";
    if (strstr(url, "/rest/getStarred2"))     return "starred.json";
    return NULL;
}

int nd_net_fetch(const char *url, uint8_t *buffer, int buffer_size, const NdNetOptions *opts)
{
    (void)opts;
    snprintf(g_last_url, sizeof(g_last_url), "%s", url);
    const char *f = fixture_for(url);
    if (!f) return -1;
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", g_fixture_dir, f);
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    int n = (int)fread(buffer, 1, buffer_size - 1, fp);
    fclose(fp);
    buffer[n] = 0;
    return n;
}

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

int main(int argc, char **argv)
{
    g_fixture_dir = argc > 1 ? argv[1] : "fixtures";
    NdConfig cfg; memset(&cfg, 0, sizeof(cfg));
    strcpy(cfg.server_url, "https://demo.navidrome.org/");
    strcpy(cfg.username, "de mo");
    strcpy(cfg.password, "demo");

    char err[128];
    CHECK(nd_api_ping(&cfg, err, sizeof(err)) == 0);
    CHECK(strstr(g_last_url, "https://demo.navidrome.org/rest/ping?u=de%20mo&t=") == g_last_url);
    CHECK(strstr(g_last_url, "&f=json"));

    NdLibrary libs[ND_MAX_LIBRARIES]; int nlib = 0;
    CHECK(nd_api_get_libraries(&cfg, libs, &nlib) == 0);
    CHECK(nlib == 1 && libs[0].section_id == 1 && strcmp(libs[0].title, "Music Library") == 0);

    static NdArtist artists[2000]; NdPage page;
    CHECK(nd_api_get_artists(&cfg, 1, 0, 2000, artists, &page) == 0);
    CHECK(strstr(g_last_url, "musicFolderId=1"));
    printf("artists: %d (first: %s / %s / sort '%s')\n", page.count, artists[0].id, artists[0].title, artists[0].title_sort);
    CHECK(page.count > 5 && page.total == page.count);
    CHECK(strlen(artists[0].id) > 0 && strlen(artists[0].thumb) > 0);
    int all = page.count;
    char third_id[ND_MAX_ID]; strcpy(third_id, artists[2].id);
    /* paging: small first page reports the full total; offset slices the rest */
    CHECK(nd_api_get_artists(&cfg, 0, 0, 2, artists, &page) == 0 && page.count == 2 && page.total == all);
    CHECK(nd_api_get_artists(&cfg, 0, 2, 2000, artists, &page) == 0 && page.count == all - 2);
    CHECK(strcmp(artists[0].id, third_id) == 0);
    CHECK(nd_api_get_artists(&cfg, 0, all, 2000, artists, &page) == 0 && page.count == 0);

    static NdAlbum albums[ND_MAX_ARTIST_ALBUMS]; int nal = 0;
    CHECK(nd_api_get_albums(&cfg, "abc", albums, &nal) == 0);
    printf("albums: %d (first: %s / %s / %s / %s)\n", nal, albums[0].id, albums[0].title, albums[0].artist, albums[0].year);
    CHECK(nal > 0 && strlen(albums[0].id) > 0 && strlen(albums[0].artist_id) > 0);

    static NdTrack tracks[ND_MAX_TRACKS]; int nt = 0;
    CHECK(nd_api_get_tracks(&cfg, "4QZO6nBGty9odiq7pUok5n", tracks, &nt) == 0);
    printf("tracks: %d (first: %s / %s / %dms / #%d / %s %dkbps)\n", nt, tracks[0].id, tracks[0].title,
           tracks[0].duration_ms, tracks[0].track_number, tracks[0].audio_codec, tracks[0].audio_bitrate_kbps);
    CHECK(nt == 2 && strcmp(tracks[0].id, "12CyVV6UsF1YXW0YbJQiRe") == 0);
    CHECK(tracks[0].duration_ms == 189000 && tracks[0].track_number == 1);
    CHECK(strcmp(tracks[0].audio_codec, "mp3") == 0 && tracks[0].audio_bitrate_kbps == 188);

    /* error response → -1 */
    CHECK(nd_api_get_tracks(&cfg, "bogus", tracks, &nt) == -1);

    CHECK(nd_api_get_all_albums(&cfg, 0, 0, 5, albums, &page) == 0);
    printf("byYear: %d total=%d first=%s (%s)\n", page.count, page.total, albums[0].title, albums[0].year);
    CHECK(page.count == 5 && page.total == 5 + 500);
    CHECK(strstr(g_last_url, "size=5&offset=0"));
    CHECK(nd_api_get_all_albums(&cfg, 0, 10, 50, albums, &page) == 0);
    CHECK(page.count == 5 && page.total == 15);  /* short page: exact */

    CHECK(nd_api_get_recent_albums(&cfg, 0, 5, albums, &nal) == 0 && nal == 5);

    CHECK(nd_api_get_starred_tracks(&cfg, tracks, ND_MAX_TRACKS, &nt) == 0);
    printf("starred songs: %d\n", nt);

    char url[2048];
    nd_api_get_stream_url(&cfg, &tracks[0], url, sizeof(url));
    CHECK(strstr(url, "/rest/stream?") && strstr(url, "format=raw"));
    nd_api_get_transcode_url(&cfg, &tracks[0], 128, url, sizeof(url));
    CHECK(strstr(url, "format=opus&maxBitRate=128"));
    nd_api_get_cover_art_url(&cfg, "al-x_1", 300, url, sizeof(url));
    CHECK(strstr(url, "/rest/getCoverArt?") && strstr(url, "id=al-x_1&size=300"));

    printf("ALL OK\n");
    return 0;
}
