#ifndef DISKATLAS_H
#define DISKATLAS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#if defined(_WIN32) || defined(__CYGWIN__)
#ifdef DISKATLAS_BUILD_SHARED
#define DISKATLAS_API __declspec(dllexport)
#else
#define DISKATLAS_API __declspec(dllimport)
#endif
#elif defined(__GNUC__)
#define DISKATLAS_API __attribute__((visibility("default")))
#else
#define DISKATLAS_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** Library-wide init (optional); safe to call once before any scan_* from any thread. */
DISKATLAS_API int diskatlas_init(void);

/* -------------------------------------------------------------------------- */
/* Versioning: bump when fields are appended; zero-initialize new structs.   */
/* -------------------------------------------------------------------------- */

#define DISKATLAS_SCAN_OPTIONS_STRUCT_VERSION 1u
#define DISKATLAS_FILE_NODE_STRUCT_VERSION 2u
#define DISKATLAS_SCAN_PROGRESS_STRUCT_VERSION 1u
#define DISKATLAS_SCAN_RESULTS_VIEW_STRUCT_VERSION 1u

/* -------------------------------------------------------------------------- */
/* scan_options_t — forward-compatible options (extend with new trailing fields,
 * bump struct_version in a future header revision; keep flags for behavior bits). */
/* -------------------------------------------------------------------------- */

typedef struct scan_options {
  uint32_t struct_version; /**< Must be DISKATLAS_SCAN_OPTIONS_STRUCT_VERSION. */
  uint32_t flags;          /**< Bitmask; see DISKATLAS_SCAN_OPTION_*. */
  uint32_t max_depth;      /**< 0 = unlimited directory depth. */
  uint32_t io_threads;     /**< Hint for parallel directory reads; 0 = library default. */
  uint64_t reserved_u64[2];
  void *reserved_ptr;
} scan_options_t;

#define DISKATLAS_SCAN_OPTION_FOLLOW_SYMLINKS (1u << 0)
#define DISKATLAS_SCAN_OPTION_INCLUDE_HIDDEN (1u << 1)
/** Duplicate grouping includes exact mtime (nanoseconds); same size/name still split if mtimes differ. */
#define DISKATLAS_SCAN_OPTION_DUPLICATE_USE_MTIME (1u << 2)

/* -------------------------------------------------------------------------- */
/* scan_result_t — incomplete / opaque; only scan_result_t* may appear in API. */
/* -------------------------------------------------------------------------- */

typedef struct diskatlas_scan_result scan_result_t;

/* -------------------------------------------------------------------------- */
/* file_node_t — minimal entry metadata; strings are library-owned for the
 * lifetime of the scan_result_t they came from (see scan_get_results). */
/* -------------------------------------------------------------------------- */

typedef struct file_node {
  uint32_t struct_version; /**< DISKATLAS_FILE_NODE_STRUCT_VERSION. */
  uint32_t attributes;      /**< DISKATLAS_NODE_* bits. */
  uint64_t size_bytes;
  uint64_t mtime_unix_ns; /**< UTC wall time if available; 0 if unknown. */
  const char *path;       /**< UTF-8, NUL-terminated; not owned by caller. */
  /** 0 = not in any multi-file duplicate cluster; otherwise shared id among same-cluster files. */
  uint32_t duplicate_group_id;
  uint32_t reserved_u32;
} file_node_t;

/** Sentinel: file is unique or unmatched for duplicate clustering. */
#define DISKATLAS_DUPLICATE_GROUP_NONE (0u)

#define DISKATLAS_NODE_KIND_MASK (7u << 0)
#define DISKATLAS_NODE_KIND_UNKNOWN 0u
#define DISKATLAS_NODE_KIND_FILE 1u
#define DISKATLAS_NODE_KIND_DIR 2u
#define DISKATLAS_NODE_KIND_SYMLINK 3u

/* -------------------------------------------------------------------------- */
/* scan_progress_t — instantaneous snapshot returned by scan_get_progress. */
/* Safe to poll from UI threads while a scan worker runs. Implementation must
 * not block other threads indefinitely (short critical sections only).          */
/* -------------------------------------------------------------------------- */

typedef struct scan_progress {
  uint32_t struct_version; /**< DISKATLAS_SCAN_PROGRESS_STRUCT_VERSION. */
  uint32_t phase; /**< Reserved for future phased scans; implementation-defined today. */
  uint64_t bytes_accounted;
  uint64_t entry_count_visits;
  bool is_running;
  bool is_complete;
  bool is_cancel_requested;
  bool is_cancel_observed;
  uint64_t reserved_u64;
} scan_progress_t;

/* -------------------------------------------------------------------------- */
/* scan_results_view_t — read-only snapshot from scan_get_results. Valid only
 * while scan_result_t is alive and after logical completion/cancel settles. */
/* -------------------------------------------------------------------------- */

