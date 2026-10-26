#ifndef MCLC_ZIP_H
#define MCLC_ZIP_H

#include <wchar.h>

/*
 * Extract all regular files from a ZIP archive into dest_dir.
 * Paths are sanitised (no absolute paths or ".."), and META-INF signatures
 * are skipped. Returns the number of extracted files, or -1 on error.
 */
int zip_extract_all(const wchar_t *zip_path, const wchar_t *dest_dir,
                    char **error);

#endif /* MCLC_ZIP_H */
