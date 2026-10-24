/* Live integration test against a real Navidrome server (default: the public
 * demo). Uses the real network + TLS stack. Not run in CI.
 * Usage: tests/run_live_test.sh [server_url user password] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "nd_api.h"
#include "nd_net.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); return 1; } } while (0)

static long file_size(const char *p) { struct stat st; return stat(p, &st) == 0 ? (long)st.st_size : -1; }

static void magic(const char *p, char *out)
{
    unsigned char b[12] = {0};
    FILE *f = fopen(p, "rb"); if (f) { fread(b, 1, 12, f); fclose(f); }
    sprintf(out, "%02x%02x%02x%02x '%.4s'", b[0], b[1], b[2], b[3], (char *)b);
}

int main(int argc, char **argv)
{
    NdConfig cfg; memset(&cfg, 0, sizeof(cfg));
    strcpy(cfg.server_url, argc > 1 ? argv[1] : "https://demo.navidrome.org");
    strcpy(cfg.username,   argc > 2 ? argv[2] : "demo");
    strcpy(cfg.password,   argc > 3 ? argv[3] : "demo");
    const char *tmp = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";

    char err[128];
    CHECK(nd_api_ping(&cfg, err, sizeof(err)) == 0);
    printf("ping ok\n");

    NdConfig bad = cfg; strcpy(bad.password, "definitely-wrong");
    CHECK(nd_api_ping(&bad, err, sizeof(err)) != 0);
    printf("bad password -> '%s'\n", err);

    NdLibrary libs[ND_MAX_LIBRARIES]; int nlib;
    CHECK(nd_api_get_libraries(&cfg, libs, &nlib) == 0 && nlib > 0);
    printf("folders: %d (%s)\n", nlib, libs[0].title);

    static NdArtist artists[5000]; NdPage page;
    CHECK(nd_api_get_artists(&cfg, 0, 0, 5000, artists, &page) == 0 && page.count > 0);
    printf("artists: %d\n", page.count);

    static NdAlbum albums[ND_MAX_ARTIST_ALBUMS]; int nal;
    CHECK(nd_api_get_albums(&cfg, artists[0].id, albums, &nal) == 0 && nal > 0);
    static NdTrack tracks[ND_MAX_TRACKS]; int nt;
    CHECK(nd_api_get_tracks(&cfg, albums[0].id, tracks, &nt) == 0 && nt > 0);
    printf("album '%s': %d tracks, first '%s' (%s, %d kbps, %d ms)\n", albums[0].title, nt,
           tracks[0].title, tracks[0].audio_codec, tracks[0].audio_bitrate_kbps, tracks[0].duration_ms);

    CHECK(nd_api_get_all_albums(&cfg, 0, 0, 20, albums, &page) == 0 && page.count > 0);
    printf("albums by year: %d, first '%s' (%s)\n", page.count, albums[0].title, albums[0].year);

    NdNetOptions opts = { .method = ND_HTTP_GET, .timeout_sec = 60, .no_persist = true };
    char url[2048], path[512], m[64];

    nd_api_get_stream_url(&cfg, &tracks[0], url, sizeof(url));
    snprintf(path, sizeof(path), "%s/nd_live_raw.%s", tmp, tracks[0].audio_codec);
    int n = nd_net_download_file(url, path, NULL, NULL, &opts);
    magic(path, m);
    printf("raw stream: %d bytes (file %ld) magic %s\n", n, file_size(path), m);
    CHECK(n > 10000);

    nd_api_get_transcode_url(&cfg, &tracks[0], 96, url, sizeof(url));
    snprintf(path, sizeof(path), "%s/nd_live_transcode.opus", tmp);
    n = nd_net_download_file(url, path, NULL, NULL, &opts);
    magic(path, m);
    printf("opus transcode: %d bytes magic %s\n", n, m);
    CHECK(n > 10000);

    nd_api_get_cover_art_url(&cfg, albums[0].thumb, 300, url, sizeof(url));
    static uint8_t img[512 * 1024];
    n = nd_net_fetch(url, img, sizeof(img), &opts);
    printf("cover art: %d bytes, starts %02x%02x\n", n, img[0], img[1]);
    CHECK(n > 1000);

    /* star / unstar round trip on a song, then confirm via getStarred2 */
    static NdTrack starred[1000]; int ns;
    int ns_before_had = 0;
    CHECK(nd_api_get_starred_tracks(&cfg, starred, 1000, &ns) == 0);
    for (int i = 0; i < ns; i++) if (nd_id_eq(starred[i].id, tracks[0].id)) ns_before_had = 1;
    CHECK(nd_api_set_starred(&cfg, tracks[0].id, true) == 0);
    CHECK(nd_api_get_starred_tracks(&cfg, starred, 1000, &ns) == 0);
    int found = 0; for (int i = 0; i < ns; i++) if (nd_id_eq(starred[i].id, tracks[0].id)) found = 1;
    printf("starred songs: %d, test song present: %d\n", ns, found);
    /* leave the server as we found it (unless the song was already starred) */
    if (ns_before_had == 0) CHECK(nd_api_set_starred(&cfg, tracks[0].id, false) == 0);
    CHECK(found);

    printf("LIVE OK\n");
    return 0;
}
