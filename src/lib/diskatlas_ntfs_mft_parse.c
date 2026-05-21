#if defined(_WIN32)

#include <string.h>

#include <windows.h>

#include "diskatlas_ntfs_mft_parse.h"

static uint16_t read_u16_le(const unsigned char *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_u32_le(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t read_u64_le(const unsigned char *p) {
  return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24) |
         ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) | ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
}

static uint32_t fn_namespace_score(unsigned char ns) {
  if (ns == 1u || ns == 3u) {
    return 1000u;
  }
  if (ns == 2u) {
    return 100u;
  }
  return 50u;
}

static void fn_consider(da_ntfs_fn_pick_t *best, const unsigned char *val, uint32_t val_len) {
  if (val_len < 0x44u) {
    return;
  }
  uint64_t parent = read_u64_le(val) & DA_NTFS_MFT_REF_MASK;
  uint64_t rsz = read_u64_le(val + 0x30);
  unsigned char nl = val[0x40];
  unsigned char ns = val[0x41];
  if (nl == 0 || (size_t)nl * 2u + 0x42u > val_len) {
    return;
  }
  uint32_t score = fn_namespace_score(ns) + (uint32_t)nl;
  if (score <= best->score) {
    return;
  }
  best->score = score;
  best->parent_id = parent;
  best->real_size = rsz;
  best->name_chars = nl;
  memcpy(best->name_buf, val + 0x42, (size_t)nl * sizeof(wchar_t));
}

DISKATLAS_API bool da_ntfs_fixup_record(unsigned char *rec, size_t rec_len, uint16_t sector_size) {
  if (rec_len < (size_t)sector_size || sector_size < 512u) {
    return true;
  }
  uint16_t usa_off = read_u16_le(rec + 4);
  uint16_t usa_count = read_u16_le(rec + 6);
  if (usa_off == 0 || usa_count < 2 || usa_off + usa_count * 2u > rec_len) {
    return false;
  }
  unsigned nsec = (unsigned)(rec_len / sector_size);
  if (usa_count - 1u < nsec) {
    nsec = usa_count - 1u;
  }
  const unsigned char *usa = rec + usa_off;
  for (unsigned i = 0; i < nsec; i++) {
    memcpy(rec + ((size_t)i + 1u) * sector_size - 2u, usa + 2u * (i + 1u), 2u);
  }
  return true;
}

DISKATLAS_API bool da_ntfs_parse_resident_attrs(const unsigned char *rec, size_t rec_len,
                                                 da_ntfs_fn_pick_t *fn_out,
                                  uint64_t *mtime_ns_out, uint32_t *dos_attr_out, int *has_si_out,
                                  uint64_t *data_size_out) {
  uint16_t attr_off = read_u16_le(rec + 0x14);
  uint32_t bytes_used = read_u32_le(rec + 0x18);
  size_t off = attr_off;
  fn_out->score = 0;
  *mtime_ns_out = 0;
  *dos_attr_out = 0;
  *has_si_out = 0;
  if (data_size_out) {
    *data_size_out = 0;
  }

  while (off + 8 <= rec_len && off + 8 <= bytes_used) {
    uint32_t type = read_u32_le(rec + off);
    uint32_t alen = read_u32_le(rec + off + 4);
    if (alen < 24 || off + alen > rec_len) {
      break;
    }
    if (type == 0xFFFFFFFFu) {
      break;
    }
    if (data_size_out && type == DA_NTFS_ATTR_DATA && rec[off + 9] == 0) {
      if (rec[off + 8] != 0 && alen >= 0x38u) {
        *data_size_out = read_u64_le(rec + off + 0x30);
      } else if (rec[off + 8] == 0) {
        *data_size_out = (uint64_t)read_u32_le(rec + off + 0x10);
      }
    }
    if (rec[off + 8] == 0) {
      uint32_t vlen = read_u32_le(rec + off + 0x10);
      uint16_t voff = read_u16_le(rec + off + 0x14);
      const unsigned char *val = rec + off + voff;
      if ((size_t)voff + vlen > alen) {
        off += alen;
        continue;
      }
      if (type == DA_NTFS_ATTR_FILE_NAME && rec[off + 9] == 0) {
        fn_consider(fn_out, val, vlen);
      } else if (type == DA_NTFS_ATTR_STANDARD_INFORMATION && vlen >= 0x28u) {
        uint64_t ft_raw = read_u64_le(val + 0x10);
        if (ft_raw != 0) {
          ULARGE_INTEGER uli;
          uli.QuadPart = ft_raw;
          const uint64_t epoch_diff_100ns = 116444736000000000ULL;
          if (uli.QuadPart >= epoch_diff_100ns) {
            uint64_t rel = uli.QuadPart - epoch_diff_100ns;
            *mtime_ns_out = rel * 100ULL;
          }
        }
        if (vlen >= 0x24u) {
          *dos_attr_out = read_u32_le(val + 0x20);
        }
        *has_si_out = 1;
      }
    }
    off += alen;
  }
  return fn_out->score > 0;
}

#endif /* _WIN32 */
