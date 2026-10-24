#ifndef ND_CONFIG_H
#define ND_CONFIG_H

#include <stdbool.h>
#include "nd_models.h"

/*
 * Load config from $SHARED_USERDATA_PATH/navidrome/config.json.
 * Returns 0 on success, -1 if file doesn't exist or parse error.
 * cfg is zero-initialized on failure.
 */
int nd_config_load(NdConfig *cfg);

/*
 * Save config to $SHARED_USERDATA_PATH/navidrome/config.json.
 * Creates directory if needed. Returns 0 on success.
 */
int nd_config_save(const NdConfig *cfg);

/* Returns true if cfg has a non-empty server_url, username and password. */
bool nd_config_is_valid(const NdConfig *cfg);

/*
 * Return a mutable pointer to the application-global NdConfig.
 * Defined in main.c. Modules call this to update the live config after
 * saving so the browse module sees the new values immediately.
 */
NdConfig *nd_config_get_mutable(void);

#endif /* ND_CONFIG_H */