typedef struct scan_results_view {
  uint32_t struct_version; /**< DISKATLAS_SCAN_RESULTS_VIEW_STRUCT_VERSION. */
  uint32_t reserved_u32;
  const file_node_t *nodes;
  size_t count;
  uint64_t reserved_u64;
} scan_results_view_t;

/* -------------------------------------------------------------------------- */
/* Lifecycle & threading contract (informative)                                */
/* - scan_start: allocates a scan_result_t; launches a Win32 worker via CreateThread
 *   and returns immediately while the filesystem scan runs on that thread.
 * - scan_cancel: thread-safe cancellation request; idempotent when complete.
 * - scan_get_progress / scan_get_results: observable from UI threads via atomics
 *   and minimal barriers; callers must not tear down buffers until joined in
 *   scan_result_free.
 * - scan_result_free: WaitForSingleObject on the worker, then frees result —
 *   not concurrent with other uses of this pointer after you begin freeing.
 * - path: UTF-8; native path separators per OS conventions. */
/* -------------------------------------------------------------------------- */

DISKATLAS_API scan_result_t *scan_start(const char *path,
                                        const scan_options_t *options);

DISKATLAS_API void scan_cancel(scan_result_t *result);

DISKATLAS_API scan_progress_t scan_get_progress(scan_result_t *result);

DISKATLAS_API scan_results_view_t scan_get_results(scan_result_t *result);

DISKATLAS_API void scan_result_free(scan_result_t *result);

/** Highest assigned duplicate_group_id (>0); 0 when no duplicates. Valid after scan completes. */
DISKATLAS_API uint32_t diskatlas_dup_max_group_id(const scan_result_t *result);

/** Count of file_node indices stored for this group id (members >= 2 per group by construction). */
DISKATLAS_API size_t diskatlas_dup_group_member_count(const scan_result_t *result,
                                                      uint32_t group_id);

/** Read-only contiguous member list: node indices into scan_get_results().nodes. NULL if invalid. */
DISKATLAS_API const size_t *diskatlas_dup_group_members(const scan_result_t *result,
                                                         uint32_t group_id,
                                                         size_t *out_count);

/* -------------------------------------------------------------------------- */
/* Contiguous scan index — parent linkage by ID only (tree navigation TBD).  */
/* All entries live in a single realloc'd array (growth by doubling); no     */
/* per-node allocations. Implicit node id equals array index [0,count).       */
/* -------------------------------------------------------------------------- */

#define DISKATLAS_INDEX_STRUCT_VERSION 1u
/** Sentinel: node has no parent in this index (e.g., logical roots). */
#define DISKATLAS_INDEX_NO_PARENT (UINT32_MAX)

typedef struct diskatlas_index_entry {
  file_node_t node;
  uint32_t parent_id; /**< DISKATLAS_INDEX_NO_PARENT or index of parent. */
  uint32_t reserved_u32;
} diskatlas_index_entry_t;

typedef struct diskatlas_index {
  uint32_t struct_version;
  diskatlas_index_entry_t *entries; /**< Owned; single contiguous slab. */
  size_t count;
  size_t capacity;
  unsigned char finalized; /**< Non-zero after index_finalize(): no adds. */
} diskatlas_index_t;

/** Zero-initialize index; allocates no storage yet (see index_add_node growth). */
DISKATLAS_API void diskatlas_index_init(diskatlas_index_t *idx);

/** Shallow-insert a copy of *node at the end of the slab; assigns id == current count before push.
 * Paths in file_node_t remain pointers owned by whoever supplied *node's lifetime (e.g. scan blob).
 * Returns 0 on success, -1 on invalid args / finalized / allocation / bad parent id / overflow.
 * Sets *out_id when non-NULL (id is uint32_t only if count stays < 4Gi entries). */
DISKATLAS_API int diskatlas_index_add_node(diskatlas_index_t *idx, const file_node_t *node,
                                           uint32_t parent_id, uint32_t *out_id);

/** Trims capacity down to count (best-effort realloc) and marks index read-only for adds.
 * Safe to call more than once. */
DISKATLAS_API void diskatlas_index_finalize(diskatlas_index_t *idx);

DISKATLAS_API size_t diskatlas_index_count(const diskatlas_index_t *idx);

/** O(1) lookup; NULL if idx NULL or i >= count. */
DISKATLAS_API const diskatlas_index_entry_t *diskatlas_index_get(const diskatlas_index_t *idx,
                                                                 size_t i);

/** Frees slab; leaves idx in reusable zero state (calls diskatlas_index_init semantics). */
DISKATLAS_API void diskatlas_index_clear(diskatlas_index_t *idx);

/* --- Short names mirroring logical index_* API (same linkage as symbols above where used) --- */

DISKATLAS_API void index_init(diskatlas_index_t *idx);

DISKATLAS_API int index_add_node(diskatlas_index_t *idx, const file_node_t *node,
                                 uint32_t parent_id, uint32_t *out_id);

DISKATLAS_API void index_finalize(diskatlas_index_t *idx);

#ifdef __cplusplus
}
#endif

#endif /* DISKATLAS_H */
