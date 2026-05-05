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
#include <winioctl.h>

/* MinGW may omit FILE_ID_INFO / FileIdInfo unless a newer WINNT macro is set; define locally. */
#ifndef FileIdInfo
#define FileIdInfo 18
#endif
typedef struct da_file_id_128 {
  unsigned char Identifier[16];
} da_file_id_128_t;
typedef struct da_file_id_info {
  unsigned long long VolumeSerialNumber;
  da_file_id_128_t FileId;
} da_file_id_info_t;

#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#include <stddef.h>

#include "diskatlas_internal.h"

/*
 * FIXME(ntfs-mft): Raw NTFS $MFT enumeration is experimental — attribute-list records, security
 * descriptors, all stream types, and edge cases are not fully handled. Search for FIXME(ntfs-mft).
 */

#define NTFS_MAGIC_FILE 0x454C4946u /* 'FILE' little-endian */

#define ATTR_STANDARD_INFORMATION 0x10u
#define ATTR_ATTRIBUTE_LIST 0x20u
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

/** Total logical $DATA stream size implied by decoded runs (sparse holes included). */
static uint64_t ntfs_run_list_stream_total_bytes(const ntfs_run_list_t *runs, uint64_t cluster_bytes) {
  uint64_t total = 0;
  for (size_t i = 0; i < runs->len; i++) {
    total += (runs->segs[i].vcn_hi_excl - runs->segs[i].vcn_lo) * cluster_bytes;
  }
  return total;
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

static void enable_backup_privilege_best_effort(void) {
  HANDLE tok = NULL;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) {
    return;
  }
  TOKEN_PRIVILEGES tp;
  memset(&tp, 0, sizeof(tp));
  tp.PrivilegeCount = 1;
  tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  if (LookupPrivilegeValueW(NULL, L"SeBackupPrivilege", &tp.Privileges[0].Luid)) {
    (void)AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), NULL, NULL);
  }
  CloseHandle(tok);
}

static int cmp_seg_vcn_lo(const void *a, const void *b) {
  const ntfs_run_seg_t *x = (const ntfs_run_seg_t *)a;
  const ntfs_run_seg_t *y = (const ntfs_run_seg_t *)b;
  if (x->vcn_lo < y->vcn_lo) {
    return -1;
  }
  if (x->vcn_lo > y->vcn_lo) {
    return 1;
  }
  return 0;
}

static bool append_retrieval_buffer_to_runs(ntfs_run_list_t *rl, unsigned char *buf, DWORD br) {
  if (br < sizeof(RETRIEVAL_POINTERS_BUFFER)) {
    return false;
  }
  RETRIEVAL_POINTERS_BUFFER *rp = (RETRIEVAL_POINTERS_BUFFER *)buf;
  ULONG ec = rp->ExtentCount;
  size_t need = offsetof(RETRIEVAL_POINTERS_BUFFER, Extents) + (size_t)ec * sizeof(rp->Extents[0]);
  if (br < need) {
    return false;
  }
  uint64_t cur_vcn = (uint64_t)rp->StartingVcn.QuadPart;
  for (ULONG i = 0; i < ec; i++) {
    uint64_t next_vcn = (uint64_t)rp->Extents[i].NextVcn.QuadPart;
    int64_t lcn = (int64_t)rp->Extents[i].Lcn.QuadPart;
    if (next_vcn > cur_vcn) {
      if (!run_list_push(rl, cur_vcn, next_vcn, lcn)) {
        return false;
      }
    }
    cur_vcn = next_vcn;
  }
  return true;
}

static bool build_runs_from_retrieval_pointers(HANDLE hf, ntfs_run_list_t *out_runs) {
  LARGE_INTEGER next_in;
  next_in.QuadPart = 0;
  unsigned char buf[256 * 1024];
  for (;;) {
    STARTING_VCN_INPUT_BUFFER in_vc;
    memset(&in_vc, 0, sizeof(in_vc));
    in_vc.StartingVcn = next_in;
    DWORD br = 0;
    if (!DeviceIoControl(hf, FSCTL_GET_RETRIEVAL_POINTERS, &in_vc, sizeof(in_vc), buf,
                         (DWORD)sizeof(buf), &br, NULL)) {
      DWORD err = GetLastError();
      if (next_in.QuadPart > 0 &&
          (err == ERROR_INVALID_PARAMETER || err == ERROR_HANDLE_EOF || err == ERROR_NO_MORE_FILES)) {
        break;
      }
      return false;
    }
    RETRIEVAL_POINTERS_BUFFER *rp = (RETRIEVAL_POINTERS_BUFFER *)buf;
    if (br < sizeof(RETRIEVAL_POINTERS_BUFFER) || rp->ExtentCount == 0) {
      break;
    }
    if (!append_retrieval_buffer_to_runs(out_runs, buf, br)) {
      return false;
    }
    ULONG ec = rp->ExtentCount;
    uint64_t last_next = (uint64_t)rp->Extents[ec - 1].NextVcn.QuadPart;
    next_in.QuadPart = (LONGLONG)last_next;
  }
  if (out_runs->len > 1) {
    qsort(out_runs->segs, out_runs->len, sizeof(ntfs_run_seg_t), cmp_seg_vcn_lo);
  }
  return out_runs->len > 0;
}

