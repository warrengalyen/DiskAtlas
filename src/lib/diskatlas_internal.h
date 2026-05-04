#ifndef DISKATLAS_INTERNAL_H
#define DISKATLAS_INTERNAL_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "diskatlas.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#if !defined(_WIN32)
#include <pthread.h>
#endif

typedef struct diskatlas_scan_result {
  atomic_uint_fast32_t cancel;
  atomic_uint_fast32_t complete;
  atomic_uint_least64_t bytes_accounted;
  atomic_uint_least64_t entry_visits;

#if defined(_WIN32)
  HANDLE worker;
  wchar_t *scan_root_wide;
  wchar_t **stk_paths;
  wchar_t *pat_buf;
  wchar_t *join_buf;
  size_t pat_wcap_chars;
  size_t join_wcap_chars;
#else
  pthread_t worker;
  int worker_started;
  char *scan_root_owned;
  char **stk_paths;
  char *pat_buf;
  char *join_buf;
  size_t pat_cap;
  size_t join_cap;
#endif

  uint32_t *stk_depths;
  size_t stk_len;
  size_t stk_cap;

  scan_options_t options_copy;

  char *path_blob;
  size_t path_blob_len;
  size_t path_blob_cap;
  size_t *path_offs;
  file_node_t *nodes;
  size_t node_count;
  size_t node_cap;

  uint32_t dup_max_group_id;
  size_t *dup_group_off;
  size_t *dup_group_mem;
  uint64_t vol_cluster_bytes;
} diskatlas_scan_result_t;

bool diskatlas_nodes_ensure_capacity(diskatlas_scan_result_t *r);
void diskatlas_finalize_paths(diskatlas_scan_result_t *r);
int diskatlas_compute_duplicate_clusters(diskatlas_scan_result_t *r, uint32_t scan_flags);

uint64_t diskatlas_basename_hash_ci_fold_utf8(const char *basename_utf8);

void diskatlas_impl_join_worker(diskatlas_scan_result_t *r);
void diskatlas_impl_free_heap(diskatlas_scan_result_t *r);

#if defined(_WIN32)
/** Append one scan node from explicit metadata (used by NTFS MFT scan path). */
bool diskatlas_win32_record_entry_metadata(diskatlas_scan_result_t *r,
                                           const wchar_t *full_path_w,
                                           uint64_t size_bytes,
                                           uint64_t mtime_unix_ns,
                                           uint32_t win32_attributes,
                                           bool is_dir);
/** Try to populate \p r from raw NTFS MFT; returns true if successful (no directory walking). */
bool diskatlas_scan_ntfs_mft(diskatlas_scan_result_t *r, wchar_t *root_path_wide,
                             const scan_options_t *opts);
#endif

#endif /* DISKATLAS_INTERNAL_H */
