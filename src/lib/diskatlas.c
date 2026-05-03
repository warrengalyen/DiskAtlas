#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WINVER
#define WINVER 0x0601
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#endif

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "diskatlas.h"

#if defined(_WIN32)

struct diskatlas_scan_result {
  volatile LONG cancel;
  volatile LONG complete;

  volatile LONG64 bytes_accounted;
  volatile LONG64 entry_visits;

  wchar_t *scan_root_wide; /* Owned; consumed by worker (first stk_push frees later). */

  HANDLE worker;

  scan_options_t options_copy;

  wchar_t **stk_paths;
  uint32_t *stk_depths;
  size_t stk_len;
  size_t stk_cap;

  char *path_blob;
  size_t path_blob_len;
  size_t path_blob_cap;

  size_t *path_offs;
  file_node_t *nodes;
  size_t node_count;
  size_t node_cap;

  wchar_t *pat_buf;
  size_t pat_wcap;
  wchar_t *join_buf;
  size_t join_wcap;
};

static void scan_drive_tree(struct diskatlas_scan_result *r, wchar_t *root_path,
                             const scan_options_t *opts);
static DWORD WINAPI diskatlas_worker_main(void *param);

static void scan_join_worker(struct diskatlas_scan_result *r);
static void scan_free_heap_state(struct diskatlas_scan_result *r);
static void scan_destroy(struct diskatlas_scan_result *r);

static void da_atomic_add_i64(volatile LONG64 *cell, LONG64 delta) {
  LONG64 d = delta;
  while (d > 0) {
    LONG64 chunk = d > 0x7fffffffLL ? 0x7fffffffLL : d;
    InterlockedExchangeAdd64(cell, chunk);
    d -= chunk;
  }
}

static uint64_t da_atomic_load_u64(volatile LONG64 *cell) {
  /* Reads are coherent with publisher Interlocked increments. */
  return (uint64_t)InterlockedExchangeAdd64(cell, 0LL);
}

