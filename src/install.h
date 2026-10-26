#ifndef MCLC_INSTALL_H
#define MCLC_INSTALL_H

#include "version.h"

#include <wchar.h>

/*
 * Make sure the client jar, libraries, natives, logging config and all asset
 * objects for a version exist. Existing files are checked by size; when
 * verify is non-zero their SHA-1 is checked as well.
 */
int install_version(Version *version, const wchar_t *game_dir, int verify,
                    char **error);

#endif /* MCLC_INSTALL_H */
