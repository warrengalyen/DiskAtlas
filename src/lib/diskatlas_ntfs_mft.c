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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <wchar.h>
#include <wctype.h>

#include <stddef.h>

#include "diskatlas_internal.h"
#include "diskatlas_ntfs_mft_parse.h"

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
  unsigned char *p = (unsigned char *)buf;
  DWORD left = len;
  uint64_t cur_off = off;
  while (left > 0) {
    DWORD chunk = left > (64u * 1024u) ? (64u * 1024u) : left;
    DWORD got = 0;
    /* Use OVERLAPPED to embed the byte offset, making this call thread-safe.
     * On synchronous handles Windows reads from Offset/OffsetHigh directly,
     * so there is no shared file-pointer state between concurrent threads. */
    OVERLAPPED ov;
    memset(&ov, 0, sizeof(ov));
    ov.Offset     = (DWORD)(cur_off & 0xFFFFFFFFu);
    ov.OffsetHigh = (DWORD)(cur_off >> 32);
    if (!ReadFile(vol, p, chunk, &got, &ov) || got == 0) {
      return false;
    }
    p += got;
    cur_off += (uint64_t)got;
    left -= got;
  }
  return true;
}

/** Like vol_read_at but uses 4 MB chunks — suitable for large sequential reads. */
static bool vol_read_large(HANDLE vol, uint64_t off, void *buf, uint64_t len) {
  unsigned char *p = (unsigned char *)buf;
  uint64_t cur_off = off;
  while (len > 0) {
    DWORD chunk = (DWORD)(len > (4ULL * 1024 * 1024) ? (4ULL * 1024 * 1024) : len);
    DWORD got = 0;
    OVERLAPPED ov;
    memset(&ov, 0, sizeof(ov));
    ov.Offset     = (DWORD)(cur_off & 0xFFFFFFFFu);
    ov.OffsetHigh = (DWORD)(cur_off >> 32);
    if (!ReadFile(vol, p, chunk, &got, &ov) || got == 0) {
      return false;
    }
    p += got;
    cur_off += (uint64_t)got;
    len -= (uint64_t)got;
  }
  return true;
}

/**
 * Read the entire MFT into one contiguous heap buffer — a single sequential pass
 * through all runs.  Returns NULL if the MFT is too large (> 1 GiB) or malloc fails.
 * Workers can then index directly into this buffer with zero system-call overhead.
 */
