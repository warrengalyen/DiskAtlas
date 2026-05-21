#define _POSIX_C_SOURCE 200809L

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "diskatlas_internal.h"

#if defined(_WIN32)
#define DA_PATH_STRCMP(a, b) _stricmp((a), (b))
#define DA_PATH_STRNCMP(a, b, n) _strnicmp((a), (b), (int)(n))
static char *da_path_strdup(const char *s) {
  return _strdup(s);
}
#else
#define DA_PATH_STRCMP(a, b) strcmp((a), (b))
#define DA_PATH_STRNCMP(a, b, n) strncmp((a), (b), (n))
static char *da_path_strdup(const char *s) {
  return strdup(s);
}
#endif

static char *da_path_join_repl(const char *prefix, const char *suffix_from_old) {
  size_t lp = strlen(prefix);
  size_t ls = strlen(suffix_from_old);
  char *o = (char *)malloc(lp + ls + 1u);
  if (o == NULL) {
    return NULL;
  }
  memcpy(o, prefix, lp);
  memcpy(o + lp, suffix_from_old, ls + 1u);
  return o;
}

DISKATLAS_API void scan_cancel(scan_result_t *result) {
  if (!result) {
    return;
  }
  atomic_store_explicit(&((diskatlas_scan_result_t *)result)->cancel, 1, memory_order_release);
}

DISKATLAS_API scan_progress_t scan_get_progress(scan_result_t *result) {
  scan_progress_t p;
  memset(&p, 0, sizeof(p));
  p.struct_version = DISKATLAS_SCAN_PROGRESS_STRUCT_VERSION;
  if (!result) {
    return p;
  }
  diskatlas_scan_result_t *r = (diskatlas_scan_result_t *)result;
  p.bytes_accounted = (uint64_t)atomic_load_explicit(&r->bytes_accounted, memory_order_relaxed);
  p.entry_count_visits =
      (uint64_t)atomic_load_explicit(&r->entry_visits, memory_order_relaxed);
  p.folder_count = (uint64_t)atomic_load_explicit(&r->folders_recorded, memory_order_relaxed);
  p.file_count = (uint64_t)atomic_load_explicit(&r->files_recorded, memory_order_relaxed);

  uint32_t c = atomic_load_explicit(&r->complete, memory_order_acquire);
  p.is_complete = (c != 0);
  uint32_t q = atomic_load_explicit(&r->cancel, memory_order_relaxed);
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
  diskatlas_scan_result_t *r = (diskatlas_scan_result_t *)result;
  if (!atomic_load_explicit(&r->complete, memory_order_acquire)) {
    return v;
  }
  atomic_thread_fence(memory_order_acquire);
  v.nodes = r->nodes;
  v.count = r->node_count;
  return v;
}

DISKATLAS_API void scan_result_free(scan_result_t *result) {
  if (!result) {
    return;
  }
  diskatlas_scan_result_t *r = (diskatlas_scan_result_t *)result;
  diskatlas_impl_join_worker(r);
  diskatlas_impl_free_heap(r);
  free(r);
}

DISKATLAS_API file_node_t *scan_result_nodes_mutable(scan_result_t *result, size_t *count_out) {
  if (count_out != NULL) {
    *count_out = 0;
  }
  if (!result) {
    return NULL;
  }
  diskatlas_scan_result_t *r = (diskatlas_scan_result_t *)result;
  if (!atomic_load_explicit(&r->complete, memory_order_acquire)) {
    return NULL;
  }
  atomic_thread_fence(memory_order_acquire);
  if (count_out != NULL) {
    *count_out = r->node_count;
  }
  return r->nodes;
}

DISKATLAS_API uint32_t diskatlas_dup_max_group_id(const scan_result_t *result) {
  if (!result) {
    return 0;
  }
  diskatlas_scan_result_t *r = (diskatlas_scan_result_t *)result;
  if (!atomic_load_explicit(&r->complete, memory_order_acquire)) {
    return 0;
  }
  atomic_thread_fence(memory_order_acquire);
  return r->dup_max_group_id;
}

