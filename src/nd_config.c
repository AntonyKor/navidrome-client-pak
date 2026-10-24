#define _GNU_SOURCE
#include "nd_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

#include "include/parson/parson.h"
#include "api.h"
#include "nd_log.h"

#define ND_CONFIG_FALLBACK_DIR "/mnt/SDCARD/.userdata/shared/navidrome"
#define ND_CONFIG_FILENAME     "config.json"

/* Build the path to config.json into `out` (caller supplies buffer). */
static void config_path(char *out, int out_size)
{
    const char *base = getenv("SHARED_USERDATA_PATH");
    if (base && *base) {
        snprintf(out, out_size, "%s/navidrome/%s", base, ND_CONFIG_FILENAME);
    } else {
        snprintf(out, out_size, "%s/%s",
                 ND_CONFIG_FALLBACK_DIR, ND_CONFIG_FILENAME);
    }
}

/* Ensure the directory portion of a path exists (one level only). */
static void ensure_config_dir(const char *file_path)
{
    char dir[640];
    strncpy(dir, file_path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';

    char *slash = strrchr(dir, '/');
    if (!slash) return;
    *slash = '\0';

    /* Create parent of directory first (best-effort) */
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
 * Public API
 * ------------------------------------------------------------------ */

int nd_config_load(NdConfig *cfg)
{
    if (!cfg) return -1;
    memset(cfg, 0, sizeof(*cfg));

    char path[640];
    config_path(path, sizeof(path));

    JSON_Value *root = json_parse_file(path);
    if (!root) {
        ND_LOG_ERROR("[NdConfig] Could not parse: %s\n", path);
        return -1;
    }

    JSON_Object *obj = json_value_get_object(root);
    if (!obj) {
        json_value_free(root);
        return -1;
    }

    const char *server_url  = json_object_get_string(obj, "server_url");
    const char *username    = json_object_get_string(obj, "username");
    const char *password    = json_object_get_string(obj, "password");
    const char *server_name = json_object_get_string(obj, "server_name");

    if (server_url)
        strncpy(cfg->server_url, server_url, sizeof(cfg->server_url) - 1);
    if (username)
        strncpy(cfg->username, username, sizeof(cfg->username) - 1);
    if (password)
        strncpy(cfg->password, password, sizeof(cfg->password) - 1);
    if (server_name)
        strncpy(cfg->server_name, server_name, sizeof(cfg->server_name) - 1);

    cfg->offline_mode = (json_object_get_boolean(obj, "offline_mode") == 1);

    {
        int st = (int)json_object_get_number(obj, "screen_timeout");
        cfg->screen_timeout = (st > 0) ? st : 30;
    }

    cfg->library_id = (int)json_object_get_number(obj, "library_id");
    const char *library_name = json_object_get_string(obj, "library_name");
    if (library_name)
        strncpy(cfg->library_name, library_name, sizeof(cfg->library_name) - 1);

    if (json_object_has_value(obj, "stream_bitrate_kbps"))
        cfg->stream_bitrate_kbps = (int)json_object_get_number(obj, "stream_bitrate_kbps");
    else
        cfg->stream_bitrate_kbps = 0;   /* Original: Opus transcoding needs ffmpeg on the server */

    if (json_object_has_value(obj, "download_bitrate_kbps"))
        cfg->download_bitrate_kbps = (int)json_object_get_number(obj, "download_bitrate_kbps");
    else
        cfg->download_bitrate_kbps = 0;

    /* Default true: pocket lock on unless explicitly saved as false */
    if (json_object_has_value(obj, "pocket_lock_enabled"))
        cfg->pocket_lock_enabled = (json_object_get_boolean(obj, "pocket_lock_enabled") == 1);
    else
        cfg->pocket_lock_enabled = true;

    if (json_object_has_value(obj, "preload_count")) {
        cfg->preload_count = (int)json_object_get_number(obj, "preload_count");
        if (cfg->preload_count < 0)  cfg->preload_count = 0;
        if (cfg->preload_count > 10) cfg->preload_count = 10;
    } else {
        cfg->preload_count = 2;
    }

    json_value_free(root);
    return 0;
}

int nd_config_save(const NdConfig *cfg)
{
    if (!cfg) return -1;

    char path[640];
    config_path(path, sizeof(path));
    ensure_config_dir(path);

    JSON_Value  *root = json_value_init_object();
    JSON_Object *obj  = json_value_get_object(root);

    json_object_set_string(obj, "server_url",   cfg->server_url);
    json_object_set_string(obj, "username",     cfg->username);
    json_object_set_string(obj, "password",     cfg->password);
    json_object_set_string(obj, "server_name",  cfg->server_name);
    json_object_set_boolean(obj, "offline_mode", cfg->offline_mode ? 1 : 0);
    json_object_set_number(obj, "screen_timeout", cfg->screen_timeout);
    json_object_set_number(obj, "library_id",     cfg->library_id);
    json_object_set_string(obj, "library_name",   cfg->library_name);
    json_object_set_number(obj, "stream_bitrate_kbps",   cfg->stream_bitrate_kbps);
    json_object_set_number(obj, "download_bitrate_kbps", cfg->download_bitrate_kbps);
    json_object_set_boolean(obj, "pocket_lock_enabled",  cfg->pocket_lock_enabled ? 1 : 0);
    json_object_set_number(obj, "preload_count",         cfg->preload_count);

    char *json_str = json_serialize_to_string_pretty(root);
    json_value_free(root);
    if (!json_str) return -1;

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        json_free_serialized_string(json_str);
        return -1;
    }

    size_t len = strlen(json_str);
    ssize_t written = write(fd, json_str, len);
    int sync_err = fsync(fd);
    close(fd);
    json_free_serialized_string(json_str);

    if (written != (ssize_t)len || sync_err != 0) {
        ND_LOG_ERROR("[NdConfig] Failed to write or fsync: %s\n", path);
        return -1;
    }
    return 0;
}

bool nd_config_is_valid(const NdConfig *cfg)
{
    return cfg &&
           cfg->server_url[0] != '\0' &&
           cfg->username[0]   != '\0' &&
           cfg->password[0]   != '\0';
}
