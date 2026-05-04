#if defined(_WIN32)

#ifndef WINVER
#define WINVER 0x0601
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "diskatlas.h"

#define NTFS_BOOT_OEM_OFF 0x03u
#define NTFS_BOOT_BPS_OFF 0x0Bu
#define NTFS_BOOT_SPC_OFF 0x0Du
#define NTFS_BOOT_MFT_LCN_OFF 0x30u
#define NTFS_BOOT_MFTMIRR_LCN_OFF 0x38u
#define NTFS_BOOT_CLUSTERS_PER_MFT_RECORD_OFF 0x40u

static const unsigned char k_ntfs_oem[8] = {'N', 'T', 'F', 'S', ' ', ' ', ' ', ' '};

static uint16_t read_u16_le(const unsigned char *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static int64_t read_i64_le(const unsigned char *p) {
  uint64_t u =
      (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24) |
      ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) | ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
  return (int64_t)u;
}

static unsigned char is_pow2_u8(unsigned char x) {
  return x != 0 && (x & (x - 1u)) == 0;
}

static wchar_t *utf8_to_wide_path(const char *utf8) {
  DWORD conv = MB_ERR_INVALID_CHARS;
  int n = MultiByteToWideChar(CP_UTF8, conv, utf8, -1, NULL, 0);
  if (n <= 0) {
    conv = 0;
    n = MultiByteToWideChar(CP_UTF8, conv, utf8, -1, NULL, 0);
  }
  if (n <= 0) {
    return NULL;
  }
  wchar_t *w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
  if (!w) {
    return NULL;
  }
  if (MultiByteToWideChar(CP_UTF8, conv, utf8, -1, w, n) <= 0) {
    free(w);
    return NULL;
  }
  return w;
}

static uint32_t decode_mft_record_size_bytes(uint64_t cluster_bytes, int8_t clusters_per_record_raw) {
  if (cluster_bytes == 0 || cluster_bytes > (uint64_t)UINT32_MAX) {
    return 0;
  }
  if (clusters_per_record_raw < 0) {
    int exp = -(int)clusters_per_record_raw;
    if (exp < 1 || exp > 31) {
      return 0;
    }
    return 1u << (unsigned)exp;
  }
  uint64_t mul = (uint64_t)(uint8_t)clusters_per_record_raw * cluster_bytes;
  if (mul == 0 || mul > (uint64_t)UINT32_MAX) {
    return 0;
  }
  return (uint32_t)mul;
}

/**
 * Reads NTFS VBR, validates OEM / BPB, and derives $MFT / $MFTMirr on-disk byte offsets.
 * \p volume_device_path_utf8: device path such as "\\\\.\\C:" (administrator often required).
 * \return 0 on success; -1 on failure (see \p out->win32_error; ERROR_BAD_FORMAT if not NTFS VBR).
 */
DISKATLAS_API int diskatlas_ntfs_get_mft_location(const char *volume_device_path_utf8,
                                                  diskatlas_ntfs_mft_location_t *out) {
  if (!out) {
    return -1;
  }
  memset(out, 0, sizeof(*out));
  out->struct_version = DISKATLAS_NTFS_MFT_LOCATION_STRUCT_VERSION;

  if (!volume_device_path_utf8 || volume_device_path_utf8[0] == '\0') {
    out->win32_error = (uint32_t)ERROR_INVALID_PARAMETER;
    return -1;
  }

  wchar_t *wpath = utf8_to_wide_path(volume_device_path_utf8);
  if (!wpath) {
    out->win32_error = (uint32_t)ERROR_INVALID_PARAMETER;
    return -1;
  }

  HANDLE h = CreateFileW(wpath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                         OPEN_EXISTING, 0, NULL);
  free(wpath);
  if (h == INVALID_HANDLE_VALUE) {
    out->win32_error = (uint32_t)GetLastError();
    return -1;
  }

  unsigned char boot[4096];
  DWORD got = 0;
  if (!ReadFile(h, boot, sizeof(boot), &got, NULL)) {
    out->win32_error = (uint32_t)GetLastError();
    CloseHandle(h);
    return -1;
  }
  if (got < 512u) {
    out->win32_error = (uint32_t)ERROR_INVALID_DATA;
    CloseHandle(h);
    return -1;
  }
  CloseHandle(h);

  if (memcmp(boot + NTFS_BOOT_OEM_OFF, k_ntfs_oem, sizeof(k_ntfs_oem)) != 0) {
    out->win32_error = (uint32_t)ERROR_BAD_FORMAT;
    return -1;
  }

  uint16_t bps = read_u16_le(boot + NTFS_BOOT_BPS_OFF);
  if (bps == 0 || (bps % 512u) != 0 || bps > 4096u) {
    out->win32_error = (uint32_t)ERROR_BAD_FORMAT;
    return -1;
  }

  unsigned char spc_u = boot[NTFS_BOOT_SPC_OFF];
  if (spc_u == 0 || !is_pow2_u8(spc_u)) {
    out->win32_error = (uint32_t)ERROR_BAD_FORMAT;
    return -1;
  }

  int64_t mft_lcn_i = read_i64_le(boot + NTFS_BOOT_MFT_LCN_OFF);
  int64_t mirr_lcn_i = read_i64_le(boot + NTFS_BOOT_MFTMIRR_LCN_OFF);
  if (mft_lcn_i < 0 || mirr_lcn_i < 0) {
    out->win32_error = (uint32_t)ERROR_BAD_FORMAT;
    return -1;
  }

  uint64_t mft_lcn = (uint64_t)mft_lcn_i;
  uint64_t mirr_lcn = (uint64_t)mirr_lcn_i;

  uint64_t cluster_bytes = (uint64_t)bps * (uint64_t)spc_u;
  if (cluster_bytes == 0) {
    out->win32_error = (uint32_t)ERROR_BAD_FORMAT;
    return -1;
  }

  int8_t cpr = (int8_t)boot[NTFS_BOOT_CLUSTERS_PER_MFT_RECORD_OFF];

  out->bytes_per_sector = (uint32_t)bps;
  out->sectors_per_cluster = (uint32_t)spc_u;
  out->cluster_size_bytes = cluster_bytes;
  out->mft_start_lcn = mft_lcn;
  out->mft_mirror_start_lcn = mirr_lcn;
  out->mft_byte_offset = mft_lcn * cluster_bytes;
  out->mft_mirror_byte_offset = mirr_lcn * cluster_bytes;
  out->mft_record_size_bytes = decode_mft_record_size_bytes(cluster_bytes, cpr);
  out->win32_error = 0;
  return 0;
}

#endif /* _WIN32 */
