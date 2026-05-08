#ifndef DISKATLAS_STORAGE_H
#define DISKATLAS_STORAGE_H

/**
 * diskatlas_storage.h — Cross-platform storage abstraction layer.
 *
 * Provides:
 *   - filesystem_type_t: identifies the filesystem on a mounted volume
 *   - storage_device_t:  describes a mounted volume/device
 *   - scan_capability_t: optimal scan method for a given filesystem
 *   - scan_error_t:      portable error codes for storage operations
 *   - platform_enum_storage_devices(): enumerate all mounted volumes
 *   - filesystem_get_scan_capability(): query best scan method
 *   - da_path_*():       cross-platform path utilities (UTF-8 internally)
 *
 * Threading: platform_enum_storage_devices() is synchronous and blocking.
 * Call it off the UI thread. The returned array must be freed with free().
 */

#include "diskatlas.h"

#include <stddef.h>
#include <stdint.h>

/* PATH_MAX portability: Linux/macOS define it in <limits.h>; Windows does not.
 * We define a fallback large enough for all supported platforms. */
#ifndef PATH_MAX
#ifdef MAX_PATH
#define PATH_MAX MAX_PATH
#else
#define PATH_MAX 4096
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Filesystem type                                                             */
/* -------------------------------------------------------------------------- */

typedef enum filesystem_type {
  FS_UNKNOWN  = 0,
  FS_NTFS     = 1,
  FS_FAT      = 2,
  FS_FAT32    = 3,
  FS_EXFAT    = 4,
  FS_EXT2     = 5,
  FS_EXT3     = 6,
  FS_EXT4     = 7,
  FS_XFS      = 8,
  FS_BTRFS    = 9,
  FS_APFS     = 10,
  FS_HFS      = 11,
  FS_NETWORK  = 12, /**< SMB, CIFS, NFS, or any other network filesystem. */
  FS_MTP      = 13  /**< Portable/MTP device (best-effort, generic traversal). */
} filesystem_type_t;

/* -------------------------------------------------------------------------- */
/* Storage device descriptor                                                  */
/* -------------------------------------------------------------------------- */

#define DISKATLAS_STORAGE_DEVICE_STRUCT_VERSION 1u

typedef struct storage_device {
  uint32_t struct_version; /**< DISKATLAS_STORAGE_DEVICE_STRUCT_VERSION. */
  uint32_t reserved_u32;

  char mount_path[PATH_MAX]; /**< UTF-8 mount point / drive root (e.g. "C:\\" or "/mnt/data"). */
  char display_name[256];    /**< Human-readable label; empty string if unavailable. */

  filesystem_type_t fs_type;

  uint64_t total_bytes; /**< Total capacity; 0 if unavailable. */
  uint64_t free_bytes;  /**< Free space; 0 if unavailable. */

  int is_removable; /**< Non-zero if this is a removable/external device. */
  int is_network;   /**< Non-zero if this is a network-backed mount (SMB, NFS, …). */
  int is_read_only; /**< Non-zero if the filesystem is mounted read-only. */
  int reserved_int;
} storage_device_t;

/* -------------------------------------------------------------------------- */
/* Scan capability                                                             */
/* -------------------------------------------------------------------------- */

/**
 * Describes the optimal scan strategy available for a given filesystem.
 *
 *   SCAN_CAP_GENERIC        — standard recursive directory traversal
 *   SCAN_CAP_FAST_METADATA  — filesystem exposes a fast metadata API
 *                             (e.g. NTFS $MFT direct read)
 *   SCAN_CAP_RAW_FILESYSTEM — reserved; future low-level FS parser
 *                             (e.g. EXT4 journal, APFS metadata tree)
 */
typedef enum scan_capability {
  SCAN_CAP_GENERIC        = 0,
  SCAN_CAP_FAST_METADATA  = 1,
  SCAN_CAP_RAW_FILESYSTEM = 2
} scan_capability_t;

/* -------------------------------------------------------------------------- */
/* Portable error codes                                                       */
/* -------------------------------------------------------------------------- */

typedef enum scan_error {
  SCAN_OK                        = 0,
  SCAN_ERR_ACCESS_DENIED         = 1,
  SCAN_ERR_DEVICE_UNAVAILABLE    = 2,
  SCAN_ERR_UNSUPPORTED_FILESYSTEM = 3,
  SCAN_ERR_IO                    = 4
} scan_error_t;

/* -------------------------------------------------------------------------- */
/* Device enumeration                                                         */
/* -------------------------------------------------------------------------- */

/**
 * Enumerate all mounted storage volumes visible to the current process.
 *
 * Populates *devices with a heap-allocated array of storage_device_t and
 * returns the number of entries. The caller is responsible for freeing
 * *devices with free() when done.
 *
 * Returns 0 and leaves *devices unchanged on failure.
 * Individual mounts that are unavailable or permission-denied are silently
 * skipped — only accessible volumes are returned.
 *
 * This function is blocking; call it off the UI thread.
 */
DISKATLAS_API size_t platform_enum_storage_devices(storage_device_t **devices);

/* -------------------------------------------------------------------------- */
/* Filesystem capability query                                                */
/* -------------------------------------------------------------------------- */

/**
 * Return the best available scan capability for the given filesystem type.
 * Currently: NTFS → SCAN_CAP_FAST_METADATA; all others → SCAN_CAP_GENERIC.
 * SCAN_CAP_RAW_FILESYSTEM is reserved for future parsers.
 */
DISKATLAS_API scan_capability_t filesystem_get_scan_capability(filesystem_type_t fs);

/* -------------------------------------------------------------------------- */
/* Cross-platform path utilities (UTF-8 on all platforms)                    */
/* -------------------------------------------------------------------------- */

/**
 * Normalize path separators in-place: on Windows converts '\\' to '/';
 * collapses redundant consecutive separators to a single '/'.
 * The path is modified in place; len is the buffer size (including NUL).
 */
DISKATLAS_API void da_path_normalize(char *path, size_t len);

/**
 * Returns non-zero if path is absolute:
 *   POSIX: leading '/'
 *   Win32: drive letter + ':' (e.g. "C:") or UNC prefix "\\\\"
 */
DISKATLAS_API int da_path_is_absolute(const char *path);

/**
 * Safely join base and rel into out[0..out_sz).
 * Inserts a path separator between base and rel if needed.
 * Always NUL-terminates. Returns 0 on success, -1 if out_sz is too small.
 */
DISKATLAS_API int da_path_join(char *out, size_t out_sz,
                               const char *base, const char *rel);

#ifdef __cplusplus
}
#endif

#endif /* DISKATLAS_STORAGE_H */