DISKATLAS_API size_t diskatlas_dup_group_member_count(const scan_result_t *result,
                                                      uint32_t group_id) {
  size_t c = 0;
  diskatlas_dup_group_members(result, group_id, &c);
  return c;
}

DISKATLAS_API const size_t *diskatlas_dup_group_members(const scan_result_t *result,
                                                         uint32_t group_id,
                                                         size_t *out_count) {
  if (out_count != NULL) {
    *out_count = 0;
  }
  if (!result || group_id == 0u) {
    return NULL;
  }
  diskatlas_scan_result_t *r = (diskatlas_scan_result_t *)result;
  if (!atomic_load_explicit(&r->complete, memory_order_acquire)) {
    return NULL;
  }
  atomic_thread_fence(memory_order_acquire);
  if (group_id > r->dup_max_group_id || r->dup_group_off == NULL || r->dup_group_mem == NULL) {
    return NULL;
  }
  size_t lo = r->dup_group_off[group_id];
  size_t hi = r->dup_group_off[group_id + 1u];
  if (hi < lo) {
    return NULL;
  }
  size_t nc = hi - lo;
  if (out_count != NULL) {
    *out_count = nc;
  }
  if (nc == 0) {
    return NULL;
  }
  return r->dup_group_mem + lo;
}

static bool da_blob_append_utf8_path(diskatlas_scan_result_t *r, const char *utf8, size_t *out_off) {
  if (r == NULL || utf8 == NULL || out_off == NULL) {
    return false;
  }
  size_t len = strlen(utf8) + 1u;
  size_t need = r->path_blob_len + len;
  if (need < r->path_blob_len) {
    return false;
  }
  while (r->path_blob_cap < need) {
    size_t cap = r->path_blob_cap ? r->path_blob_cap : 64u;
    cap = cap + (cap >> 1u) + 64u;
    if (cap < need) {
      cap = need;
    }
    char *nb = (char *)realloc(r->path_blob, cap);
    if (nb == NULL) {
      return false;
    }
    r->path_blob = nb;
    r->path_blob_cap = cap;
  }
  memcpy(r->path_blob + r->path_blob_len, utf8, len);
  *out_off = r->path_blob_len;
  r->path_blob_len = need;
  return true;
}

DISKATLAS_API int scan_result_paths_after_rename(scan_result_t *result, const char *old_path_utf8,
                                                 const char *new_path_utf8) {
  if (result == NULL || old_path_utf8 == NULL || old_path_utf8[0] == '\0' || new_path_utf8 == NULL ||
      new_path_utf8[0] == '\0') {
    return -1;
  }
  diskatlas_scan_result_t *r = (diskatlas_scan_result_t *)result;
  if (!atomic_load_explicit(&r->complete, memory_order_acquire)) {
    return -1;
  }
  atomic_thread_fence(memory_order_acquire);
  if (r->nodes == NULL || r->path_offs == NULL || r->path_blob == NULL || r->node_count == 0u) {
    return -1;
  }

  const size_t old_len = strlen(old_path_utf8);
  int updated = 0;

  for (size_t i = 0; i < r->node_count; i++) {
    const char *p = r->nodes[i].path;
    if (p == NULL || p[0] == '\0') {
      continue;
    }

    char *replacement = NULL;
    if (DA_PATH_STRCMP(p, old_path_utf8) == 0) {
      replacement = da_path_strdup(new_path_utf8);
    } else {
      size_t plen = strlen(p);
      if (plen > old_len && (p[old_len] == '/' || p[old_len] == '\\') &&
          DA_PATH_STRNCMP(p, old_path_utf8, old_len) == 0) {
        replacement = da_path_join_repl(new_path_utf8, p + old_len);
      }
    }
    if (replacement == NULL) {
      continue;
    }

    size_t off = 0;
    if (!da_blob_append_utf8_path(r, replacement, &off)) {
      free(replacement);
      return -1;
    }
    free(replacement);

    r->path_offs[i] = off;
    r->nodes[i].path = r->path_blob + off;
    updated++;
  }

  return updated;
}