static wchar_t *wcs_dup(const wchar_t *s) {
  size_t n = wcslen(s);
  size_t bytes = (n + 1u) * sizeof(wchar_t);
  wchar_t *r = (wchar_t *)malloc(bytes);
  if (!r) {
    return NULL;
  }
  memcpy(r, s, bytes);
  return r;
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

static wchar_t *normalize_full_path_w(const wchar_t *path) {
  DWORD need = GetFullPathNameW(path, 0, NULL, NULL);
  if (need == 0) {
    return wcs_dup(path);
  }
  wchar_t *buf = (wchar_t *)malloc((size_t)need * sizeof(wchar_t));
  if (!buf) {
    return NULL;
  }
  DWORD got = GetFullPathNameW(path, need, buf, NULL);
  if (got == 0 || got >= need) {
    free(buf);
    return wcs_dup(path);
  }
  return buf;
}

static bool wbuf_ensure(wchar_t **buf, size_t *wcap_chars, size_t min_chars) {
  if (*wcap_chars >= min_chars) {
    return true;
  }
  size_t nw = (*wcap_chars == 0) ? 32768u : (*wcap_chars * 2u);
  while (nw < min_chars) {
    nw *= 2u;
  }
  wchar_t *nb = (wchar_t *)realloc(*buf, nw * sizeof(wchar_t));
  if (!nb) {
    return false;
  }
  *buf = nb;
  *wcap_chars = nw;
  return true;
}

static bool utf8_blob_append_wide(struct diskatlas_scan_result *r, const wchar_t *wpath,
                                   size_t *out_utf8_anchor) {
  int need = WideCharToMultiByte(CP_UTF8, 0, wpath, -1, NULL, 0, NULL, NULL);
  if (need <= 0) {
    return false;
  }
  size_t add = (size_t)need;
  size_t nl = r->path_blob_len;
  size_t need_total = nl + add;
  if (need_total < nl) {
    return false;
  }
  while (need_total > r->path_blob_cap) {
    size_t nc = r->path_blob_cap ? r->path_blob_cap * 2u : (64u * 1024u);
    while (nc < need_total) {
      nc *= 2u;
    }
    char *nb = (char *)realloc(r->path_blob, nc);
    if (!nb) {
      return false;
    }
    r->path_blob = nb;
    r->path_blob_cap = nc;
  }
  WideCharToMultiByte(CP_UTF8, 0, wpath, -1, r->path_blob + nl, need, NULL, NULL);
  *out_utf8_anchor = nl;
  r->path_blob_len = need_total;
  return true;
}

static bool nodes_ensure_capacity(struct diskatlas_scan_result *r) {
  if (r->node_count < r->node_cap) {
    return true;
  }
  size_t nc =
      r->node_cap ? (r->node_cap + (r->node_cap >> 1u)) + 16u : 4096u; /* ~1.5x growth */
  file_node_t *nn = (file_node_t *)realloc(r->nodes, nc * sizeof(file_node_t));
  if (!nn) {
    return false;
  }
  size_t *no =
      (size_t *)realloc(r->path_offs, nc * sizeof(size_t)); /* MSVC C: size_t okay */
  if (!no) {
    return false;
  }
  r->nodes = nn;
  r->path_offs = no;
  r->node_cap = nc;
  return true;
}

static bool stk_push(struct diskatlas_scan_result *r, wchar_t *owned_path,
                     uint32_t depth) {
  if (r->stk_len == r->stk_cap) {
    size_t nc =
        r->stk_cap ? (r->stk_cap + (r->stk_cap >> 1u)) + 8u : 256u;
    wchar_t **np = (wchar_t **)realloc(r->stk_paths, nc * sizeof(wchar_t *));
    uint32_t *nd =
        (uint32_t *)realloc(r->stk_depths, nc * sizeof(uint32_t));
    if (!np || !nd) {
      free(np);
      free(nd);
      return false;
    }
    r->stk_paths = np;
    r->stk_depths = nd;
    r->stk_cap = nc;
  }
  r->stk_paths[r->stk_len] = owned_path;
  r->stk_depths[r->stk_len] = depth;
  r->stk_len++;
  return true;
}

static void finalize_paths(struct diskatlas_scan_result *r) {
  if (!r->path_blob || !r->nodes || !r->path_offs) {
    return;
  }
  for (size_t i = 0; i < r->node_count; i++) {
    r->nodes[i].path = r->path_blob + r->path_offs[i];
  }
}

static uint64_t filetime_to_unix_ns(const FILETIME *ft) {
  if (ft->dwLowDateTime == 0 && ft->dwHighDateTime == 0) {
    return 0;
  }
  ULARGE_INTEGER uli;
  uli.LowPart = ft->dwLowDateTime;
  uli.HighPart = ft->dwHighDateTime;
  /* Windows FILETIME ticks are 100ns intervals since 1601-01-01 UTC */
  const uint64_t epoch_diff_100ns = 116444736000000000ULL;
  if (uli.QuadPart < epoch_diff_100ns) {
    return 0;
  }
  uint64_t rel_100ns = uli.QuadPart - epoch_diff_100ns;
  return rel_100ns * 100ULL;
}

static uint64_t file_node_size_bytes(const WIN32_FIND_DATAW *fd, bool is_dir) {
  if (is_dir) {
    return 0;
  }
  ULARGE_INTEGER uli;
  uli.LowPart = fd->nFileSizeLow;
  uli.HighPart = fd->nFileSizeHigh;
  return uli.QuadPart;
}

static bool record_entry(struct diskatlas_scan_result *r, const wchar_t *full_path_w,
                         const WIN32_FIND_DATAW *fd, bool is_dir) {
  if (!nodes_ensure_capacity(r)) {
    return false;
  }
  size_t anchor = 0;
  if (!utf8_blob_append_wide(r, full_path_w, &anchor)) {
    return false;
  }

  uint32_t kind = is_dir ? DISKATLAS_NODE_KIND_DIR : DISKATLAS_NODE_KIND_FILE;
  uint32_t attr_field = kind & DISKATLAS_NODE_KIND_MASK;

  file_node_t node;
  memset(&node, 0, sizeof(node));
  node.struct_version = DISKATLAS_FILE_NODE_STRUCT_VERSION;
  node.attributes = attr_field;
  node.size_bytes = file_node_size_bytes(fd, is_dir);
  node.mtime_unix_ns = filetime_to_unix_ns(&fd->ftLastWriteTime);
  node.path = NULL;
  node.reserved_u64 = 0;

  r->path_offs[r->node_count] = anchor;
  r->nodes[r->node_count] = node;
  r->node_count++;

  da_atomic_add_i64(&r->bytes_accounted, (LONG64)node.size_bytes);
  return true;
}

static HANDLE find_first_file(const wchar_t *pattern, WIN32_FIND_DATAW *fd) {
  HANDLE h =
      FindFirstFileExW(pattern, FindExInfoBasic, fd, FindExSearchNameMatch, NULL,
                       FIND_FIRST_EX_LARGE_FETCH);
  if (h != INVALID_HANDLE_VALUE) {
    return h;
  }
  return FindFirstFileW(pattern, fd);
}

static bool build_pattern(struct diskatlas_scan_result *r, const wchar_t *dir_path) {
  size_t dl = wcslen(dir_path);
  size_t min = dl + 2u + 16u;
  if (!wbuf_ensure(&r->pat_buf, &r->pat_wcap, min)) {
    return false;
  }
  if (dl + 2 >= r->pat_wcap) {
    return false;
  }
  memcpy(r->pat_buf, dir_path, dl * sizeof(wchar_t));
  if (dl == 0 || (r->pat_buf[dl - 1] != L'\\' && r->pat_buf[dl - 1] != L'/')) {
    r->pat_buf[dl++] = L'\\';
  }
  r->pat_buf[dl++] = L'*';
  r->pat_buf[dl] = L'\0';
  return true;
}

static bool combine_child_path(struct diskatlas_scan_result *r, const wchar_t *dir_path,
                               const wchar_t *name, size_t name_len) {
  size_t dl = wcslen(dir_path);
  size_t need = dl + 1u + name_len + 1u;
  if (!wbuf_ensure(&r->join_buf, &r->join_wcap, need)) {
    return false;
  }
  memcpy(r->join_buf, dir_path, dl * sizeof(wchar_t));
  size_t pos = dl;
  if (pos > 0 && r->join_buf[pos - 1] != L'\\' && r->join_buf[pos - 1] != L'/') {
    r->join_buf[pos++] = L'\\';
  }
  memcpy(r->join_buf + pos, name, name_len * sizeof(wchar_t));
  pos += name_len;
  r->join_buf[pos] = L'\0';
  return true;
}

static void stk_drain_all(struct diskatlas_scan_result *r) {
  while (r->stk_len > 0) {
    wchar_t *p = r->stk_paths[--r->stk_len];
    free(p);
  }
}

static bool should_descend(uint32_t child_depth_dir, uint32_t max_depth) {
  /* max_depth==0 documented as unlimited */
  if (max_depth == 0) {
    return true;
  }
  return child_depth_dir <= max_depth;
}

DISKATLAS_API int diskatlas_init(void) {
  return 0;
}

static void scan_join_worker(struct diskatlas_scan_result *r) {
  if (!r || !r->worker) {
    return;
  }

  DWORD w = WaitForSingleObject(r->worker, INFINITE);
  (void)w;

  CloseHandle(r->worker);
  r->worker = NULL;
}

static void scan_free_heap_state(struct diskatlas_scan_result *r) {
  if (!r) {
    return;
  }

  for (size_t i = 0; i < r->stk_len; i++) {
    free(r->stk_paths[i]);
  }

  free(r->stk_paths);
  free(r->stk_depths);
  free(r->path_blob);
  free(r->path_offs);
  free(r->nodes);
  free(r->pat_buf);
  free(r->join_buf);

  if (r->scan_root_wide) {
    free(r->scan_root_wide);
    r->scan_root_wide = NULL;
  }
}

static void scan_destroy(struct diskatlas_scan_result *r) {
  if (!r) {
    return;
  }

  scan_join_worker(r);
  scan_free_heap_state(r);
  free(r);
}

static void scan_drive_tree(struct diskatlas_scan_result *r, wchar_t *root_path,
                             const scan_options_t *opts) {
  wchar_t *root_owned = root_path;
  if (!stk_push(r, root_owned, 0)) {
    free(root_owned);
    r->scan_root_wide = NULL;
    goto scan_done;
  }
  r->scan_root_wide = NULL;
  root_owned = NULL;

  uint32_t max_depth_req = opts->max_depth;

  while (r->stk_len > 0) {
    if (InterlockedCompareExchange(&r->cancel, 0, 0) != 0) {
      stk_drain_all(r);
      break;
    }

    wchar_t *dir_path = r->stk_paths[r->stk_len - 1];
    uint32_t depth = r->stk_depths[r->stk_len - 1];
    r->stk_len--;

    InterlockedIncrement64(&r->entry_visits);

    if (!build_pattern(r, dir_path)) {
      free(dir_path);
      goto scan_done;
    }

    WIN32_FIND_DATAW fd;
    HANDLE h = find_first_file(r->pat_buf, &fd);
    if (h == INVALID_HANDLE_VALUE) {
      free(dir_path);
      continue;
    }

    DWORD tick_guard = 0;
    do {
      if ((tick_guard++ & 63u) == 0u &&
          InterlockedCompareExchange(&r->cancel, 0, 0) != 0) {
        FindClose(h);
        free(dir_path);
        stk_drain_all(r);
        goto scan_done;
      }

      if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
        continue;
      }

      DWORD att = fd.dwFileAttributes;
      const bool include_hidden = (opts->flags & DISKATLAS_SCAN_OPTION_INCLUDE_HIDDEN) != 0;
      if (!include_hidden && (att & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) {
        continue;
      }

      size_t name_len = wcslen(fd.cFileName);
      if (!combine_child_path(r, dir_path, fd.cFileName, name_len)) {
        continue;
      }

      const bool is_reparse = (att & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
      const bool follow = (opts->flags & DISKATLAS_SCAN_OPTION_FOLLOW_SYMLINKS) != 0;
      const bool is_dir = (att & FILE_ATTRIBUTE_DIRECTORY) != 0;

      bool descend = is_dir;
      if (is_reparse && !follow) {
        descend = false;
      }

      if (!record_entry(r, r->join_buf, &fd, is_dir)) {
        FindClose(h);
        free(dir_path);
        stk_drain_all(r);
        goto scan_done;
      }

      if (descend) {
        uint32_t child_depth = depth + 1u;
        if (should_descend(child_depth, max_depth_req)) {
          wchar_t *child_copy = wcs_dup(r->join_buf);
          if (!child_copy || !stk_push(r, child_copy, child_depth)) {
            free(child_copy);
            FindClose(h);
            free(dir_path);
            stk_drain_all(r);
            goto scan_done;
          }
        }
      }
    } while (FindNextFileW(h, &fd));

    FindClose(h);
    free(dir_path);
  }

scan_done:
  finalize_paths(r);
  MemoryBarrier();
  InterlockedExchange(&r->complete, 1);
}

static DWORD WINAPI diskatlas_worker_main(void *param) {
  struct diskatlas_scan_result *r = (struct diskatlas_scan_result *)param;
  wchar_t *root = r->scan_root_wide;
  scan_drive_tree(r, root, &r->options_copy);
  return 0;
}

DISKATLAS_API scan_result_t *scan_start(const char *path, const scan_options_t *options) {
  if (!path) {
    return NULL;
  }

  scan_options_t def;
  memset(&def, 0, sizeof(def));
  def.struct_version = DISKATLAS_SCAN_OPTIONS_STRUCT_VERSION;
  def.flags = 0;
  def.max_depth = 0;
  def.io_threads = 0;

  const scan_options_t *opts = options ? options : &def;
  if (opts->struct_version != DISKATLAS_SCAN_OPTIONS_STRUCT_VERSION) {
    return NULL;
  }

  struct diskatlas_scan_result *r =
      (struct diskatlas_scan_result *)calloc(1, sizeof(struct diskatlas_scan_result));
  if (!r) {
    return NULL;
  }

  wchar_t *wrel = utf8_to_wide_path(path);
  if (!wrel) {
    free(r);
    return NULL;
  }
  wchar_t *full = normalize_full_path_w(wrel);
  free(wrel);
  if (!full) {
    scan_destroy(r);
    return NULL;
  }

  memcpy(&r->options_copy, opts, sizeof(scan_options_t));
  r->scan_root_wide = full;

  HANDLE th = CreateThread(NULL, 0, diskatlas_worker_main, r, 0, NULL);
  if (!th) {
    scan_destroy(r);
    return NULL;
  }

  r->worker = th;
  return (scan_result_t *)r;
}

DISKATLAS_API void scan_cancel(scan_result_t *result) {
  if (!result) {
    return;
  }
  InterlockedExchange(&((struct diskatlas_scan_result *)result)->cancel, 1);
}

DISKATLAS_API scan_progress_t scan_get_progress(scan_result_t *result) {
  scan_progress_t p;
  memset(&p, 0, sizeof(p));
  p.struct_version = DISKATLAS_SCAN_PROGRESS_STRUCT_VERSION;
  if (!result) {
    return p;
  }
  struct diskatlas_scan_result *r = (struct diskatlas_scan_result *)result;
  p.bytes_accounted = da_atomic_load_u64(&r->bytes_accounted);
  p.entry_count_visits = da_atomic_load_u64(&r->entry_visits);

  LONG c = InterlockedCompareExchange(&r->complete, 0, 0);
  p.is_complete = (c != 0);
  LONG q = InterlockedCompareExchange(&r->cancel, 0, 0);
  p.is_cancel_requested = (q != 0);

  p.is_running = !p.is_complete;
  p.is_cancel_observed = (q != 0);

  return p;
}

DISKATLAS_API scan_results_view_t scan_get_results(scan_result_t *result) {
  scan_results_view_t v;
  memset(&v, 0, sizeof(v));
  v.struct_version = DISKATLAS_SCAN_RESULTS_VIEW_STRUCT_VERSION;
  if (!result) {
    return v;
  }
  struct diskatlas_scan_result *r = (struct diskatlas_scan_result *)result;
  if (!InterlockedCompareExchange(&r->complete, 0, 0)) {
    return v;
  }
  MemoryBarrier();
  v.nodes = r->nodes;
  v.count = r->node_count;
  return v;
}

DISKATLAS_API void scan_result_free(scan_result_t *result) {
  if (!result) {
    return;
  }

  scan_destroy((struct diskatlas_scan_result *)result);
}

#else /* !_WIN32 */

struct diskatlas_scan_result {
  int unused;
};

DISKATLAS_API int diskatlas_init(void) {
  return 0;
}

DISKATLAS_API scan_result_t *scan_start(const char *path, const scan_options_t *options) {
  (void)path;
  (void)options;
  return NULL;
}

DISKATLAS_API void scan_cancel(scan_result_t *result) {
  (void)result;
}

DISKATLAS_API scan_progress_t scan_get_progress(scan_result_t *result) {
  (void)result;
  scan_progress_t p;
  memset(&p, 0, sizeof(p));
  return p;
}

DISKATLAS_API scan_results_view_t scan_get_results(scan_result_t *result) {
  (void)result;
  scan_results_view_t v;
  memset(&v, 0, sizeof(v));
  return v;
}

DISKATLAS_API void scan_result_free(scan_result_t *result) {
  free(result);
}

#endif /* _WIN32 */