/**
 * Opens \\?\\X:\\$MFT and builds the full volume runlist via FSCTL_GET_RETRIEVAL_POINTERS.
 * Avoids relying on the primary $MFT FILE record when $DATA is split via $ATTRIBUTE_LIST.
 */
static bool try_load_mft_runs_via_special_file(wchar_t drive_letter, ntfs_run_list_t *runs_out,
                                               uint64_t *stream_total_out) {
  enable_backup_privilege_best_effort();
  const wchar_t *fmt[2] = {L"\\\\?\\%c:\\$MFT", L"\\\\.\\%c:\\$MFT"};
  wchar_t path[32];
  HANDLE hf = INVALID_HANDLE_VALUE;
  for (int fi = 0; fi < 2; fi++) {
    swprintf(path, 32, fmt[fi], drive_letter);
    hf =
        CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (hf != INVALID_HANDLE_VALUE) {
      break;
    }
    hf = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                     NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf != INVALID_HANDLE_VALUE) {
      break;
    }
  }
  if (hf == INVALID_HANDLE_VALUE) {
    return false;
  }
  LARGE_INTEGER fsz = {{0}};
  if (!GetFileSizeEx(hf, &fsz)) {
    CloseHandle(hf);
    return false;
  }
  bool ok = build_runs_from_retrieval_pointers(hf, runs_out);
  CloseHandle(hf);
  if (!ok || runs_out->len == 0) {
    run_list_free(runs_out);
    return false;
  }
  *stream_total_out = (uint64_t)fsz.QuadPart;
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

static const unsigned char *find_best_unnamed_nonresident_data(const unsigned char *rec,
                                                                 size_t rec_len,
                                                                 uint16_t *run_off_out,
                                                                 uint32_t *attr_total_len_out) {
  uint16_t attr_off = read_u16_le(rec + 0x14);
  uint32_t bytes_used = read_u32_le(rec + 0x18);
  size_t off = attr_off;
  const unsigned char *best = NULL;
  uint64_t best_rs = 0;
  uint16_t best_ro = 0;
  uint32_t best_alen = 0;

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
      uint64_t rs = read_u64_le(rec + off + 0x30);
      if (rs >= best_rs) {
        best_rs = rs;
        best = rec + off;
        best_ro = read_u16_le(rec + off + 0x20);
        best_alen = alen;
      }
    }
    off += alen;
  }
  if (best != NULL && run_off_out != NULL) {
    *run_off_out = best_ro;
  }
  if (best != NULL && attr_total_len_out != NULL) {
    *attr_total_len_out = best_alen;
  }
  return best;
}