static unsigned char *bulk_read_mft(HANDLE vol, const ntfs_run_list_t *runs,
                                    uint64_t cluster_bytes, size_t total_bytes) {
  if (total_bytes > 1024ULL * 1024 * 1024) {
    return NULL;
  }
  unsigned char *buf = (unsigned char *)malloc(total_bytes);
  if (!buf) {
    return NULL;
  }
  uint64_t dst_off = 0;
  for (size_t ri = 0; ri < runs->len; ri++) {
    uint64_t run_bytes = (runs->segs[ri].vcn_hi_excl - runs->segs[ri].vcn_lo) * cluster_bytes;
    if (dst_off + run_bytes > (uint64_t)total_bytes) {
      run_bytes = (uint64_t)total_bytes - dst_off;
    }
    if (run_bytes == 0) {
      break;
    }
    if (runs->segs[ri].lcn_start < 0) {
      memset(buf + dst_off, 0, (size_t)run_bytes);
    } else {
      uint64_t vol_off = (uint64_t)runs->segs[ri].lcn_start * cluster_bytes;
      if (!vol_read_large(vol, vol_off, buf + dst_off, run_bytes)) {
        free(buf);
        return NULL;
      }
    }
    dst_off += run_bytes;
    if (dst_off >= (uint64_t)total_bytes) {
      break;
    }
  }
  return buf;
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

typedef da_ntfs_fn_pick_t fn_pick_t;

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
  unsigned char *mft_buf; /* non-NULL = entire MFT in one heap buffer; workers memcpy from here */
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
  if (s->mft_buf) {
    memcpy(buf, s->mft_buf + off, (size_t)s->record_size);
  } else if (s->use_mmap) {
    mft_mmap_read(s->mmap, off, buf, (size_t)s->record_size);
  } else if (!mft_stream_read(s->vol, s->cluster_bytes, s->runs, s->stream_base_vcn, off, buf,
                              (size_t)s->record_size)) {
    return false;
  }
  if (!da_ntfs_fixup_record(buf, (size_t)s->record_size, s->bps)) {
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
 * When $DATA lives in an extension record (base record has $ATTRIBUTE_LIST only), the
 * non-resident $DATA header — which carries the authoritative real_size — is not visible
 * in the base record.  Walk the $ATTRIBUTE_LIST for unnamed $DATA entries, load the first
 * extension record that has a non-resident $DATA attribute, and return its real_size.
 */
static uint64_t find_data_size_via_attribute_list(const unsigned char *base_rec, size_t base_len,
                                                   mft_parse_shared_t *s, unsigned char *ext_buf) {
  const unsigned char *alist = NULL;
  uint32_t alist_len = 0;
  if (!resident_attr_value_unnamed(base_rec, base_len, ATTR_ATTRIBUTE_LIST, &alist, &alist_len)) {
    return 0;
  }
  size_t pos = 0;
  while (pos + ATTR_LIST_ENTRY_MIN <= alist_len) {
    uint32_t ent_type = read_u32_le(alist + pos);
    uint16_t ent_len  = read_u16_le(alist + pos + 4);
    if (ent_len < ATTR_LIST_ENTRY_MIN || pos + ent_len > alist_len) {
      break;
    }
    if (ent_type == ATTR_DATA && alist[pos + 6] == 0) { /* unnamed $DATA */
      uint64_t ref = read_u64_le(alist + pos + 16) & MFT_REF_MASK;
      if (ref != 0 && ref < s->record_count) {
        if (mft_load_record_raw(s, (size_t)ref, ext_buf)) {
          uint16_t aoff = read_u16_le(ext_buf + 0x14);
          uint32_t bused = read_u32_le(ext_buf + 0x18);
          size_t rlen = (size_t)s->record_size;
          size_t off = aoff;
          while (off + 8 <= rlen && off + 8 <= bused) {
            uint32_t t = read_u32_le(ext_buf + off);
            uint32_t a = read_u32_le(ext_buf + off + 4);
            if (a < 24 || off + a > rlen || t == 0xFFFFFFFFu) { break; }
            /* Non-resident unnamed $DATA: real_size at attr+0x30 */
            if (t == ATTR_DATA && ext_buf[off + 9] == 0 &&
                ext_buf[off + 8] != 0 && a >= 0x38u) {
              uint64_t sz = read_u64_le(ext_buf + off + 0x30);
              if (sz > 0) { return sz; }
            }
            off += a;
          }
        }
      }
    }
    pos += ent_len;
  }
  return 0;
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
      uint64_t dsz_try = 0;
      if (!da_ntfs_parse_resident_attrs(ext_buf, (size_t)s->record_size, &fn_try, &mt, &da, &hs, &dsz_try)) {
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

/** TRUE when @a rel (path after volume root) is under $Recycle.Bin or basename is $I/$R recycle internal name. */
static bool ntfs_path_rel_is_recycle_internal_ci(const wchar_t *rel) {
  if (rel == NULL || rel[0] == L'\0') {
    return false;
  }
  if (_wcsnicmp(rel, L"$Recycle.Bin\\", 14) == 0 || _wcsicmp(rel, L"$Recycle.Bin") == 0) {
    return true;
  }
  const wchar_t *base = rel;
  for (const wchar_t *p = rel; *p != L'\0'; p++) {
    if (*p == L'\\') {
      base = p + 1;
    }
  }
  if (base[0] == L'$' && (base[1] == L'I' || base[1] == L'R')) {
    return true;
  }
  return false;
}

static uint32_t ntfs_augment_dosattrs_for_emit(const wchar_t *full, const wchar_t *vol_root,
                                                uint32_t dosattrs, bool is_dir) {
  size_t vrl = wcslen(vol_root);
  const wchar_t *rel = full;
  if (vrl > 0 && _wcsnicmp(full, vol_root, vrl) == 0) {
    rel = full + vrl;
    if (*rel == L'\\') {
      rel++;
    }
  }
  if (!ntfs_path_rel_is_recycle_internal_ci(rel)) {
    return dosattrs;
  }
  dosattrs |= FILE_ATTRIBUTE_HIDDEN;
  if (!is_dir) {
    dosattrs |= FILE_ATTRIBUTE_SYSTEM;
  }
  return dosattrs;
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

/** Sentinel value for path_offs[] entries that have not been resolved yet. */
#define PATH_OFFS_NONE ((size_t)(-1))

/**
 * Bump-allocator for wide-char path strings.  All paths are packed into one
 * contiguous buffer; offsets remain valid across realloc because we store
 * indices, not raw pointers.
 */
typedef struct wchar_arena {
  wchar_t *buf;
  size_t   len; /* used, in wchar_t units */
  size_t   cap; /* allocated, in wchar_t units */
} wchar_arena_t;

static size_t wchar_arena_alloc(wchar_arena_t *a, size_t n) {
  if (n == 0) {
    n = 1;
  }
  if (a->len + n < a->len) { /* overflow */
    return PATH_OFFS_NONE;
  }
  if (a->len + n > a->cap) {
    size_t nc = a->cap ? a->cap : (512u * 1024u);
    while (nc < a->len + n) {
      nc *= 2u;
    }
    wchar_t *nb = (wchar_t *)realloc(a->buf, nc * sizeof(wchar_t));
    if (!nb) {
      return PATH_OFFS_NONE;
    }
    a->buf = nb;
    a->cap = nc;
  }
  size_t off = a->len;
  a->len += n;
  return off;
}

/**
 * Recursively resolve the full path for MFT record `idx`, storing the result
 * as a wchar_t string in the arena and caching its offset in path_offs[].
 * Returns PATH_OFFS_NONE on failure.  All previously cached offsets remain
 * valid even when the arena is grown (because they are offsets, not pointers).
 */
static size_t path_for_index(uint64_t idx, size_t *path_offs, wchar_arena_t *arena,
                              wchar_t **names, uint64_t *parents, unsigned char *valid,
                              size_t record_count, const wchar_t *vol_root,
                              uint64_t root_mft_idx, unsigned depth) {
  if (idx >= record_count || depth > 128u) {
    return PATH_OFFS_NONE;
  }
  if (path_offs[idx] != PATH_OFFS_NONE) {
    return path_offs[idx];
  }

  if (idx == root_mft_idx) {
    size_t n = wcslen(vol_root) + 1u;
    size_t off = wchar_arena_alloc(arena, n);
    if (off == PATH_OFFS_NONE) {
      return PATH_OFFS_NONE;
    }
    memcpy(arena->buf + off, vol_root, n * sizeof(wchar_t));
    path_offs[idx] = off;
    return off;
  }

  if (!valid[idx] || !names[idx]) {
    return PATH_OFFS_NONE;
  }
  uint64_t par = parents[idx];
  if (par == (uint64_t)idx) {
    return PATH_OFFS_NONE;
  }

  size_t par_off = path_for_index(par, path_offs, arena, names, parents, valid,
                                   record_count, vol_root, root_mft_idx, depth + 1u);
  if (par_off == PATH_OFFS_NONE) {
    return PATH_OFFS_NONE;
  }

  /* Measure before allocating (arena->buf may move on realloc). */
  size_t pl       = wcslen(arena->buf + par_off);
  size_t nl       = wcslen(names[idx]);
  bool   need_sep = (pl == 0 || (arena->buf[par_off + pl - 1] != L'\\' &&
                                  arena->buf[par_off + pl - 1] != L'/'));
  size_t total    = pl + (need_sep ? 1u : 0u) + nl + 1u;

  size_t off = wchar_arena_alloc(arena, total);
  if (off == PATH_OFFS_NONE) {
    return PATH_OFFS_NONE;
  }

  /* Re-derive parent pointer after potential realloc. */
  wchar_t *dst = arena->buf + off;
  memcpy(dst, arena->buf + par_off, pl * sizeof(wchar_t));
  size_t pos = pl;
  if (need_sep) {
    dst[pos++] = L'\\';
  }
  memcpy(dst + pos, names[idx], (nl + 1u) * sizeof(wchar_t));

  path_offs[idx] = off;
  return off;
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
    if (s->mft_buf) {
      memcpy(recbuf, s->mft_buf + off, (size_t)s->record_size);
    } else if (s->use_mmap) {
      mft_mmap_read(s->mmap, off, recbuf, (size_t)s->record_size);
    } else if (!mft_stream_read(s->vol, s->cluster_bytes, s->runs, s->stream_base_vcn, off, recbuf,
                                (size_t)s->record_size)) {
      continue;
    }
    if (!da_ntfs_fixup_record(recbuf, (size_t)s->record_size, s->bps)) {
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
    uint64_t data_size = 0;
    memset(&fn, 0, sizeof(fn));
    if (!da_ntfs_parse_resident_attrs(recbuf, (size_t)s->record_size, &fn, &mtime_ns, &dattr, &has_si,
                              &data_size)) {
      if (!parse_fn_via_attribute_list(recbuf, (size_t)s->record_size, s, idx, &fn, &mtime_ns,
                                       &dattr, &has_si, extbuf)) {
        continue;
      }
      /* Re-scan base record for $DATA size (parse_fn_via_attribute_list only yields $FILE_NAME). */
      uint64_t base_data_size = 0;
      fn_pick_t fn_dummy; memset(&fn_dummy, 0, sizeof(fn_dummy));
      uint64_t mt_dummy = 0; uint32_t da_dummy = 0; int si_dummy = 0;
      da_ntfs_parse_resident_attrs(recbuf, (size_t)s->record_size, &fn_dummy, &mt_dummy, &da_dummy,
                           &si_dummy, &base_data_size);
      if (base_data_size > 0) data_size = base_data_size;
    }

    /* If $DATA size is still 0, look it up through $ATTRIBUTE_LIST (extension records). */
    if (data_size == 0 && (flags & FILE_RECORD_IS_DIRECTORY) == 0) {
      data_size = find_data_size_via_attribute_list(recbuf, (size_t)s->record_size, s, extbuf);
    }

    s->valid[idx] = 1;
    s->parents[idx] = fn.parent_id;
    /* Prefer $DATA attribute size (authoritative); fall back to $FILE_NAME real_size. */
    s->sizes[idx] = data_size > 0 ? data_size : fn.real_size;
    s->mtimes[idx] = mtime_ns;
    s->dosattrs[idx] = dattr;
    s->is_dir[idx] = (unsigned char)((flags & FILE_RECORD_IS_DIRECTORY) != 0 ? 1 : 0);
    /* Update live progress counters so the UI label reflects MFT worker activity. */
    if (s->is_dir[idx]) {
      atomic_fetch_add_explicit(&s->r->folders_recorded, 1, memory_order_relaxed);
    } else {
      atomic_fetch_add_explicit(&s->r->files_recorded, 1, memory_order_relaxed);
    }
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

static void mft_vol_close_if_open(HANDLE *volp) {
  if (volp != NULL && *volp != NULL && *volp != INVALID_HANDLE_VALUE) {
    CloseHandle(*volp);
    *volp = INVALID_HANDLE_VALUE;
  }
}

/**
 * Shared MFT parse + emit: @a mft_buf_pre non-NULL = caller-owned buffer transferred here (freed after workers).
 * If NULL, reads MFT via bulk_read / mmap from @a *vol_io (must be open when NULL).
 */
static bool mft_parse_emit_mft_common(
    diskatlas_scan_result_t *r,
    unsigned char *mft_buf_pre,
    HANDLE *vol_io,
    ntfs_run_list_t *runs,
    uint64_t stream_total,
    uint32_t record_size,
    uint16_t bps,
    uint64_t cluster_bytes,
    uint64_t stream_base_vcn,
    wchar_t *vol_path_full,
    wchar_t *root_path_wide,
    uint64_t root_mft_idx,
    const scan_options_t *opts) {
  size_t record_count = (size_t)(stream_total / (uint64_t)record_size);
  uint64_t *parents = (uint64_t *)calloc(record_count, sizeof(uint64_t));
  wchar_t **names = (wchar_t **)calloc(record_count, sizeof(wchar_t *));
  unsigned char *valid = (unsigned char *)calloc(record_count, 1);
  unsigned char *is_dir = (unsigned char *)calloc(record_count, 1);
  uint64_t *sizes = (uint64_t *)calloc(record_count, sizeof(uint64_t));
  uint64_t *mtimes = (uint64_t *)calloc(record_count, sizeof(uint64_t));
  uint32_t *dosattrs = (uint32_t *)calloc(record_count, sizeof(uint32_t));
  size_t *path_offs = (size_t *)malloc(record_count * sizeof(size_t));

  if (!parents || !names || !valid || !is_dir || !sizes || !mtimes || !dosattrs || !path_offs) {
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
    free(path_offs);
    run_list_free(runs);
    if (mft_buf_pre != NULL) {
      free(mft_buf_pre);
    }
    mft_vol_close_if_open(vol_io);
    return false;
  }
  memset(path_offs, 0xFF, record_count * sizeof(size_t));

  unsigned char *mft_buf = mft_buf_pre;
  mft_stream_mmap_t mft_mmap;
  memset(&mft_mmap, 0, sizeof(mft_mmap));
  int mmap_ok = 0;
  if (mft_buf == NULL) {
    HANDLE volh = (vol_io != NULL) ? *vol_io : INVALID_HANDLE_VALUE;
    mft_buf = bulk_read_mft(volh, runs, cluster_bytes, (size_t)stream_total);
    if (!mft_buf) {
      mmap_ok = mft_mmap_try_build(&mft_mmap, volh, cluster_bytes, runs, stream_total) ? 1 : 0;
    }
  }

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
  psh.vol = (vol_io != NULL && *vol_io != INVALID_HANDLE_VALUE) ? *vol_io : INVALID_HANDLE_VALUE;
  psh.runs = runs;
  psh.mmap = &mft_mmap;
  psh.use_mmap = mmap_ok;
  psh.mft_buf = mft_buf;
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
    free(mft_buf);
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
    free(path_offs);
    run_list_free(runs);
    mft_vol_close_if_open(vol_io);
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
      free(mft_buf);
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
      free(path_offs);
      run_list_free(runs);
      mft_vol_close_if_open(vol_io);
      return false;
    }
  }
  (void)WaitForMultipleObjects((DWORD)tc, th, TRUE, INFINITE);
  for (unsigned ti = 0; ti < tc; ti++) {
    CloseHandle(th[ti]);
  }
  free(th);

  free(mft_buf);
  mft_mmap_destroy(&mft_mmap);

  wchar_arena_t path_arena;
  memset(&path_arena, 0, sizeof(path_arena));

  atomic_store_explicit(&r->folders_recorded, 0, memory_order_relaxed);
  atomic_store_explicit(&r->files_recorded, 0, memory_order_relaxed);

  const uint32_t max_depth_req = opts->max_depth;
  const bool include_hidden = (opts->flags & DISKATLAS_SCAN_OPTION_INCLUDE_HIDDEN) != 0;

  for (size_t idx = 0; idx < record_count; idx++) {
    if (!valid[idx]) {
      continue;
    }
    size_t path_off = path_for_index((uint64_t)idx, path_offs, &path_arena, names, parents, valid,
                                     record_count, vol_path_full, root_mft_idx, 0);
    if (path_off == PATH_OFFS_NONE) {
      continue;
    }
    const wchar_t *full = path_arena.buf + path_off;
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
    uint32_t emit_attrs =
        ntfs_augment_dosattrs_for_emit(full, vol_path_full, dosattrs[idx], dir_f);
    if (!diskatlas_win32_record_entry_metadata(r, full, sizes[idx], mtimes[idx], emit_attrs,
                                               dir_f)) {
      continue;
    }
  }

  free(parents);
  for (size_t i = 0; i < record_count; i++) {
    free(names[i]);
  }
  free(names);
  free(path_offs);
  free(path_arena.buf);
  free(valid);
  free(is_dir);
  free(sizes);
  free(mtimes);
  free(dosattrs);
  run_list_free(runs);
  mft_vol_close_if_open(vol_io);

  return true;
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
    if (!da_ntfs_fixup_record(rec0, record_size, bps)) {
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

  if (!mft_parse_emit_mft_common(r, NULL, &vol, &runs, stream_total, record_size, bps, cluster_bytes,
                                 stream_base_vcn, vol_path_full, root_path_wide, root_mft_idx, opts)) {
    return false;
  }
  return true;
}

static int mft_dump_stream_runs_to_dst(HANDLE vol, HANDLE dst, const ntfs_run_list_t *runs, uint64_t cluster_bytes,
                                       uint64_t total_bytes, char *errbuf, size_t errlen,
                                       void (*on_progress)(void *user, int pct, uint64_t done, uint64_t total),
                                       void *user) {
  enum { k_buf = 1 << 20 };
  unsigned char *buf = (unsigned char *)malloc(k_buf);
  if (buf == NULL) {
    if (errbuf != NULL && errlen > 0) {
      (void)snprintf(errbuf, errlen, "out of memory");
    }
    return -1;
  }
  uint64_t dst_off = 0;
  for (size_t ri = 0; ri < runs->len; ri++) {
    uint64_t run_bytes = (runs->segs[ri].vcn_hi_excl - runs->segs[ri].vcn_lo) * cluster_bytes;
    if (dst_off + run_bytes > total_bytes) {
      run_bytes = total_bytes - dst_off;
    }
    if (run_bytes == 0) {
      break;
    }
    uint64_t pos = 0;
    while (pos < run_bytes) {
      uint64_t n64 = run_bytes - pos;
      if (n64 > (uint64_t)k_buf) {
        n64 = (uint64_t)k_buf;
      }
      if (runs->segs[ri].lcn_start < 0) {
        memset(buf, 0, (size_t)n64);
      } else {
        uint64_t vol_off = (uint64_t)runs->segs[ri].lcn_start * cluster_bytes + pos;
        if (!vol_read_large(vol, vol_off, buf, n64)) {
          if (errbuf != NULL && errlen > 0) {
            (void)snprintf(errbuf, errlen, "volume read failed (error %lu)", (unsigned long)GetLastError());
          }
          free(buf);
          return -1;
        }
      }
      DWORD n = (DWORD)n64;
      DWORD wrote = 0;
      if (!WriteFile(dst, buf, n, &wrote, NULL) || wrote != n) {
        if (errbuf != NULL && errlen > 0) {
          (void)snprintf(errbuf, errlen, "write failed (error %lu)", (unsigned long)GetLastError());
        }
        free(buf);
        return -1;
      }
      dst_off += (uint64_t)n64;
      pos += n64;
      if (on_progress != NULL && total_bytes > 0) {
        int pct = (int)((dst_off * 100ull) / total_bytes);
        if (pct > 100) {
          pct = 100;
        }
        on_progress(user, pct, dst_off, total_bytes);
      }
      if (dst_off >= total_bytes) {
        break;
      }
    }
    if (dst_off >= total_bytes) {
      break;
    }
  }
  free(buf);
  return 0;
}

static int mft_dump_via_volume_fallback(wchar_t dl, const wchar_t *destw, char *errbuf, size_t errlen,
                                        void (*on_progress)(void *user, int pct, uint64_t done, uint64_t total),
                                        void *user) {
  wchar_t dev[16];
  dev[0] = L'\\';
  dev[1] = L'\\';
  dev[2] = L'.';
  dev[3] = L'\\';
  dev[4] = dl;
  dev[5] = L':';
  dev[6] = L'\0';

  HANDLE vol = CreateFileW(dev, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
  if (vol == INVALID_HANDLE_VALUE) {
    if (errbuf != NULL && errlen > 0) {
      (void)snprintf(errbuf, errlen, "cannot open volume for MFT dump (error %lu)",
                     (unsigned long)GetLastError());
    }
    return -1;
  }

  unsigned char boot[4096];
  DWORD bgot = 0;
  if (!ReadFile(vol, boot, sizeof(boot), &bgot, NULL) || bgot < 512u) {
    if (errbuf != NULL && errlen > 0) {
      (void)snprintf(errbuf, errlen, "cannot read volume boot sector");
    }
    CloseHandle(vol);
    return -1;
  }

  if (memcmp(boot + 3, k_ntfs_oem, sizeof(k_ntfs_oem)) != 0) {
    if (errbuf != NULL && errlen > 0) {
      (void)snprintf(errbuf, errlen, "not an NTFS volume");
    }
    CloseHandle(vol);
    return -1;
  }

  uint16_t bps = read_u16_le(boot + NTFS_BOOT_BPS_OFF);
  unsigned char spc_u = boot[NTFS_BOOT_SPC_OFF];
  if (bps == 0 || (bps % 512u) != 0 || spc_u == 0 || !is_pow2_u8(spc_u)) {
    if (errbuf != NULL && errlen > 0) {
      (void)snprintf(errbuf, errlen, "invalid NTFS boot parameters");
    }
    CloseHandle(vol);
    return -1;
  }

  uint64_t cluster_bytes = (uint64_t)bps * (uint64_t)spc_u;
  int64_t mft_lcn_i = read_i64_le(boot + NTFS_BOOT_MFT_LCN_OFF);
  if (mft_lcn_i < 0) {
    if (errbuf != NULL && errlen > 0) {
      (void)snprintf(errbuf, errlen, "invalid MFT LCN in boot sector");
    }
    CloseHandle(vol);
    return -1;
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
  if (DeviceIoControl(vol, FSCTL_GET_NTFS_VOLUME_DATA, NULL, 0, &vol_data, sizeof(vol_data), &vol_data_br, NULL) &&
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

  if (try_load_mft_runs_via_special_file(dl, &runs, &stream_total)) {
    if (mft_valid_len_ioctl > stream_total) {
      stream_total = mft_valid_len_ioctl;
    }
    stream_base_vcn = 0;
  } else {
    unsigned char *rec0 = (unsigned char *)malloc(record_size);
    if (!rec0) {
      if (errbuf != NULL && errlen > 0) {
        (void)snprintf(errbuf, errlen, "out of memory");
      }
      CloseHandle(vol);
      return -1;
    }
    if (!vol_read_at(vol, mft_vol_off, rec0, record_size)) {
      free(rec0);
      if (errbuf != NULL && errlen > 0) {
        (void)snprintf(errbuf, errlen, "cannot read $MFT record 0 from volume");
      }
      CloseHandle(vol);
      return -1;
    }
    if (!da_ntfs_fixup_record(rec0, record_size, bps)) {
      free(rec0);
      if (errbuf != NULL && errlen > 0) {
        (void)snprintf(errbuf, errlen, "invalid $MFT record 0");
      }
      CloseHandle(vol);
      return -1;
    }
    if (read_u32_le(rec0) != NTFS_MAGIC_FILE) {
      free(rec0);
      if (errbuf != NULL && errlen > 0) {
        (void)snprintf(errbuf, errlen, "bad $MFT record 0 signature");
      }
      CloseHandle(vol);
      return -1;
    }

    uint16_t run_rel = 0;
    uint32_t attr_len = 0;
    const unsigned char *adata = find_best_unnamed_nonresident_data(rec0, record_size, &run_rel, &attr_len);
    if (!adata || run_rel >= attr_len) {
      free(rec0);
      if (errbuf != NULL && errlen > 0) {
        (void)snprintf(errbuf, errlen, "no $MFT $DATA attribute");
      }
      CloseHandle(vol);
      return -1;
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
      if (errbuf != NULL && errlen > 0) {
        (void)snprintf(errbuf, errlen, "bad $MFT data run list");
      }
      CloseHandle(vol);
      return -1;
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
    if (errbuf != NULL && errlen > 0) {
      (void)snprintf(errbuf, errlen, "invalid MFT stream size");
    }
    return -1;
  }
  (void)stream_base_vcn;

  HANDLE dst =
      CreateFileW(destw, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (dst == INVALID_HANDLE_VALUE) {
    run_list_free(&runs);
    CloseHandle(vol);
    if (errbuf != NULL && errlen > 0) {
      (void)snprintf(errbuf, errlen, "cannot create output file (error %lu)", (unsigned long)GetLastError());
    }
    return -1;
  }

  if (on_progress != NULL) {
    on_progress(user, 0, 0, stream_total);
  }

  int st = mft_dump_stream_runs_to_dst(vol, dst, &runs, cluster_bytes, stream_total, errbuf, errlen, on_progress, user);
  run_list_free(&runs);
  CloseHandle(dst);
  CloseHandle(vol);
  if (st != 0) {
    return st;
  }
  if (on_progress != NULL && stream_total > 0) {
    on_progress(user, 100, stream_total, stream_total);
  }
  return 0;
}

static wchar_t *da_mft_utf8_to_wide_alloc(const char *utf8) {
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
  if (w == NULL) {
    return NULL;
  }
  if (MultiByteToWideChar(CP_UTF8, conv, utf8, -1, w, n) <= 0) {
    free(w);
    return NULL;
  }
  return w;
}

static bool da_mft_read_all_bytes(HANDLE h, unsigned char *buf, uint64_t total) {
  uint64_t got_total = 0;
  while (got_total < total) {
    uint64_t remain = total - got_total;
    DWORD chunk = (DWORD)(remain > (16ULL * 1024 * 1024) ? (16ULL * 1024 * 1024) : remain);
    DWORD got = 0;
    if (!ReadFile(h, buf + got_total, chunk, &got, NULL) || got == 0) {
      return false;
    }
    got_total += (uint64_t)got;
  }
  return true;
}

DISKATLAS_API scan_result_t *diskatlas_scan_import_raw_mft_file(const char *mft_dump_utf8,
                                                                const char *root_hint_utf8,
                                                                const scan_options_t *opts_in,
                                                                char *errbuf, size_t errbuf_len) {
  if (errbuf != NULL && errbuf_len > 0) {
    errbuf[0] = '\0';
  }
  if (mft_dump_utf8 == NULL || mft_dump_utf8[0] == '\0' || root_hint_utf8 == NULL || root_hint_utf8[0] == '\0') {
    if (errbuf != NULL && errbuf_len > 0) {
      (void)snprintf(errbuf, errbuf_len, "invalid arguments");
    }
    return NULL;
  }

  scan_options_t def_opts;
  memset(&def_opts, 0, sizeof(def_opts));
  def_opts.struct_version = DISKATLAS_SCAN_OPTIONS_STRUCT_VERSION;
  def_opts.flags = DISKATLAS_SCAN_OPTION_DUPLICATE_USE_MTIME;
  const scan_options_t *opts = opts_in != NULL ? opts_in : &def_opts;
  if (opts->struct_version != DISKATLAS_SCAN_OPTIONS_STRUCT_VERSION) {
    if (errbuf != NULL && errbuf_len > 0) {
      (void)snprintf(errbuf, errbuf_len, "unsupported scan_options_t version");
    }
    return NULL;
  }

  wchar_t *root_path_wide = da_mft_utf8_to_wide_alloc(root_hint_utf8);
  if (root_path_wide == NULL) {
    if (errbuf != NULL && errbuf_len > 0) {
      (void)snprintf(errbuf, errbuf_len, "invalid root path encoding");
    }
    return NULL;
  }

  wchar_t vol_path_full[MAX_PATH + 4];
  if (!GetVolumePathNameW(root_path_wide, vol_path_full, MAX_PATH)) {
    if (errbuf != NULL && errbuf_len > 0) {
      (void)snprintf(errbuf, errbuf_len, "GetVolumePathNameW failed (error %lu)",
                     (unsigned long)GetLastError());
    }
    free(root_path_wide);
    return NULL;
  }

  if (vol_path_full[0] == L'\0' || vol_path_full[1] != L':' || vol_path_full[2] != L'\\') {
    if (errbuf != NULL && errbuf_len > 0) {
      (void)snprintf(errbuf, errbuf_len, "could not resolve NTFS volume root");
    }
    free(root_path_wide);
    return NULL;
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

  HANDLE vol = CreateFileW(dev, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
  if (vol == INVALID_HANDLE_VALUE) {
    if (errbuf != NULL && errbuf_len > 0) {
      (void)snprintf(errbuf, errbuf_len, "cannot open volume (error %lu)", (unsigned long)GetLastError());
    }
    free(root_path_wide);
    return NULL;
  }

  unsigned char boot[4096];
  DWORD bgot = 0;
  if (!ReadFile(vol, boot, sizeof(boot), &bgot, NULL) || bgot < 512u) {
    if (errbuf != NULL && errbuf_len > 0) {
      (void)snprintf(errbuf, errbuf_len, "cannot read volume boot sector");
    }
    CloseHandle(vol);
    free(root_path_wide);
    return NULL;
  }

  if (memcmp(boot + 3, k_ntfs_oem, sizeof(k_ntfs_oem)) != 0) {
    if (errbuf != NULL && errbuf_len > 0) {
      (void)snprintf(errbuf, errbuf_len, "not an NTFS volume");
    }
    CloseHandle(vol);
    free(root_path_wide);
    return NULL;
  }

  uint16_t bps = read_u16_le(boot + NTFS_BOOT_BPS_OFF);
  unsigned char spc_u = boot[NTFS_BOOT_SPC_OFF];
  if (bps == 0 || (bps % 512u) != 0 || spc_u == 0 || !is_pow2_u8(spc_u)) {
    if (errbuf != NULL && errbuf_len > 0) {
      (void)snprintf(errbuf, errbuf_len, "invalid NTFS boot parameters");
    }
    CloseHandle(vol);
    free(root_path_wide);
    return NULL;
  }

  uint64_t cluster_bytes = (uint64_t)bps * (uint64_t)spc_u;

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

  NTFS_VOLUME_DATA_BUFFER vol_data;
  DWORD vol_data_br = 0;
  if (DeviceIoControl(vol, FSCTL_GET_NTFS_VOLUME_DATA, NULL, 0, &vol_data, sizeof(vol_data), &vol_data_br, NULL) &&
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
  }

  CloseHandle(vol);

  wchar_t *dump_w = da_mft_utf8_to_wide_alloc(mft_dump_utf8);
  if (dump_w == NULL) {
    if (errbuf != NULL && errbuf_len > 0) {
      (void)snprintf(errbuf, errbuf_len, "invalid dump path encoding");
    }
    free(root_path_wide);
    return NULL;
  }

  HANDLE fh = CreateFileW(dump_w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  free(dump_w);
  if (fh == INVALID_HANDLE_VALUE) {
    if (errbuf != NULL && errbuf_len > 0) {
      (void)snprintf(errbuf, errbuf_len, "cannot open dump file (error %lu)", (unsigned long)GetLastError());
    }
    free(root_path_wide);
    return NULL;
  }

  LARGE_INTEGER fsz = {{0}};
  if (!GetFileSizeEx(fh, &fsz)) {
    if (errbuf != NULL && errbuf_len > 0) {
      (void)snprintf(errbuf, errbuf_len, "GetFileSizeEx failed (error %lu)", (unsigned long)GetLastError());
    }
    CloseHandle(fh);
    free(root_path_wide);
    return NULL;
  }
  uint64_t file_bytes = (uint64_t)fsz.QuadPart;
  if (file_bytes > 1024ULL * 1024 * 1024) {
    if (errbuf != NULL && errbuf_len > 0) {
      (void)snprintf(errbuf, errbuf_len, "MFT dump exceeds 1 GiB limit");
    }
    CloseHandle(fh);
    free(root_path_wide);
    return NULL;
  }
  if (record_size == 0 || file_bytes < (uint64_t)record_size) {
    if (errbuf != NULL && errbuf_len > 0) {
      (void)snprintf(errbuf, errbuf_len, "dump file too small for NTFS record size (%u bytes)", (unsigned)record_size);
    }
    CloseHandle(fh);
    free(root_path_wide);
    return NULL;
  }

  uint64_t stream_total = file_bytes - (file_bytes % (uint64_t)record_size);
  if (stream_total == 0 || stream_total / (uint64_t)record_size > (uint64_t)SIZE_MAX / 8u) {
    if (errbuf != NULL && errbuf_len > 0) {
      (void)snprintf(errbuf, errbuf_len, "invalid MFT dump size");
    }
    CloseHandle(fh);
    free(root_path_wide);
    return NULL;
  }

  unsigned char *mft_buf = (unsigned char *)malloc((size_t)stream_total);
  if (mft_buf == NULL) {
    if (errbuf != NULL && errbuf_len > 0) {
      (void)snprintf(errbuf, errbuf_len, "out of memory for MFT buffer");
    }
    CloseHandle(fh);
    free(root_path_wide);
    return NULL;
  }

  if (!da_mft_read_all_bytes(fh, mft_buf, stream_total)) {
    if (errbuf != NULL && errbuf_len > 0) {
      (void)snprintf(errbuf, errbuf_len, "read dump failed (error %lu)", (unsigned long)GetLastError());
    }
    free(mft_buf);
    CloseHandle(fh);
    free(root_path_wide);
    return NULL;
  }
  CloseHandle(fh);

  unsigned char *probe = (unsigned char *)malloc((size_t)record_size);
  if (probe == NULL) {
    if (errbuf != NULL && errbuf_len > 0) {
      (void)snprintf(errbuf, errbuf_len, "out of memory");
    }
    free(mft_buf);
    free(root_path_wide);
    return NULL;
  }
  memcpy(probe, mft_buf, (size_t)record_size);
  if (!da_ntfs_fixup_record(probe, record_size, bps) || read_u32_le(probe) != NTFS_MAGIC_FILE) {
    if (errbuf != NULL && errbuf_len > 0) {
      (void)snprintf(errbuf, errbuf_len, "dump does not look like a raw NTFS $MFT (bad first record)");
    }
    free(probe);
    free(mft_buf);
    free(root_path_wide);
    return NULL;
  }
  free(probe);

  diskatlas_scan_result_t *r = (diskatlas_scan_result_t *)calloc(1, sizeof(diskatlas_scan_result_t));
  if (r == NULL) {
    if (errbuf != NULL && errbuf_len > 0) {
      (void)snprintf(errbuf, errbuf_len, "out of memory");
    }
    free(mft_buf);
    free(root_path_wide);
    return NULL;
  }

  r->options_copy = *opts;
  r->vol_cluster_bytes = cluster_bytes;

  ntfs_run_list_t runs = {0};
  if (!mft_parse_emit_mft_common(r, mft_buf, NULL, &runs, stream_total, record_size, bps, cluster_bytes, 0,
                                 vol_path_full, root_path_wide, root_mft_idx, opts)) {
    if (errbuf != NULL && errbuf_len > 0) {
      (void)snprintf(errbuf, errbuf_len, "MFT parse failed");
    }
    diskatlas_impl_free_heap(r);
    free(r);
    free(root_path_wide);
    return NULL;
  }
  free(root_path_wide);

  diskatlas_finalize_paths(r);

  if (diskatlas_compute_duplicate_clusters(r, r->options_copy.flags) != 0) {
    if (errbuf != NULL && errbuf_len > 0) {
      (void)snprintf(errbuf, errbuf_len, "duplicate index build failed");
    }
    diskatlas_impl_free_heap(r);
    free(r);
    return NULL;
  }

  atomic_store_explicit(&r->complete, 1u, memory_order_release);
  return (scan_result_t *)r;
}

DISKATLAS_API int diskatlas_win32_dump_mft_file(const char *volume_root_utf8, const char *dest_utf8,
                                                char *errbuf, size_t errlen,
                                                void (*on_progress)(void *user, int pct, uint64_t done,
                                                                    uint64_t total),
                                                void *user) {
  if (errbuf != NULL && errlen > 0) {
    errbuf[0] = '\0';
  }
  if (volume_root_utf8 == NULL || dest_utf8 == NULL || dest_utf8[0] == '\0') {
    if (errbuf != NULL && errlen > 0) {
      (void)snprintf(errbuf, errlen, "invalid arguments");
    }
    return -1;
  }
  const char *p = volume_root_utf8;
  while (*p == ' ' || *p == '\t') {
    p++;
  }
  if (!((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) || p[1] != ':') {
    if (errbuf != NULL && errlen > 0) {
      (void)snprintf(errbuf, errlen, "expected drive letter volume root (e.g. C:\\\\)");
    }
    return -1;
  }
  wchar_t dl = (wchar_t)toupper((unsigned char)p[0]);
  enable_backup_privilege_best_effort();

  wchar_t destw[MAX_PATH];
  if (MultiByteToWideChar(CP_UTF8, 0, dest_utf8, -1, destw, MAX_PATH) <= 0) {
    if (errbuf != NULL && errlen > 0) {
      (void)snprintf(errbuf, errlen, "invalid destination path encoding");
    }
    return -1;
  }

  wchar_t mft_path[40];
  HANDLE src = INVALID_HANDLE_VALUE;
  for (int fi = 0; fi < 2 && src == INVALID_HANDLE_VALUE; fi++) {
    if (fi == 0) {
      mft_path[0] = L'\\';
      mft_path[1] = L'\\';
      mft_path[2] = L'?';
      mft_path[3] = L'\\';
      mft_path[4] = dl;
      mft_path[5] = L':';
      mft_path[6] = L'\\';
      mft_path[7] = L'$';
      mft_path[8] = L'M';
      mft_path[9] = L'F';
      mft_path[10] = L'T';
      mft_path[11] = L'\0';
    } else {
      mft_path[0] = L'\\';
      mft_path[1] = L'\\';
      mft_path[2] = L'.';
      mft_path[3] = L'\\';
      mft_path[4] = dl;
      mft_path[5] = L':';
      mft_path[6] = L'\\';
      mft_path[7] = L'$';
      mft_path[8] = L'M';
      mft_path[9] = L'F';
      mft_path[10] = L'T';
      mft_path[11] = L'\0';
    }
    src = CreateFileW(mft_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (src != INVALID_HANDLE_VALUE) {
      break;
    }
    src = CreateFileW(mft_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (src != INVALID_HANDLE_VALUE) {
      break;
    }
  }
  if (src == INVALID_HANDLE_VALUE) {
    return mft_dump_via_volume_fallback(dl, destw, errbuf, errlen, on_progress, user);
  }

  LARGE_INTEGER fsz = {{0}};
  if (!GetFileSizeEx(src, &fsz)) {
    if (errbuf != NULL && errlen > 0) {
      (void)snprintf(errbuf, errlen, "GetFileSizeEx failed (error %lu)", (unsigned long)GetLastError());
    }
    CloseHandle(src);
    return -1;
  }
  uint64_t total = (uint64_t)fsz.QuadPart;

  HANDLE dst = CreateFileW(destw, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (dst == INVALID_HANDLE_VALUE) {
    if (errbuf != NULL && errlen > 0) {
      (void)snprintf(errbuf, errlen, "cannot create output file (error %lu)", (unsigned long)GetLastError());
    }
    CloseHandle(src);
    return -1;
  }

  enum { k_buf = 1 << 20 };
  unsigned char *buf = (unsigned char *)malloc(k_buf);
  if (buf == NULL) {
    if (errbuf != NULL && errlen > 0) {
      (void)snprintf(errbuf, errlen, "out of memory");
    }
    CloseHandle(dst);
    CloseHandle(src);
    return -1;
  }

  uint64_t done = 0;
  if (on_progress != NULL) {
    on_progress(user, total > 0 ? 0 : 0, 0, total);
  }

  while (done < total) {
    DWORD chunk = (DWORD)((total - done) > (uint64_t)k_buf ? (uint64_t)k_buf : (total - done));
    DWORD got = 0;
    if (!ReadFile(src, buf, chunk, &got, NULL) || got == 0) {
      if (errbuf != NULL && errlen > 0) {
        (void)snprintf(errbuf, errlen, "read $MFT failed at offset %llu (error %lu)",
                       (unsigned long long)done, (unsigned long)GetLastError());
      }
      free(buf);
      CloseHandle(dst);
      CloseHandle(src);
      return -1;
    }
    DWORD wrote = 0;
    if (!WriteFile(dst, buf, got, &wrote, NULL) || wrote != got) {
      if (errbuf != NULL && errlen > 0) {
        (void)snprintf(errbuf, errlen, "write failed (error %lu)", (unsigned long)GetLastError());
      }
      free(buf);
      CloseHandle(dst);
      CloseHandle(src);
      return -1;
    }
    done += (uint64_t)got;
    if (on_progress != NULL && total > 0) {
      int pct = (int)((done * 100ull) / total);
      if (pct > 100) {
        pct = 100;
      }
      on_progress(user, pct, done, total);
    }
  }

  free(buf);
  CloseHandle(dst);
  CloseHandle(src);
  if (on_progress != NULL && total > 0) {
    on_progress(user, 100, total, total);
  }
  return 0;
}

#endif /* _WIN32 */
