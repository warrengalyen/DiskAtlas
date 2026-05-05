#if defined(_WIN32)

#ifndef WINVER
#define WINVER 0x0601
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "diskatlas_internal.h"

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

static bool utf8_blob_append_wide(diskatlas_scan_result_t *r, const wchar_t *wpath,
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
  int wrote = WideCharToMultiByte(CP_UTF8, 0, wpath, -1, r->path_blob + nl, need, NULL, NULL);
  if (wrote <= 0) {
    return false;
  }
  *out_utf8_anchor = nl;
  r->path_blob_len = need_total;
  return true;
}

static bool stk_push(diskatlas_scan_result_t *r, wchar_t *owned_path, uint32_t depth) {
  if (r->stk_len == r->stk_cap) {
    size_t nc = r->stk_cap ? (r->stk_cap + (r->stk_cap >> 1u)) + 8u : 256u;
    wchar_t **np = (wchar_t **)realloc(r->stk_paths, nc * sizeof(wchar_t *));
    uint32_t *nd = (uint32_t *)realloc(r->stk_depths, nc * sizeof(uint32_t));
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

static void init_volume_cluster_from_path(diskatlas_scan_result_t *r, const wchar_t *path) {
  r->vol_cluster_bytes = 0;
  if (path == NULL || path[0] == L'\0') {
    r->vol_cluster_bytes = 4096;
    return;
  }
  wchar_t root[8];
  if (path[0] != L'\0' && path[1] == L':') {
    root[0] = path[0];
    root[1] = L':';
    root[2] = L'\\';
    root[3] = L'\0';
  } else {
    r->vol_cluster_bytes = 4096;
    return;
  }
  DWORD spc = 0, bps = 0, fcl = 0, tcl = 0;
  if (GetDiskFreeSpaceW(root, &spc, &bps, &fcl, &tcl) && spc > 0 && bps > 0) {
    r->vol_cluster_bytes = (uint64_t)spc * (uint64_t)bps;
  } else {
    r->vol_cluster_bytes = 4096;
  }
}

static uint64_t filetime_to_unix_ns(const FILETIME *ft) {
  if (ft->dwLowDateTime == 0 && ft->dwHighDateTime == 0) {
    return 0;
  }
  ULARGE_INTEGER uli;
  uli.LowPart = ft->dwLowDateTime;
  uli.HighPart = ft->dwHighDateTime;
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

bool diskatlas_win32_record_entry_metadata(diskatlas_scan_result_t *r,
                                           const wchar_t *full_path_w,
                                           uint64_t size_bytes,
                                           uint64_t mtime_unix_ns,
                                           uint32_t win32_attributes,
                                           bool is_dir) {
  if (!diskatlas_nodes_ensure_capacity(r)) {
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
  node.size_bytes = is_dir ? 0 : size_bytes;
  node.mtime_unix_ns = mtime_unix_ns;
  node.win32_attributes = win32_attributes;
  node.path = NULL;
  node.duplicate_group_id = DISKATLAS_DUPLICATE_GROUP_NONE;
  if (!is_dir) {
    uint64_t sz = node.size_bytes;
    uint64_t cl = r->vol_cluster_bytes ? r->vol_cluster_bytes : 4096;
    node.allocated_bytes = ((sz + cl - 1ull) / cl) * cl;
  } else {
    node.allocated_bytes = 0;
  }

  r->path_offs[r->node_count] = anchor;
  r->nodes[r->node_count] = node;
  r->node_count++;

  if (is_dir) {
    atomic_fetch_add_explicit(&r->folders_recorded, 1, memory_order_relaxed);
  } else {
    atomic_fetch_add_explicit(&r->files_recorded, 1, memory_order_relaxed);
  }

  atomic_fetch_add_explicit(&r->bytes_accounted,
                            (uint_least64_t)node.size_bytes,
                            memory_order_relaxed);
  return true;
}

static bool record_entry(diskatlas_scan_result_t *r, const wchar_t *full_path_w,
                         const WIN32_FIND_DATAW *fd, bool is_dir) {
  return diskatlas_win32_record_entry_metadata(r, full_path_w, file_node_size_bytes(fd, is_dir),
                                               filetime_to_unix_ns(&fd->ftLastWriteTime),
                                               fd->dwFileAttributes, is_dir);
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

static bool build_pattern(diskatlas_scan_result_t *r, const wchar_t *dir_path) {
  size_t dl = wcslen(dir_path);
  size_t min = dl + 2u + 16u;
  if (!wbuf_ensure(&r->pat_buf, &r->pat_wcap_chars, min)) {
    return false;
  }
  if (dl + 2 >= r->pat_wcap_chars) {
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

static bool combine_child_path(diskatlas_scan_result_t *r, const wchar_t *dir_path,
                               const wchar_t *name, size_t name_len) {
  size_t dl = wcslen(dir_path);
  size_t need = dl + 1u + name_len + 1u;
  if (!wbuf_ensure(&r->join_buf, &r->join_wcap_chars, need)) {
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

static void stk_drain_all(diskatlas_scan_result_t *r) {
  while (r->stk_len > 0) {
    wchar_t *p = r->stk_paths[--r->stk_len];
    free(p);
  }
}

static bool should_descend(uint32_t child_depth_dir, uint32_t max_depth) {
  if (max_depth == 0) {
    return true;
  }
  return child_depth_dir <= max_depth;
}

static void scan_drive_tree(diskatlas_scan_result_t *r, wchar_t *root_path,
                            const scan_options_t *opts) {
  init_volume_cluster_from_path(r, root_path);
  /* FIXME(ntfs-mft): Entry point for experimental MFT scan (see diskatlas_ntfs_mft.c). */
  if ((opts->flags & DISKATLAS_SCAN_OPTION_WIN32_NTFS_MFT) != 0 &&
      diskatlas_scan_ntfs_mft(r, root_path, opts)) {
    goto scan_done;
  }
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
    if (atomic_load_explicit(&r->cancel, memory_order_relaxed) != 0) {
      stk_drain_all(r);
      break;
    }

    wchar_t *dir_path = r->stk_paths[r->stk_len - 1];
    uint32_t depth = r->stk_depths[r->stk_len - 1];
    r->stk_len--;

    atomic_fetch_add_explicit(&r->entry_visits, 1, memory_order_relaxed);

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
          atomic_load_explicit(&r->cancel, memory_order_relaxed) != 0) {
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
      if (!include_hidden) {
        DWORD skip_mask = FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM;
        if ((att & FILE_ATTRIBUTE_DIRECTORY) != 0) {
          /* Match Explorer: SYSTEM alone does not hide folders (only hidden dirs). */
          skip_mask = FILE_ATTRIBUTE_HIDDEN;
        }
        if ((att & skip_mask) != 0) {
          continue;
        }
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
  diskatlas_finalize_paths(r);
  (void)diskatlas_compute_duplicate_clusters(r, r->options_copy.flags);
  atomic_store_explicit(&r->complete, 1, memory_order_release);
}

static DWORD WINAPI diskatlas_worker_main(void *param) {
  diskatlas_scan_result_t *r = (diskatlas_scan_result_t *)param;
  wchar_t *root = r->scan_root_wide;
  scan_drive_tree(r, root, &r->options_copy);
  return 0;
}

void diskatlas_impl_join_worker(diskatlas_scan_result_t *r) {
  if (!r || !r->worker) {
    return;
  }
  (void)WaitForSingleObject(r->worker, INFINITE);
  CloseHandle(r->worker);
  r->worker = NULL;
}

void diskatlas_impl_free_heap(diskatlas_scan_result_t *r) {
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

  free(r->dup_group_off);
  free(r->dup_group_mem);
  r->dup_group_off = NULL;
  r->dup_group_mem = NULL;
  r->dup_max_group_id = 0;

  if (r->scan_root_wide) {
    free(r->scan_root_wide);
    r->scan_root_wide = NULL;
  }
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

  diskatlas_scan_result_t *r =
      (diskatlas_scan_result_t *)calloc(1, sizeof(diskatlas_scan_result_t));
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
    diskatlas_impl_free_heap(r);
    free(r);
    return NULL;
  }

  memcpy(&r->options_copy, opts, sizeof(scan_options_t));
  r->scan_root_wide = full;

  HANDLE th = CreateThread(NULL, 0, diskatlas_worker_main, r, 0, NULL);
  if (!th) {
    diskatlas_impl_free_heap(r);
    free(r);
    return NULL;
  }

  r->worker = th;
  return (scan_result_t *)r;
}

#endif /* _WIN32 */
