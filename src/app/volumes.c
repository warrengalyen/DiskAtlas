#include <string.h>

#include "volumes.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#if !defined(_WIN32)
#include <errno.h>
#include <sys/statvfs.h>
#endif

#if defined(_WIN32)

static int utf8_to_wide(const char *utf8, WCHAR *out, int cchOut) {
  if (!utf8 || !out || cchOut <= 0) {
    return 0;
  }
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, out, cchOut);
  if (n <= 0) {
    n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, cchOut);
  }
  return n > 0 ? n : 0;
}

int da_volume_space_for_path(const char *path_utf8, uint64_t *total, uint64_t *free_bytes,
                             uint64_t *used_bytes) {
  WCHAR wpath[4096];
  if (!utf8_to_wide(path_utf8, wpath, (int)(sizeof(wpath) / sizeof(wpath[0])))) {
    return -1;
  }
  WCHAR root[8];
  if (wpath[0] != L'\0' && wpath[1] == L':') {
    root[0] = wpath[0];
    root[1] = L':';
    root[2] = L'\\';
    root[3] = L'\0';
  } else {
    root[0] = L'\0';
  }
  ULARGE_INTEGER free_caller = {0}, total_all = {0}, total_free = {0};
  if (!GetDiskFreeSpaceExW(root[0] ? root : wpath, &free_caller, &total_all, &total_free)) {
    return -1;
  }
  if (total) {
    *total = total_all.QuadPart;
  }
  if (free_bytes) {
    *free_bytes = total_free.QuadPart;
  }
  if (used_bytes) {
    if (total_all.QuadPart >= total_free.QuadPart) {
      *used_bytes = total_all.QuadPart - total_free.QuadPart;
    } else {
      *used_bytes = 0;
    }
  }
  return 0;
}

#else

int da_volume_space_for_path(const char *path_utf8, uint64_t *total, uint64_t *free_bytes,
                             uint64_t *used_bytes) {
  struct statvfs vfs;
  memset(&vfs, 0, sizeof(vfs));
  if (statvfs(path_utf8, &vfs) != 0) {
    return -1;
  }
  uint64_t fr = (uint64_t)vfs.f_frsize;
  uint64_t blocks = (uint64_t)vfs.f_blocks;
  uint64_t bavail = (uint64_t)vfs.f_bavail;
  uint64_t bfree = (uint64_t)vfs.f_bfree;
  uint64_t t = fr * blocks;
  uint64_t f = fr * bavail;
  if (total) {
    *total = t;
  }
  if (free_bytes) {
    *free_bytes = f;
  }
  if (used_bytes) {
    *used_bytes = (t > f) ? (t - f) : 0;
  }
  (void)bfree;
  return 0;
}

#endif /* _WIN32 */
