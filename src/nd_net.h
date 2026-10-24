#ifndef ND_NET_H
#define ND_NET_H

#include <stdint.h>
#include <stdbool.h>

/* HTTP methods */
typedef enum { ND_HTTP_GET, ND_HTTP_POST } NdHttpMethod;

/* Per-request options */
typedef struct {
    NdHttpMethod method;
    const char    *body;            /* POST body (NULL for GET) */
    const char    *token;           /* unused (auth is in the URL query); kept for API stability */
    int            timeout_sec;     /* 0 = default (15s) */
    bool           no_persist;      /* true = bypass s_persist_ctx pool */
} NdNetOptions;

/*
 * Fetch a URL (http or https). Adds User-Agent and Accept: application/json.
 * The buffer is always NUL-terminated (at most buffer_size-1 bytes written).
 *
 * Returns bytes written to buffer, or -1 on error.
 */
int nd_net_fetch(const char *url, uint8_t *buffer, int buffer_size,
                   const NdNetOptions *opts);

/*
 * Download a URL to a local file path in streaming fashion (no large memory
 * buffer). Uses the same SSL/HTTP infrastructure as nd_net_fetch.
 *
 * progress_pct: updated with 0-100 as download proceeds; may be NULL.
 *               Updated from Content-Length header when available.
 * should_cancel: if set to true by caller, abort download and return -1; may
 *                be NULL.
 * opts: standard NdNetOptions (token may be NULL if already in URL).
 *
 * Returns total bytes written on success, or -1 on error or cancellation.
 */
int nd_net_download_file(const char *url, const char *filepath,
                           volatile int *progress_pct,
                           volatile bool *should_cancel,
                           const NdNetOptions *opts);

/*
 * Close and release the persistent keep-alive connection (if any).
 * Call this from the main thread after any worker thread using fetch has been
 * joined (e.g. in module_browse_reset()).
 */
void nd_net_connection_close(void);

#endif /* ND_NET_H */
