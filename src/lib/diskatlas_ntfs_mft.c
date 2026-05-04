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
#include <wchar.h>
#include <wctype.h>

#include "diskatlas_internal.h"

#define NTFS_MAGIC_FILE 0x454C4946u /* 'FILE' little-endian */

#define ATTR_STANDARD_INFORMATION 0x10u
#define ATTR_FILE_NAME 0x30u
#define ATTR_DATA 0x80u

#define FILE_RECORD_IN_USE 0x0001u
#define FILE_RECORD_IS_DIRECTORY 0x0002u

#define MFT_REF_MASK 0x0000FFFFFFFFFFFFull

#ifndef NTFS_BOOT_BPS_OFF
#define NTFS_BOOT_BPS_OFF 0x0Bu
#define NTFS_BOOT_SPC_OFF 0x0Du
#define NTFS_BOOT_MFT_LCN_OFF 0x30u
#endif

static const unsigned char k_ntfs_oem[8] = {'N', 'T', 'F', 'S', ' ', ' ', ' ', ' '};

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

static int64_t read_i64_le(const unsigned char *p) {
  uint64_t u =
      (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24) |
      ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) | ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
  return (int64_t)u;
}

static unsigned char is_pow2_u8(unsigned char x) {
  return x != 0 && (x & (x - 1u)) == 0;
}

static uint64_t read_uint_vcni(const unsigned char *p, unsigned size) {
  uint64_t v = 0;
  for (unsigned i = 0; i < size; i++) {
    v |= (uint64_t)p[i] << (8u * i);
  }
  return v;
}

static int64_t read_int_vcni(const unsigned char *p, unsigned size) {
  uint64_t v = read_uint_vcni(p, size);
  unsigned shift = (size < 8 ? (8u - size) * 8u : 0u);
  uint64_t sign = (uint64_t)1 << (size * 8u - 1u);
  if (v & sign) {
    return (int64_t)(v | (~0ULL << (size * 8u)));
  }
  (void)shift;
  return (int64_t)v;
}

typedef struct ntfs_run_seg {
  uint64_t vcn_lo;
  uint64_t vcn_hi_excl;
  int64_t lcn_start;
} ntfs_run_seg_t;

typedef struct ntfs_run_list {
  ntfs_run_seg_t *segs;
  size_t len;
  size_t cap;
} ntfs_run_list_t;

static bool run_list_push(ntfs_run_list_t *rl, uint64_t vlo, uint64_t vhi, int64_t lcn) {
  if (rl->len == rl->cap) {
    size_t nc = rl->cap ? rl->cap * 2u : 32u;
    ntfs_run_seg_t *ns = (ntfs_run_seg_t *)realloc(rl->segs, nc * sizeof(ntfs_run_seg_t));
    if (!ns) {
      return false;
    }
    rl->segs = ns;
    rl->cap = nc;
  }
  rl->segs[rl->len].vcn_lo = vlo;
  rl->segs[rl->len].vcn_hi_excl = vhi;
  rl->segs[rl->len].lcn_start = lcn;
  rl->len++;
  return true;
}

static void run_list_free(ntfs_run_list_t *rl) {
  free(rl->segs);
  rl->segs = NULL;
  rl->len = rl->cap = 0;
}

static bool ntfs_decode_run_list(const unsigned char *runbuf, size_t run_max_bytes,
                                 int64_t lowest_vcn, int64_t highest_vcn, ntfs_run_list_t *out) {
  (void)highest_vcn;
  uint64_t vcn = (lowest_vcn >= 0) ? (uint64_t)lowest_vcn : 0;
  int64_t cur_lcn = -1;
  size_t pos = 0;

  while (pos < run_max_bytes && runbuf[pos] != 0) {
    unsigned char hdr = runbuf[pos++];
    unsigned ll = (unsigned)(hdr & 0xFu);
    unsigned ol = (unsigned)(hdr >> 4);
    if (ll == 0 || ll > 8 || ol > 8 || pos + ll + ol > run_max_bytes) {
      return false;
    }
    uint64_t run_len = read_uint_vcni(runbuf + pos, ll);
    pos += ll;
    if (ol == 0) {
      cur_lcn = -1;
    } else {
      int64_t off = read_int_vcni(runbuf + pos, ol);
      pos += ol;
      if (cur_lcn < 0) {
        cur_lcn = off;
      } else {
        cur_lcn += off;
      }
    }
    uint64_t v_next = vcn + run_len;
    if (!run_list_push(out, vcn, v_next, cur_lcn)) {
      return false;
    }
    vcn = v_next;
  }
  return true;
}