static bool mft_stream_read(HANDLE vol, uint64_t cluster_bytes, const ntfs_run_list_t *runs,
                            uint64_t stream_base_vcn, uint64_t stream_off, unsigned char *dst,
                            size_t len) {
  while (len > 0) {
    uint64_t abs_vcn = stream_base_vcn + stream_off / cluster_bytes;
    size_t off_in_cluster = (size_t)(stream_off % cluster_bytes);
    size_t chunk = cluster_bytes - off_in_cluster;
    if (chunk > len) {
      chunk = len;
    }

    const ntfs_run_seg_t *seg = NULL;
    for (size_t i = 0; i < runs->len; i++) {
      if (abs_vcn >= runs->segs[i].vcn_lo && abs_vcn < runs->segs[i].vcn_hi_excl) {
        seg = &runs->segs[i];
        break;
      }
    }
    if (!seg) {
      memset(dst, 0, chunk);
    } else if (seg->lcn_start < 0) {
      memset(dst, 0, chunk);
    } else {
      uint64_t lcn = (uint64_t)seg->lcn_start + (abs_vcn - seg->vcn_lo);
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

/* ---------- Memory-mapped $MFT stream (fragmented runs, sparse = zeros) ---------- */

typedef struct mft_stream_slice {
  uint64_t stream_lo;
  uint64_t stream_hi_excl;
  uint8_t *read_ptr;
} mft_stream_slice_t;

typedef struct mft_stream_mmap {
  HANDLE vol;
  HANDLE file_mapping;
  mft_stream_slice_t *slices;
  size_t n_slices;
  uint8_t **unmap_bases;
  size_t n_unmap;
  uint64_t cluster_bytes;
  uint64_t stream_total_bytes;
  unsigned char fallback_active;
} mft_stream_mmap_t;

static void mft_mmap_destroy(mft_stream_mmap_t *m) {
  if (!m) {
    return;
  }
  for (size_t i = 0; i < m->n_unmap; i++) {
    if (m->unmap_bases[i] != NULL) {
      (void)UnmapViewOfFile(m->unmap_bases[i]);
    }
  }
  free(m->unmap_bases);
  m->unmap_bases = NULL;
  m->n_unmap = 0;
  free(m->slices);
  m->slices = NULL;
  m->n_slices = 0;
  if (m->file_mapping != NULL && m->file_mapping != INVALID_HANDLE_VALUE) {
    CloseHandle(m->file_mapping);
    m->file_mapping = NULL;
  }
  m->vol = NULL;
  m->cluster_bytes = 0;
  m->fallback_active = 0;
}

static int cmp_mft_slice_lo(const void *a, const void *b) {
  const mft_stream_slice_t *x = (const mft_stream_slice_t *)a;
  const mft_stream_slice_t *y = (const mft_stream_slice_t *)b;
  if (x->stream_lo < y->stream_lo) {
    return -1;
  }
  if (x->stream_lo > y->stream_lo) {
    return 1;
  }
  return 0;
}

static bool mft_mmap_try_build(mft_stream_mmap_t *out, HANDLE vol, uint64_t cluster_bytes,
                               const ntfs_run_list_t *runs, uint64_t stream_total_bytes) {
  memset(out, 0, sizeof(*out));
  out->vol = vol;
  out->cluster_bytes = cluster_bytes;
  out->stream_total_bytes = stream_total_bytes;
  out->file_mapping = CreateFileMappingW(vol, NULL, PAGE_READONLY, 0, 0, NULL);
  if (out->file_mapping == NULL || out->file_mapping == INVALID_HANDLE_VALUE) {
    out->file_mapping = NULL;
    return false;
  }

  SYSTEM_INFO si;
  GetSystemInfo(&si);
  uint64_t gran = si.dwAllocationGranularity;
  if (gran == 0) {
    gran = 65536;
  }

  uint64_t stream_acc = 0;
  for (size_t ri = 0; ri < runs->len; ri++) {
    const ntfs_run_seg_t *sg = &runs->segs[ri];
    uint64_t run_bytes = (sg->vcn_hi_excl - sg->vcn_lo) * cluster_bytes;
    uint64_t s_lo = stream_acc;
    stream_acc += run_bytes;

    if (sg->lcn_start < 0 || run_bytes == 0) {
      continue;
    }

    uint64_t vol_off = (uint64_t)sg->lcn_start * cluster_bytes;
    uint64_t vol_off_al = (vol_off / gran) * gran;
    size_t skew = (size_t)(vol_off - vol_off_al);
    SIZE_T map_len = (SIZE_T)(skew + run_bytes);
    if (skew + run_bytes > (uint64_t)(SIZE_MAX / 2)) {
      mft_mmap_destroy(out);
      memset(out, 0, sizeof(*out));
      return false;
    }

    DWORD fh = (DWORD)(vol_off_al >> 32);
    DWORD fl = (DWORD)vol_off_al;
    uint8_t *base =
        (uint8_t *)MapViewOfFile(out->file_mapping, FILE_MAP_READ, fh, fl, map_len);
    if (base == NULL) {
      mft_mmap_destroy(out);
      memset(out, 0, sizeof(*out));
      return false;
    }

    uint8_t **nb = (uint8_t **)realloc(out->unmap_bases, (out->n_unmap + 1u) * sizeof(uint8_t *));
    if (!nb) {
      UnmapViewOfFile(base);
      mft_mmap_destroy(out);
      memset(out, 0, sizeof(*out));
      return false;
    }
    out->unmap_bases = nb;
    out->unmap_bases[out->n_unmap++] = base;

    mft_stream_slice_t sl;
    sl.stream_lo = s_lo;
    sl.stream_hi_excl = s_lo + run_bytes;
    sl.read_ptr = base + skew;

    mft_stream_slice_t *ns =
        (mft_stream_slice_t *)realloc(out->slices, (out->n_slices + 1u) * sizeof(mft_stream_slice_t));
    if (!ns) {
      mft_mmap_destroy(out);
      memset(out, 0, sizeof(*out));
      return false;
    }
    out->slices = ns;
    out->slices[out->n_slices++] = sl;
  }

  if (out->n_slices > 1) {
    qsort(out->slices, out->n_slices, sizeof(mft_stream_slice_t), cmp_mft_slice_lo);
  }
  return true;
}

static const uint8_t *mft_mmap_ptr_at(mft_stream_mmap_t *m, uint64_t stream_off, size_t *avail_out) {
  if (avail_out) {
    *avail_out = 0;
  }
  size_t lo = 0;
  size_t hi = m->n_slices;
  while (lo < hi) {
    size_t mid = (lo + hi) >> 1;
    const mft_stream_slice_t *s = &m->slices[mid];
    if (stream_off < s->stream_lo) {
      hi = mid;
    } else if (stream_off >= s->stream_hi_excl) {
      lo = mid + 1;
    } else {
      size_t off_in = (size_t)(stream_off - s->stream_lo);
      size_t avail = (size_t)(s->stream_hi_excl - stream_off);
      if (avail_out) {
        *avail_out = avail;
      }
      return s->read_ptr + off_in;
    }
  }
  return NULL;
}

static void mft_mmap_read(mft_stream_mmap_t *m, uint64_t stream_off, unsigned char *dst,
                            size_t len) {
  while (len > 0) {
    if (m->stream_total_bytes != 0 && stream_off >= m->stream_total_bytes) {
      memset(dst, 0, len);
      return;
    }
    if (m->stream_total_bytes != 0) {
      uint64_t cap = m->stream_total_bytes - stream_off;
      if (cap < len) {
        len = (size_t)cap;
      }
    }
    size_t avail = 0;
    const uint8_t *p = mft_mmap_ptr_at(m, stream_off, &avail);
    size_t chunk = len;
    if (p != NULL && avail > 0) {
      if (chunk > avail) {
        chunk = avail;
      }
      memcpy(dst, p, chunk);
    } else {
      uint64_t next_bound =
          (m->stream_total_bytes != 0) ? m->stream_total_bytes : UINT64_MAX;
      for (size_t si = 0; si < m->n_slices; si++) {
        if (m->slices[si].stream_lo > stream_off && m->slices[si].stream_lo < next_bound) {
          next_bound = m->slices[si].stream_lo;
        }
      }
      uint64_t zero_run = len;
      if (next_bound > stream_off) {
        zero_run = next_bound - stream_off;
      }
      if (zero_run > len) {
        zero_run = len;
      }
      chunk = (size_t)zero_run;
      if (chunk == 0) {
        chunk = len;
      }
      memset(dst, 0, chunk);
    }
    dst += chunk;
    stream_off += chunk;
    len -= chunk;
  }
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

/** Attribute list entry prefix (unnamed attributes); optional UTF-16 name may follow. */
#define ATTR_LIST_ENTRY_MIN 26u

static bool resident_attr_value_unnamed(const unsigned char *rec, size_t rec_len, uint32_t want_type,
                                        const unsigned char **val_out, uint32_t *vlen_out) {
  uint16_t attr_off = read_u16_le(rec + 0x14);
  uint32_t bytes_used = read_u32_le(rec + 0x18);
  size_t off = attr_off;
  while (off + 8 <= rec_len && off + 8 <= bytes_used) {
    uint32_t type = read_u32_le(rec + off);
    uint32_t alen = read_u32_le(rec + off + 4);
    if (alen < 24 || off + alen > rec_len) {
      break;
    }
    if (type == 0xFFFFFFFFu) {
      break;
    }
    if (type == want_type && rec[off + 8] == 0 && rec[off + 9] == 0) {
      uint32_t vlen = read_u32_le(rec + off + 0x10);
      uint16_t voff = read_u16_le(rec + off + 0x14);
      if ((size_t)voff + vlen > alen) {
        return false;
      }
      *val_out = rec + off + voff;
      *vlen_out = vlen;
      return true;
    }
    off += alen;
  }
  return false;
}

typedef struct mft_parse_shared {
  diskatlas_scan_result_t *r;
  atomic_size_t *next_idx;
  size_t record_count;
  uint32_t record_size;
  uint16_t bps;
  uint64_t cluster_bytes;
  uint64_t stream_base_vcn;
  HANDLE vol;
  ntfs_run_list_t *runs;
  mft_stream_mmap_t *mmap;
  int use_mmap;
  uint64_t *parents;
  wchar_t **names;
  unsigned char *valid;
  unsigned char *is_dir;
  uint64_t *sizes;
  uint64_t *mtimes;
  uint32_t *dosattrs;
} mft_parse_shared_t;

static bool mft_load_record_raw(mft_parse_shared_t *s, size_t idx, unsigned char *buf) {
  uint64_t off = (uint64_t)idx * (uint64_t)s->record_size;
  if (s->use_mmap) {
    mft_mmap_read(s->mmap, off, buf, (size_t)s->record_size);
  } else if (!mft_stream_read(s->vol, s->cluster_bytes, s->runs, s->stream_base_vcn, off, buf,
                              (size_t)s->record_size)) {
    return false;
  }
  if (!ntfs_fixup_record(buf, (size_t)s->record_size, s->bps)) {
    return false;
  }
  if (read_u32_le(buf) != NTFS_MAGIC_FILE) {
    return false;
  }
  uint16_t flags = read_u16_le(buf + 0x16);
  if ((flags & FILE_RECORD_IN_USE) == 0) {
    return false;
  }
  return true;
}

/**
 * Base FILE record may hold only $ATTRIBUTE_LIST; $FILE_NAME lives in an extension record.
 * Walk the list and merge filename (and SI if still missing) from the referenced segment.
 */
static bool parse_fn_via_attribute_list(const unsigned char *base_rec, size_t base_len,
                                        mft_parse_shared_t *s, size_t home_idx, fn_pick_t *fn_out,
                                        uint64_t *mtime_ns_out, uint32_t *dos_attr_out,
                                        int *has_si_out, unsigned char *ext_buf) {
  const unsigned char *alist = NULL;
  uint32_t alist_len = 0;
  if (!resident_attr_value_unnamed(base_rec, base_len, ATTR_ATTRIBUTE_LIST, &alist, &alist_len)) {
    return false;
  }

  size_t pos = 0;
  while (pos + ATTR_LIST_ENTRY_MIN <= alist_len) {
    uint32_t ent_type = read_u32_le(alist + pos);
    uint16_t ent_len = read_u16_le(alist + pos + 4);
    if (ent_len < ATTR_LIST_ENTRY_MIN || pos + ent_len > alist_len) {
      break;
    }
    if (ent_type == ATTR_FILE_NAME) {
      uint64_t ref = read_u64_le(alist + pos + 16) & MFT_REF_MASK;
      pos += ent_len;
      if (ref == 0 || ref >= s->record_count || ref == home_idx) {
        continue;
      }
      if (!mft_load_record_raw(s, (size_t)ref, ext_buf)) {
        continue;
      }
      fn_pick_t fn_try;
      uint64_t mt = 0;
      uint32_t da = 0;
      int hs = 0;
      memset(&fn_try, 0, sizeof(fn_try));
      if (!parse_resident_attrs(ext_buf, (size_t)s->record_size, &fn_try, &mt, &da, &hs)) {
        continue;
      }
      if (fn_try.score > 0) {
        *fn_out = fn_try;
        if (!*has_si_out && hs) {
          *mtime_ns_out = mt;
          *dos_attr_out = da;
          *has_si_out = 1;
        }
        return true;
      }
      continue;
    }
    pos += ent_len;
  }
  return false;
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

static size_t scan_root_prefix_len_ci(const wchar_t *root) {
  size_t n = wcslen(root);
  while (n > 3 && root[n - 1] == L'\\') {
    n--;
  }
  return n;
}

static bool path_is_under_scan_root_ci(const wchar_t *full, const wchar_t *root) {
  size_t lr = scan_root_prefix_len_ci(root);
  for (size_t i = 0; i < lr; i++) {
    if (full[i] == L'\0' || towlower((wint_t)full[i]) != towlower((wint_t)root[i])) {
      return false;
    }
  }
  if (full[lr] == L'\0') {
    return true;
  }
  /* Drive root like "E:\\" already ends with '\\' — any longer path "E:\\foo" is under it. */
  if (lr > 0 && root[lr - 1] == L'\\') {
    return true;
  }
  /* "E:\\Media" without trailing slash — next segment must start with '\\'. */
  return full[lr] == L'\\';
}

static bool path_equals_scan_root_ci(const wchar_t *full, const wchar_t *root) {
  size_t lr = scan_root_prefix_len_ci(root);
  for (size_t i = 0; i < lr; i++) {
    if (towlower((wint_t)full[i]) != towlower((wint_t)root[i])) {
      return false;
    }
  }
  return full[lr] == L'\0';
}

static uint32_t depth_below_scan_root(const wchar_t *full, const wchar_t *root_norm) {
  size_t lr = scan_root_prefix_len_ci(root_norm);
  for (size_t i = 0; i < lr; i++) {
    if (full[i] == L'\0' || towlower((wint_t)full[i]) != towlower((wint_t)root_norm[i])) {
      return UINT32_MAX;
    }
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

/** True if any path component of rel (backslash-separated) equals want, case-insensitive. */
static bool rel_has_component_ci(const wchar_t *rel, const wchar_t *want) {
  size_t wlen = wcslen(want);
  if (wlen == 0) {
    return false;
  }
  for (const wchar_t *p = rel; *p != L'\0';) {
    const wchar_t *slash = wcschr(p, L'\\');
    size_t seglen = slash ? (size_t)(slash - p) : wcslen(p);
    if (seglen == wlen && _wcsnicmp(p, want, wlen) == 0) {
      return true;
    }
    if (!slash) {
      break;
    }
    p = slash + 1;
  }
  return false;
}

/**
 * Skip NTFS reserved metadata ($Bitmap, $MFT, $Extend\\..., etc.), Windows volume system paths,
 * and anything under "System Volume Information" (VSS metadata). Keeps $Recycle.Bin and user $… folders.
 */
static bool ntfs_emit_skip_system_metadata_ci(const wchar_t *full, const wchar_t *vol_root) {
  size_t vrl = wcslen(vol_root);
  size_t i = 0;
  for (; i < vrl && full[i] && vol_root[i]; i++) {
    if (towlower((wint_t)full[i]) != towlower((wint_t)vol_root[i])) {
      return false;
    }
  }
  if (vol_root[i] != L'\0') {
    return false;
  }
  const wchar_t *rel = full + vrl;
  if (*rel == L'\\') {
    rel++;
  }
  if (*rel == L'\0') {
    return false;
  }
  if (_wcsnicmp(rel, L"$Extend\\", 9) == 0 || _wcsicmp(rel, L"$Extend") == 0) {
    return true;
  }

  if (rel_has_component_ci(rel, L"System Volume Information")) {
    return true;
  }

  wchar_t seg[280];
  size_t si = 0;
  while (rel[si] != L'\0' && rel[si] != L'\\' && si + 1u < 280u) {
    seg[si] = rel[si];
    si++;
  }
  seg[si] = L'\0';

  if (_wcsicmp(seg, L"$Recycle.Bin") == 0) {
    return false;
  }

  static const wchar_t *const k_ntfs_meta_root[] = {
      L"$AttrDef", L"$BadClus", L"$Bitmap", L"$Boot", L"$LogFile", L"$MFT",
      L"$MFTMirr", L"$Secure",    L"$UpCase", L"$Volume", L"$Extend",
      NULL,
  };
  for (size_t k = 0; k_ntfs_meta_root[k] != NULL; k++) {
    if (_wcsicmp(seg, k_ntfs_meta_root[k]) == 0) {
      return true;
    }
  }
  return false;
}

static uint64_t ntfs_volume_root_mft_index(const wchar_t *vol_root_path) {
  HANDLE h =
      CreateFileW(vol_root_path, FILE_READ_ATTRIBUTES,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                  FILE_FLAG_BACKUP_SEMANTICS | FILE_ATTRIBUTE_DIRECTORY, NULL);
  if (h == INVALID_HANDLE_VALUE) {
    return 5ull;
  }
  da_file_id_info_t fid;
  memset(&fid, 0, sizeof(fid));
  uint64_t idx = 5ull;
  if (GetFileInformationByHandleEx(h, FileIdInfo, &fid, sizeof(fid))) {
    idx = read_u64_le(fid.FileId.Identifier) & MFT_REF_MASK;
    if (idx == 0) {
      idx = 5ull;
    }
  }
  CloseHandle(h);
  return idx;
}

static wchar_t *path_for_index(uint64_t idx, wchar_t **cache, wchar_t **names,
                               uint64_t *parents, unsigned char *valid, size_t record_count,
                               const wchar_t *vol_root, uint64_t root_mft_idx, unsigned depth) {
  if (idx >= record_count || depth > 128u) {
    return NULL;
  }
  if (cache[idx]) {
    return cache[idx];
  }
  if (idx == root_mft_idx) {
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
    return NULL;
  }

  wchar_t *pp = path_for_index(par, cache, names, parents, valid, record_count, vol_root,
                               root_mft_idx, depth + 1u);
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

static DWORD WINAPI mft_parse_worker_main(LPVOID param) {
  mft_parse_shared_t *s = (mft_parse_shared_t *)param;
  unsigned char *recbuf = (unsigned char *)malloc((size_t)s->record_size);
  unsigned char *extbuf = (unsigned char *)malloc((size_t)s->record_size);
  if (recbuf == NULL || extbuf == NULL) {
    free(recbuf);
    free(extbuf);
    return 1;
  }

  for (;;) {
    if (atomic_load_explicit(&s->r->cancel, memory_order_relaxed) != 0) {
      break;
    }
    size_t idx = atomic_fetch_add_explicit(s->next_idx, 1, memory_order_relaxed);
    if (idx >= s->record_count) {
      break;
    }
    atomic_fetch_add_explicit(&s->r->entry_visits, 1, memory_order_relaxed);

    uint64_t off = (uint64_t)idx * (uint64_t)s->record_size;
    if (s->use_mmap) {
      mft_mmap_read(s->mmap, off, recbuf, (size_t)s->record_size);
    } else if (!mft_stream_read(s->vol, s->cluster_bytes, s->runs, s->stream_base_vcn, off, recbuf,
                                (size_t)s->record_size)) {
      continue;
    }
    if (!ntfs_fixup_record(recbuf, (size_t)s->record_size, s->bps)) {
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
    memset(&fn, 0, sizeof(fn));
    if (!parse_resident_attrs(recbuf, (size_t)s->record_size, &fn, &mtime_ns, &dattr, &has_si)) {
      if (!parse_fn_via_attribute_list(recbuf, (size_t)s->record_size, s, idx, &fn, &mtime_ns,
                                       &dattr, &has_si, extbuf)) {
        continue;
      }
    }

    s->valid[idx] = 1;
    s->parents[idx] = fn.parent_id;
    s->sizes[idx] = fn.real_size;
    s->mtimes[idx] = mtime_ns;
    s->dosattrs[idx] = dattr;
    s->is_dir[idx] = (unsigned char)((flags & FILE_RECORD_IS_DIRECTORY) != 0 ? 1 : 0);
    if (fn.name_chars > 0) {
      wchar_t *nm = wcs_dup_range(fn.name_buf, fn.name_chars);
      if (nm != NULL) {
        free(s->names[idx]);
        s->names[idx] = nm;
      }
    }
    if (!has_si) {
      s->dosattrs[idx] = s->is_dir[idx] ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE;
    }
  }

  free(extbuf);
  free(recbuf);
  return 0;
}

/* FIXME(ntfs-mft): Incomplete; keep in sync with scan_controller DISKATLAS_APP_ENABLE_NTFS_MFT. */
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

  uint64_t root_mft_idx = ntfs_volume_root_mft_index(vol_path_full);

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

  uint64_t mft_valid_len_ioctl = 0;
  NTFS_VOLUME_DATA_BUFFER vol_data;
  DWORD vol_data_br = 0;
  if (DeviceIoControl(vol, FSCTL_GET_NTFS_VOLUME_DATA, NULL, 0, &vol_data, sizeof(vol_data),
                       &vol_data_br, NULL) &&
      vol_data_br >= sizeof(NTFS_VOLUME_DATA_BUFFER)) {
    if (vol_data.BytesPerSector >= 512u && vol_data.BytesPerSector <= 4096u &&
        (vol_data.BytesPerSector % 512u) == 0u) {
      bps = (uint16_t)vol_data.BytesPerSector;
    }
    if (vol_data.BytesPerCluster > 0u) {
      cluster_bytes = (uint64_t)vol_data.BytesPerCluster;
    }
    if (vol_data.BytesPerFileRecordSegment >= 512u &&
        vol_data.BytesPerFileRecordSegment <= (1024u * 1024u)) {
      record_size = vol_data.BytesPerFileRecordSegment;
    }
    mft_vol_off = (uint64_t)vol_data.MftStartLcn.QuadPart * cluster_bytes;
    mft_valid_len_ioctl = (uint64_t)vol_data.MftValidDataLength.QuadPart;
  }

  ntfs_run_list_t runs = {0};
  uint64_t stream_total = 0;
  uint64_t stream_base_vcn = 0;

  if (try_load_mft_runs_via_special_file(vol_path_full[0], &runs, &stream_total)) {
    if (mft_valid_len_ioctl > stream_total) {
      stream_total = mft_valid_len_ioctl;
    }
    stream_base_vcn = 0;
  } else {
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
    const unsigned char *adata =
        find_best_unnamed_nonresident_data(rec0, record_size, &run_rel, &attr_len);
    if (!adata || run_rel >= attr_len) {
      free(rec0);
      CloseHandle(vol);
      return false;
    }

    int64_t lowest_vcn = read_i64_le(adata + 0x10);
    int64_t highest_vcn = read_i64_le(adata + 0x18);
    uint64_t attr_allocated_size = read_u64_le(adata + 0x28);
    uint64_t attr_real_size = read_u64_le(adata + 0x30);
    uint64_t attr_initialized_size = read_u64_le(adata + 0x38);
    (void)highest_vcn;

    const unsigned char *runs_buf = adata + run_rel;
    size_t runs_max = attr_len - run_rel;
    if (!ntfs_decode_run_list(runs_buf, runs_max, lowest_vcn, highest_vcn, &runs)) {
      free(rec0);
      run_list_free(&runs);
      CloseHandle(vol);
      return false;
    }
    free(rec0);

    uint64_t run_stream_bytes = ntfs_run_list_stream_total_bytes(&runs, cluster_bytes);
    stream_total = attr_real_size;
    if (attr_initialized_size > stream_total) {
      stream_total = attr_initialized_size;
    }
    if (attr_allocated_size > stream_total) {
      stream_total = attr_allocated_size;
    }
    if (run_stream_bytes > stream_total) {
      stream_total = run_stream_bytes;
    }
    if (mft_valid_len_ioctl > stream_total) {
      stream_total = mft_valid_len_ioctl;
    }

    if (lowest_vcn >= 0) {
      stream_base_vcn = (uint64_t)lowest_vcn;
    }
  }

  if (stream_total == 0 || record_size == 0 ||
      stream_total / (uint64_t)record_size > (uint64_t)SIZE_MAX / 8u) {
    run_list_free(&runs);
    CloseHandle(vol);
    return false;
  }

  size_t record_count = (size_t)(stream_total / (uint64_t)record_size);
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

  mft_stream_mmap_t mft_mmap;
  memset(&mft_mmap, 0, sizeof(mft_mmap));
  int mmap_ok = mft_mmap_try_build(&mft_mmap, vol, cluster_bytes, &runs, stream_total) ? 1 : 0;

  atomic_size_t next_idx;
  atomic_init(&next_idx, 0);

  mft_parse_shared_t psh;
  memset(&psh, 0, sizeof(psh));
  psh.r = r;
  psh.next_idx = &next_idx;
  psh.record_count = record_count;
  psh.record_size = record_size;
  psh.bps = bps;
  psh.cluster_bytes = cluster_bytes;
  psh.stream_base_vcn = stream_base_vcn;
  psh.vol = vol;
  psh.runs = &runs;
  psh.mmap = &mft_mmap;
  psh.use_mmap = mmap_ok;
  psh.parents = parents;
  psh.names = names;
  psh.valid = valid;
  psh.is_dir = is_dir;
  psh.sizes = sizes;
  psh.mtimes = mtimes;
  psh.dosattrs = dosattrs;

  unsigned tc = (unsigned)opts->io_threads;
  if (tc == 0) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    tc = (unsigned)si.dwNumberOfProcessors;
    if (tc > 16u) {
      tc = 16u;
    }
    if (tc < 1u) {
      tc = 1u;
    }
  }
  if (record_count > 0u && tc > (unsigned)record_count) {
    tc = (unsigned)record_count;
  }
  if (record_count < 512u && opts->io_threads == 0) {
    tc = 1u;
  }
  if (tc < 1u) {
    tc = 1u;
  }

  HANDLE *th = (HANDLE *)calloc(tc, sizeof(HANDLE));
  if (th == NULL) {
    mft_mmap_destroy(&mft_mmap);
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
  for (unsigned ti = 0; ti < tc; ti++) {
    th[ti] = CreateThread(NULL, 0, mft_parse_worker_main, &psh, 0, NULL);
    if (th[ti] == NULL) {
      atomic_store_explicit(&r->cancel, 1, memory_order_release);
      for (unsigned tj = 0; tj < ti; tj++) {
        (void)WaitForSingleObject(th[tj], INFINITE);
        CloseHandle(th[tj]);
      }
      free(th);
      mft_mmap_destroy(&mft_mmap);
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
  }
  (void)WaitForMultipleObjects((DWORD)tc, th, TRUE, INFINITE);
  for (unsigned ti = 0; ti < tc; ti++) {
    CloseHandle(th[ti]);
  }
  free(th);
  mft_mmap_destroy(&mft_mmap);

  const uint32_t max_depth_req = opts->max_depth;
  const bool include_hidden = (opts->flags & DISKATLAS_SCAN_OPTION_INCLUDE_HIDDEN) != 0;

  for (size_t idx = 0; idx < record_count; idx++) {
    if (atomic_load_explicit(&r->cancel, memory_order_relaxed) != 0) {
      break;
    }
    if (!valid[idx]) {
      continue;
    }
    wchar_t *full = path_for_index((uint64_t)idx, path_cache, names, parents, valid, record_count,
                                   vol_path_full, root_mft_idx, 0);
    if (!full) {
      continue;
    }
    if (ntfs_emit_skip_system_metadata_ci(full, vol_path_full)) {
      continue;
    }
    if (path_equals_scan_root_ci(full, root_path_wide) ||
        path_equals_scan_root_ci(full, vol_path_full)) {
      continue;
    }
    if (!path_is_under_scan_root_ci(full, root_path_wide)) {
      continue;
    }
    if (max_depth_req != 0) {
      uint32_t dep = depth_below_scan_root(full, root_path_wide);
      if (dep == UINT32_MAX || dep > max_depth_req) {
        continue;
      }
    }
    if (!include_hidden) {
      uint32_t a = dosattrs[idx];
      DWORD skip_mask = FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM;
      if (is_dir[idx] != 0) {
        skip_mask = FILE_ATTRIBUTE_HIDDEN;
      }
      if ((a & skip_mask) != 0) {
        continue;
      }
    }

    bool dir_f = is_dir[idx] != 0;
    if (!diskatlas_win32_record_entry_metadata(r, full, sizes[idx], mtimes[idx], dosattrs[idx],
                                               dir_f)) {
      /* OOM or path UTF-8 conversion failure — skip this entry; keep scanning others. */
      continue;
    }
  }

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
