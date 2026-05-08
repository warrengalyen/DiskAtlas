/**
 * filesystem.c — Filesystem capability detection.
 *
 * Compiled on all platforms as part of diskatlas_core.
 *
 * Implements:
 *   filesystem_get_scan_capability()  (public, DISKATLAS_API)
 *
 * filesystem_type_from_name() lives as a static inline in da_platform.h so
 * both platform_win32.c and platform_posix.c can use it without a separate
 * compilation unit dependency. This file only provides the public API symbol
 * for scan capability selection.
 *
 * Extension guide:
 *   To add a fast-path for a new filesystem (e.g. EXT4 journal, APFS tree):
 *     1. Add the FS_* variant to filesystem_type_t in diskatlas_storage.h
 *     2. Add a string → type mapping in filesystem_type_from_name() in da_platform.h
 *     3. Add a case below returning SCAN_CAP_RAW_FILESYSTEM (or FAST_METADATA)
 */

#include "diskatlas_storage.h"

DISKATLAS_API scan_capability_t
filesystem_get_scan_capability(filesystem_type_t fs) {
  switch (fs) {
    case FS_NTFS:
      /* Windows NTFS $MFT direct read: significantly faster than traversal
       * for large volumes; requires elevated privileges. */
      return SCAN_CAP_FAST_METADATA;

    /* --- Reserved for future low-level parsers --- */
    /* case FS_EXT4:   return SCAN_CAP_RAW_FILESYSTEM; */
    /* case FS_APFS:   return SCAN_CAP_RAW_FILESYSTEM; */
    /* case FS_BTRFS:  return SCAN_CAP_RAW_FILESYSTEM; */

    case FS_UNKNOWN:
    case FS_FAT:
    case FS_FAT32:
    case FS_EXFAT:
    case FS_EXT2:
    case FS_EXT3:
    case FS_EXT4:
    case FS_XFS:
    case FS_BTRFS:
    case FS_APFS:
    case FS_HFS:
    case FS_NETWORK:
    case FS_MTP:
    default:
      return SCAN_CAP_GENERIC;
  }
}