static bool vol_read_at(HANDLE vol, uint64_t off, void *buf, DWORD len) {
  LARGE_INTEGER li;
  li.QuadPart = (LONGLONG)off;
  if (!SetFilePointerEx(vol, li, NULL, FILE_BEGIN)) {
    return false;
  }
  unsigned char *p = (unsigned char *)buf;
  DWORD left = len;
  while (left > 0) {
    DWORD chunk = left > (64u * 1024u) ? (64u * 1024u) : left;
    DWORD got = 0;
    if (!ReadFile(vol, p, chunk, &got, NULL) || got == 0) {
      return false;
    }
    p += got;
    left -= got;
  }
  return true;
}

static bool ntfs_fixup_record(unsigned char *rec, size_t rec_len, uint16_t sector_size) {
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

static const unsigned char *find_unnamed_nonresident_data(const unsigned char *rec,
                                                            size_t rec_len,
                                                            uint16_t *run_off_out,
                                                            uint32_t *attr_total_len_out) {
  uint16_t attr_off = read_u16_le(rec + 0x14);
  uint32_t bytes_used = read_u32_le(rec + 0x18);
  size_t off = attr_off;
  while (off + 8 <= rec_len && off + 8 <= bytes_used) {
    uint32_t type = read_u32_le(rec + off);
    uint32_t alen = read_u32_le(rec + off + 4);
    if (alen < 64 || off + alen > rec_len) {
      break;
    }
    if (type == 0xFFFFFFFFu) {
      break;
    }
    if (type == ATTR_DATA && rec[off + 8] != 0 && rec[off + 9] == 0) {
      uint16_t ro = read_u16_le(rec + off + 0x20);
      if (run_off_out) {
        *run_off_out = ro;
      }
      if (attr_total_len_out) {
        *attr_total_len_out = alen;
      }
      return rec + off;
    }
    off += alen;
  }
  return NULL;
}

static bool mft_stream_read(HANDLE vol, uint64_t cluster_bytes, const ntfs_run_list_t *runs,
                            uint64_t stream_off, unsigned char *dst, size_t len) {
  while (len > 0) {
    uint64_t vcn = stream_off / cluster_bytes;
    size_t off_in_cluster = (size_t)(stream_off % cluster_bytes);
    size_t chunk = cluster_bytes - off_in_cluster;
    if (chunk > len) {
      chunk = len;
    }

    const ntfs_run_seg_t *seg = NULL;
    for (size_t i = 0; i < runs->len; i++) {
      if (vcn >= runs->segs[i].vcn_lo && vcn < runs->segs[i].vcn_hi_excl) {
        seg = &runs->segs[i];
        break;
      }
    }
    if (!seg) {
      memset(dst, 0, chunk);
    } else if (seg->lcn_start < 0) {
      memset(dst, 0, chunk);
    } else {
      uint64_t lcn = (uint64_t)seg->lcn_start + (vcn - seg->vcn_lo);
      uint64_t vol_off = lcn * cluster_bytes + off_in_cluster;
      if (!vol_read_at(vol, vol_off, dst, (DWORD)chunk)) {
        return false;
      }
    }
    dst += chunk;
    stream_off += chunk;
    len -= chunk;
  }
  return true;
}

typedef struct fn_pick {
  uint32_t score;
  uint64_t parent_id;
  uint64_t real_size;
  uint16_t name_chars;
  wchar_t name_buf[260];
} fn_pick_t;

static uint32_t fn_namespace_score(unsigned char ns) {
  if (ns == 1u || ns == 3u) {
    return 1000u;
  }
  if (ns == 2u) {
    return 100u;
  }
  return 50u;
}

static void fn_consider(fn_pick_t *best, const unsigned char *val, uint32_t val_len) {
  if (val_len < 0x44u) {
    return;
  }
  uint64_t parent = read_u64_le(val) & MFT_REF_MASK;
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

static bool parse_resident_attrs(const unsigned char *rec, size_t rec_len, fn_pick_t *fn_out,
                                 uint64_t *mtime_ns_out, uint32_t *dos_attr_out,
                                 int *has_si_out) {
  uint16_t attr_off = read_u16_le(rec + 0x14);
  uint32_t bytes_used = read_u32_le(rec + 0x18);
  size_t off = attr_off;
  fn_out->score = 0;
  *mtime_ns_out = 0;
  *dos_attr_out = 0;
  *has_si_out = 0;

  while (off + 8 <= rec_len && off + 8 <= bytes_used) {
    uint32_t type = read_u32_le(rec + off);
    uint32_t alen = read_u32_le(rec + off + 4);
    if (alen < 24 || off + alen > rec_len) {
      break;
    }
    if (type == 0xFFFFFFFFu) {
      break;
    }
    if (rec[off + 8] == 0) {
      uint32_t vlen = read_u32_le(rec + off + 0x10);
      uint16_t voff = read_u16_le(rec + off + 0x14);
      const unsigned char *val = rec + off + voff;
      if ((size_t)voff + vlen > alen) {
        off += alen;
        continue;
      }
      if (type == ATTR_FILE_NAME && rec[off + 9] == 0) {
        fn_consider(fn_out, val, vlen);
      } else if (type == ATTR_STANDARD_INFORMATION && vlen >= 0x28u) {
        uint64_t ft_raw = read_u64_le(val + 0x10);
        if (ft_raw != 0) {
          ULARGE_INTEGER uli;
          uli.QuadPart = ft_raw;
          FILETIME ft;
          ft.dwLowDateTime = uli.LowPart;
          ft.dwHighDateTime = uli.HighPart;
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

static wchar_t *wcs_dup_range(const wchar_t *s, size_t nchars) {
  size_t bytes = (nchars + 1u) * sizeof(wchar_t);
  wchar_t *r = (wchar_t *)malloc(bytes);
  if (!r) {
    return NULL;
  }
  memcpy(r, s, nchars * sizeof(wchar_t));
  r[nchars] = L'\0';
  return r;
}

static bool wcs_has_prefix_ci(const wchar_t *s, const wchar_t *pre) {
  for (; *pre; ++s, ++pre) {
    if (towlower((wint_t)*s) != towlower((wint_t)*pre)) {
      return false;
    }
  }
  return *s == L'\0' || *s == L'\\';
}

static uint32_t depth_below_scan_root(const wchar_t *full, const wchar_t *root_norm) {
  size_t lr = wcslen(root_norm);
  size_t i = 0;
  for (; i < lr && full[i] && root_norm[i]; i++) {
    if (towlower((wint_t)full[i]) != towlower((wint_t)root_norm[i])) {
      return UINT32_MAX;
    }
  }
  if (root_norm[i] != L'\0') {
    return UINT32_MAX;
  }
  const wchar_t *rel = full + lr;
  if (*rel == L'\\') {
    rel++;
  }
  if (*rel == L'\0') {
    return 0;
  }
  uint32_t d = 1;
  for (; *rel; rel++) {
    if (*rel == L'\\') {
      d++;
    }
  }
  return d;
}

static wchar_t *path_for_index(uint64_t idx, wchar_t **cache, wchar_t **names,
                               uint64_t *parents, unsigned char *valid, size_t record_count,
                               const wchar_t *vol_root, unsigned depth) {
  if (idx >= record_count || depth > 128u) {
    return NULL;
  }
  if (cache[idx]) {
    return cache[idx];
  }
  if (idx == 5u) {
    wchar_t *vr = wcs_dup_range(vol_root, wcslen(vol_root));
    if (vr) {
      cache[idx] = vr;
    }
    return vr;
  }
  if (!valid[idx] || !names[idx]) {
    return NULL;
  }

  uint64_t par = parents[idx];
  if (par == idx) {
    size_t vrl = wcslen(vol_root);
    size_t nl = wcslen(names[idx]);
    wchar_t *buf = (wchar_t *)malloc((vrl + 1u + nl + 1u) * sizeof(wchar_t));
    if (!buf) {
      return NULL;
    }
    memcpy(buf, vol_root, vrl * sizeof(wchar_t));
    size_t pos = vrl;
    if (pos == 0 || (buf[pos - 1] != L'\\' && buf[pos - 1] != L'/')) {
      buf[pos++] = L'\\';
    }
    memcpy(buf + pos, names[idx], (nl + 1u) * sizeof(wchar_t));
    cache[idx] = buf;
    return buf;
  }

  wchar_t *pp = path_for_index(par, cache, names, parents, valid, record_count, vol_root,
                               depth + 1u);
  if (!pp) {
    return NULL;
  }
  size_t pl = wcslen(pp);
  size_t nl = wcslen(names[idx]);
  wchar_t *full = (wchar_t *)malloc((pl + 1u + nl + 1u) * sizeof(wchar_t));
  if (!full) {
    return NULL;
  }
  memcpy(full, pp, pl * sizeof(wchar_t));
  size_t pos = pl;
  if (pos == 0 || (full[pos - 1] != L'\\' && full[pos - 1] != L'/')) {
    full[pos++] = L'\\';
  }
  memcpy(full + pos, names[idx], (nl + 1u) * sizeof(wchar_t));
  cache[idx] = full;
  return full;
}

bool diskatlas_scan_ntfs_mft(diskatlas_scan_result_t *r, wchar_t *root_path_wide,
                             const scan_options_t *opts) {
  if (!r || !root_path_wide || !opts) {
    return false;
  }

  wchar_t vol_path_full[MAX_PATH + 4];
  if (!GetVolumePathNameW(root_path_wide, vol_path_full, MAX_PATH)) {
    return false;
  }

  if (vol_path_full[0] == L'\0' || vol_path_full[1] != L':' || vol_path_full[2] != L'\\') {
    return false;
  }

  wchar_t dev[16];
  dev[0] = L'\\';
  dev[1] = L'\\';
  dev[2] = L'.';
  dev[3] = L'\\';
  dev[4] = vol_path_full[0];
  dev[5] = L':';
  dev[6] = L'\0';

  HANDLE vol = CreateFileW(dev, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, 0, NULL);
  if (vol == INVALID_HANDLE_VALUE) {
    return false;
  }

  unsigned char boot[4096];
  DWORD bgot = 0;
  if (!ReadFile(vol, boot, sizeof(boot), &bgot, NULL) || bgot < 512u) {
    CloseHandle(vol);
    return false;
  }

  if (memcmp(boot + 3, k_ntfs_oem, sizeof(k_ntfs_oem)) != 0) {
    CloseHandle(vol);
    return false;
  }

  uint16_t bps = read_u16_le(boot + NTFS_BOOT_BPS_OFF);
  unsigned char spc_u = boot[NTFS_BOOT_SPC_OFF];
  if (bps == 0 || (bps % 512u) != 0 || spc_u == 0 || !is_pow2_u8(spc_u)) {
    CloseHandle(vol);
    return false;
  }

  uint64_t cluster_bytes = (uint64_t)bps * (uint64_t)spc_u;
  int64_t mft_lcn_i = read_i64_le(boot + NTFS_BOOT_MFT_LCN_OFF);
  if (mft_lcn_i < 0) {
    CloseHandle(vol);
    return false;
  }

  uint64_t mft_vol_off = (uint64_t)mft_lcn_i * cluster_bytes;

  int8_t cpr = (int8_t)boot[0x40];
  uint32_t record_size = 1024;
  if (cpr < 0) {
    int exp = -(int)cpr;
    if (exp >= 1 && exp <= 31) {
      record_size = 1u << (unsigned)exp;
    }
  } else if (cpr > 0) {
    uint64_t rs = (uint64_t)(unsigned char)cpr * cluster_bytes;
    if (rs >= 512 && rs <= (uint64_t)(1024 * 1024)) {
      record_size = (uint32_t)rs;
    }
  }

  unsigned char *rec0 = (unsigned char *)malloc(record_size);
  if (!rec0) {
    CloseHandle(vol);
    return false;
  }
  if (!vol_read_at(vol, mft_vol_off, rec0, record_size)) {
    free(rec0);
    CloseHandle(vol);
    return false;
  }
  if (!ntfs_fixup_record(rec0, record_size, bps)) {
    free(rec0);
    CloseHandle(vol);
    return false;
  }
  if (read_u32_le(rec0) != NTFS_MAGIC_FILE) {
    free(rec0);
    CloseHandle(vol);
    return false;
  }

  uint16_t run_rel = 0;
  uint32_t attr_len = 0;
  const unsigned char *adata = find_unnamed_nonresident_data(rec0, record_size, &run_rel, &attr_len);
  if (!adata || run_rel >= attr_len) {
    free(rec0);
    CloseHandle(vol);
    return false;
  }

  int64_t lowest_vcn = read_i64_le(adata + 0x10);
  int64_t highest_vcn = read_i64_le(adata + 0x18);
  uint64_t real_size = read_u64_le(adata + 0x30);
  (void)highest_vcn;

  const unsigned char *runs_buf = adata + run_rel;
  size_t runs_max = attr_len - run_rel;
  ntfs_run_list_t runs = {0};
  if (!ntfs_decode_run_list(runs_buf, runs_max, lowest_vcn, highest_vcn, &runs)) {
    free(rec0);
    run_list_free(&runs);
    CloseHandle(vol);
    return false;
  }
  free(rec0);

  if (real_size == 0 || record_size == 0 || real_size / (uint64_t)record_size > (uint64_t)SIZE_MAX / 8u) {
    run_list_free(&runs);
    CloseHandle(vol);
    return false;
  }

  size_t record_count = (size_t)(real_size / (uint64_t)record_size);
  uint64_t *parents = (uint64_t *)calloc(record_count, sizeof(uint64_t));
  wchar_t **names = (wchar_t **)calloc(record_count, sizeof(wchar_t *));
  unsigned char *valid = (unsigned char *)calloc(record_count, 1);
  unsigned char *is_dir = (unsigned char *)calloc(record_count, 1);
  uint64_t *sizes = (uint64_t *)calloc(record_count, sizeof(uint64_t));
  uint64_t *mtimes = (uint64_t *)calloc(record_count, sizeof(uint64_t));
  uint32_t *dosattrs = (uint32_t *)calloc(record_count, sizeof(uint32_t));
  wchar_t **path_cache = (wchar_t **)calloc(record_count, sizeof(wchar_t *));

  if (!parents || !names || !valid || !is_dir || !sizes || !mtimes || !dosattrs || !path_cache) {
    free(parents);
    if (names) {
      for (size_t i = 0; i < record_count; i++) {
        free(names[i]);
      }
    }
    free(names);
    free(valid);
    free(is_dir);
    free(sizes);
    free(mtimes);
    free(dosattrs);
    free(path_cache);
    run_list_free(&runs);
    CloseHandle(vol);
    return false;
  }

  unsigned char *recbuf = (unsigned char *)malloc(record_size);
  if (!recbuf) {
    free(parents);
    for (size_t i = 0; i < record_count; i++) {
      free(names[i]);
    }
    free(names);
    free(valid);
    free(is_dir);
    free(sizes);
    free(mtimes);
    free(dosattrs);
    free(path_cache);
    run_list_free(&runs);
    CloseHandle(vol);
    return false;
  }

  for (size_t idx = 0; idx < record_count; idx++) {
    if (atomic_load_explicit(&r->cancel, memory_order_relaxed) != 0) {
      break;
    }
    atomic_fetch_add_explicit(&r->entry_visits, 1, memory_order_relaxed);

    uint64_t off = (uint64_t)idx * (uint64_t)record_size;
    if (!mft_stream_read(vol, cluster_bytes, &runs, off, recbuf, record_size)) {
      continue;
    }
    if (!ntfs_fixup_record(recbuf, record_size, bps)) {
      continue;
    }
    if (read_u32_le(recbuf) != NTFS_MAGIC_FILE) {
      continue;
    }

    uint16_t flags = read_u16_le(recbuf + 0x16);
    if ((flags & FILE_RECORD_IN_USE) == 0) {
      continue;
    }

    uint64_t base = read_u64_le(recbuf + 0x20) & MFT_REF_MASK;
    if (base != 0) {
      continue;
    }

    fn_pick_t fn;
    uint64_t mtime_ns = 0;
    uint32_t dattr = 0;
    int has_si = 0;
    if (!parse_resident_attrs(recbuf, record_size, &fn, &mtime_ns, &dattr, &has_si)) {
      continue;
    }

    valid[idx] = 1;
    parents[idx] = fn.parent_id;
    sizes[idx] = fn.real_size;
    mtimes[idx] = mtime_ns;
    dosattrs[idx] = dattr;
    is_dir[idx] = (unsigned char)((flags & FILE_RECORD_IS_DIRECTORY) != 0 ? 1 : 0);
    if (fn.name_chars > 0) {
      wchar_t *nm = wcs_dup_range(fn.name_buf, fn.name_chars);
      if (nm) {
        free(names[idx]);
        names[idx] = nm;
      }
    }
    if (!has_si) {
      dosattrs[idx] = is_dir[idx] ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE;
    }
  }

  const uint32_t max_depth_req = opts->max_depth;
  const bool include_hidden = (opts->flags & DISKATLAS_SCAN_OPTION_INCLUDE_HIDDEN) != 0;

  for (size_t idx = 0; idx < record_count; idx++) {
    if (atomic_load_explicit(&r->cancel, memory_order_relaxed) != 0) {
      break;
    }
    if (!valid[idx]) {
      continue;
    }
    wchar_t *full =
        path_for_index((uint64_t)idx, path_cache, names, parents, valid, record_count, vol_path_full, 0);
    if (!full) {
      continue;
    }
    if (wcscmp(full, root_path_wide) == 0 || wcscmp(full, vol_path_full) == 0) {
      continue;
    }
    if (!wcs_has_prefix_ci(full, root_path_wide)) {
      continue;
    }
    if (max_depth_req != 0) {
      uint32_t dep = depth_below_scan_root(full, root_path_wide);
      if (dep == UINT32_MAX || dep > max_depth_req) {
        continue;
      }
    }
    if (!include_hidden &&
        (dosattrs[idx] & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0) {
      continue;
    }

    bool dir_f = is_dir[idx] != 0;
    if (!diskatlas_win32_record_entry_metadata(r, full, sizes[idx], mtimes[idx], dosattrs[idx],
                                               dir_f)) {
      break;
    }
  }

  free(recbuf);
  free(parents);
  for (size_t i = 0; i < record_count; i++) {
    free(names[i]);
    free(path_cache[i]);
  }
  free(names);
  free(path_cache);
  free(valid);
  free(is_dir);
  free(sizes);
  free(mtimes);
  free(dosattrs);
  run_list_free(&runs);
  CloseHandle(vol);

  return true;
}

#endif /* _WIN32 */
