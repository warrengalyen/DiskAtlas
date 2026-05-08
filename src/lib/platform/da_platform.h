#ifndef DA_PLATFORM_H
#define DA_PLATFORM_H

/**
 * da_platform.h — Internal platform helpers for the storage abstraction layer.
 *
 * Not part of the public API. Include only from src/lib/platform/ and
 * src/lib/filesystem/ translation units.
 *
 * Provides:
 *   - filesystem_type_from_name(): map an OS filesystem name string to
 *     filesystem_type_t (used by both platform_win32.c and platform_posix.c)
 *   - UTF-16 conversion helpers (Windows only)
 *   - Internal storage_device_t array growth helper
 */

#include "diskatlas_storage.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Filesystem name → type mapping                                             */
/* -------------------------------------------------------------------------- */

/**
 * Map an OS-reported filesystem name string to filesystem_type_t.
 *
 * name is case-insensitive; examples of accepted strings per platform:
 *   Windows: "NTFS", "FAT", "FAT32", "exFAT"
 *   Linux:   "ntfs", "ntfs3", "vfat", "ext2", "ext3", "ext4",
 *             "xfs", "btrfs", "cifs", "smb2", "nfs", "nfs4"
 *   macOS:   "apfs", "hfs", "exfat", "smbfs", "nfs"
 *
 * Returns FS_UNKNOWN for any unrecognised string.
 */
static inline filesystem_type_t filesystem_type_from_name(const char *name) {
  if (!name || name[0] == '\0') {
    return FS_UNKNOWN;
  }

  /* Case-insensitive compare helper — avoid locale-dependent tolower. */
#define DA_STRICMP_ASCII(a, b) (da_ascii_stricmp((a), (b)) == 0)

  /* Use a minimal inline helper instead of strcasecmp (not C17 standard). */
  char lower[32];
  size_t i = 0;
  while (i < sizeof(lower) - 1 && name[i] != '\0') {
    char c = name[i];
    if (c >= 'A' && c <= 'Z') {
      c = (char)(c + ('a' - 'A'));
    }
    lower[i] = c;
    i++;
  }
  lower[i] = '\0';

  /* NTFS */
  if (strcmp(lower, "ntfs") == 0 || strcmp(lower, "ntfs3") == 0) {
    return FS_NTFS;
  }
  /* exFAT — check before FAT32/FAT so "exfat" doesn't match "fat" prefix */
  if (strcmp(lower, "exfat") == 0) {
    return FS_EXFAT;
  }
  /* FAT32 */
  if (strcmp(lower, "fat32") == 0) {
    return FS_FAT32;
  }
  /* FAT / vfat */
  if (strcmp(lower, "fat") == 0 || strcmp(lower, "vfat") == 0 ||
      strcmp(lower, "msdos") == 0) {
    return FS_FAT;
  }
  /* EXT4 */
  if (strcmp(lower, "ext4") == 0) {
    return FS_EXT4;
  }
  /* EXT3 */
  if (strcmp(lower, "ext3") == 0) {
    return FS_EXT3;
  }
  /* EXT2 */
  if (strcmp(lower, "ext2") == 0) {
    return FS_EXT2;
  }
  /* XFS */
  if (strcmp(lower, "xfs") == 0) {
    return FS_XFS;
  }
  /* Btrfs */
  if (strcmp(lower, "btrfs") == 0) {
    return FS_BTRFS;
  }
  /* APFS */
  if (strcmp(lower, "apfs") == 0) {
    return FS_APFS;
  }
  /* HFS / HFS+ */
  if (strcmp(lower, "hfs") == 0 || strcmp(lower, "hfs+") == 0 ||
      strcmp(lower, "hfsplus") == 0) {
    return FS_HFS;
  }
  /* Network filesystems */
  if (strcmp(lower, "cifs")   == 0 || strcmp(lower, "smb2")   == 0 ||
      strcmp(lower, "smb3")   == 0 || strcmp(lower, "smbfs")  == 0 ||
      strcmp(lower, "nfs")    == 0 || strcmp(lower, "nfs4")   == 0 ||
      strcmp(lower, "nfs3")   == 0 || strcmp(lower, "davfs")  == 0 ||
      strcmp(lower, "davfs2") == 0 || strcmp(lower, "webdav") == 0 ||
      strcmp(lower, "ftp")    == 0 || strcmp(lower, "sftp")   == 0) {
    return FS_NETWORK;
  }

#undef DA_STRICMP_ASCII

  return FS_UNKNOWN;
}

/* -------------------------------------------------------------------------- */
/* Internal device array builder                                              */
/* -------------------------------------------------------------------------- */

/**
 * Append a zeroed storage_device_t slot to *arr (capacity *cap, length *len).
 * Doubles the allocation when full. Returns a pointer to the new slot, or
 * NULL on allocation failure (array and counts are left unchanged).
 */
static inline storage_device_t *da_device_array_push(storage_device_t **arr,
                                                      size_t *len,
                                                      size_t *cap) {
  if (*len >= *cap) {
    size_t new_cap = (*cap == 0) ? 8 : (*cap * 2);
    /* Avoid overflow: cap * sizeof must not exceed SIZE_MAX. */
    if (new_cap > (size_t)-1 / sizeof(storage_device_t)) {
      return NULL;
    }
    storage_device_t *tmp = (storage_device_t *)realloc(*arr,
                                new_cap * sizeof(storage_device_t));
    if (!tmp) {
      return NULL;
    }
    *arr = tmp;
    *cap = new_cap;
  }
  storage_device_t *slot = &(*arr)[*len];
  memset(slot, 0, sizeof(*slot));
  slot->struct_version = DISKATLAS_STORAGE_DEVICE_STRUCT_VERSION;
  (*len)++;
  return slot;
}

/* -------------------------------------------------------------------------- */
/* Windows UTF-16 helpers (compiled only on Win32)                           */
/* -------------------------------------------------------------------------- */

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

/**
 * Convert a UTF-8 string to a wide (UTF-16) string.
 * Writes at most wout_chars wide characters (including NUL).
 * Returns the number of wide chars written (>0) on success, 0 on failure.
 */
static inline int da_utf8_to_wide(const char *utf8, WCHAR *wout,
                                  int wout_chars) {
  if (!utf8 || !wout || wout_chars <= 0) {
    return 0;
  }
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                              utf8, -1, wout, wout_chars);
  if (n <= 0) {
    /* Fallback: tolerate invalid sequences. */
    n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wout, wout_chars);
  }
  return (n > 0) ? n : 0;
}

/**
 * Convert a wide (UTF-16) string to UTF-8.
 * Writes at most out_bytes bytes (including NUL).
 * Returns the number of bytes written (>0) on success, 0 on failure.
 */
static inline int da_wide_to_utf8(const WCHAR *wide, char *out,
                                  int out_bytes) {
  if (!wide || !out || out_bytes <= 0) {
    return 0;
  }
  int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                              wide, -1, out, out_bytes, NULL, NULL);
  if (n <= 0) {
    n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, out_bytes,
                            NULL, NULL);
  }
  return (n > 0) ? n : 0;
}

#endif /* _WIN32 */

#ifdef __cplusplus
}
#endif

#endif /* DA_PLATFORM_H */
