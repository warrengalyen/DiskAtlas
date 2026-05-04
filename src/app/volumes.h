#ifndef VOLUMES_H
#define VOLUMES_H

#include <stdint.h>

/** Returns 0 on success; fills totals in bytes (best effort). */
int da_volume_space_for_path(const char *path_utf8, uint64_t *total, uint64_t *free_bytes,
                             uint64_t *used_bytes);

#endif
