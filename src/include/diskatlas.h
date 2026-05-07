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
#define DISKATLAS_FILE_NODE_STRUCT_VERSION 3u
#define DISKATLAS_SCAN_PROGRESS_STRUCT_VERSION 2u
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
/**
 * Windows only: raw NTFS $MFT scan (ignored on non-Windows). Intended for elevated processes.
 * FIXME(ntfs-mft): Implementation is still WIP — enable only after diskatlas_ntfs_mft.c is ready.
 */
#define DISKATLAS_SCAN_OPTION_WIN32_NTFS_MFT (1u << 3)
/** Skip duplicate clustering entirely (no duplicate_group_id assignments). */
#define DISKATLAS_SCAN_OPTION_SKIP_DUPLICATE_CLUSTERING (1u << 4)
/** Duplicate grouping matches full UTF-8 path (case-folded); default is basename-only. */
#define DISKATLAS_SCAN_OPTION_DUPLICATE_MATCH_FULL_PATH (1u << 5)

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
  uint32_t attributes;      /**< DISKATLAS_NODE_* bits (kind). */
  uint64_t size_bytes;      /**< Logical file size. */
  uint64_t allocated_bytes; /**< Best-effort on-disk allocation (e.g. cluster-rounded on Windows). */
  uint64_t mtime_unix_ns;   /**< UTC wall time if available; 0 if unknown. */
  const char *path;         /**< UTF-8, NUL-terminated; not owned by caller. */
  /** 0 = not in any multi-file duplicate cluster; otherwise shared id among same-cluster files. */
  uint32_t duplicate_group_id;
  /** Windows FILE_ATTRIBUTE_* from FindFirstFile when scanning on Win32; 0 if unset. */
  uint32_t win32_attributes;
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
  /** Directory nodes recorded so far (DISKATLAS_NODE_KIND_DIR). */
  uint64_t folder_count;
  /** Regular file nodes recorded so far (DISKATLAS_NODE_KIND_FILE). */
  uint64_t file_count;
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
/* - scan_start: allocates a scan_result_t and starts a background worker
 *   (native OS thread). Returns immediately while the filesystem scan runs.
 * - scan_cancel: thread-safe cancellation request; idempotent when complete.
 * - scan_get_progress / scan_get_results: observable from UI threads via atomics
 *   and memory ordering; callers must not tear down buffers until joined in
 *   scan_result_free.
 * - scan_result_free: joins the worker thread, then frees the result —
 *   not concurrent with other uses of this pointer after you begin freeing.
 * - path: UTF-8; native path separators per OS conventions. */
/* -------------------------------------------------------------------------- */

DISKATLAS_API scan_result_t *scan_start(const char *path,
                                        const scan_options_t *options);

DISKATLAS_API void scan_cancel(scan_result_t *result);

DISKATLAS_API scan_progress_t scan_get_progress(scan_result_t *result);

DISKATLAS_API scan_results_view_t scan_get_results(scan_result_t *result);

DISKATLAS_API void scan_result_free(scan_result_t *result);

/**
 * Import scan results from a UTF-8 CSV file in the DiskAtlas GUI export format (see csv_export). Reads
 * line-by-line with a growable buffer (capped). Returns a completed scan_result_t or NULL.
 */
DISKATLAS_API scan_result_t *diskatlas_scan_import_csv(const char *utf8_path, char *errbuf, size_t errbuf_len);

#if defined(_WIN32)
/**
 * Parse a raw NTFS $MFT dump into a completed scan_result_t.
 * @a root_hint_utf8 is any path on the volume (e.g. current scan root); used with GetVolumePathNameW for the
 * volume boot sector and to scope paths under the same root as a live MFT scan.
 */
DISKATLAS_API scan_result_t *diskatlas_scan_import_raw_mft_file(const char *mft_dump_utf8,
                                                                const char *root_hint_utf8,
                                                                const scan_options_t *options,
                                                                char *errbuf, size_t errbuf_len);

/**
 * Copy the NTFS $MFT stream for @a volume_root_utf8 (e.g. "C:\\") to @a dest_utf8.
 * @a on_progress is optional; when set, invoked on the same thread with @a pct in 0–100 and byte counts.
 */
DISKATLAS_API int diskatlas_win32_dump_mft_file(const char *volume_root_utf8, const char *dest_utf8,
                                                char *errbuf, size_t errbuf_len,
                                                void (*on_progress)(void *user, int pct, uint64_t done,
                                                                    uint64_t total),
                                                void *user);
#endif

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
/** Sentinel: no child / no next sibling in the tree link fields (see diskatlas_index_build_tree). */
#define DISKATLAS_INDEX_NO_CHILD (UINT32_MAX)
#define DISKATLAS_INDEX_NO_SIBLING (UINT32_MAX)

typedef struct diskatlas_index_entry {
  file_node_t node;
  uint32_t parent_id; /**< DISKATLAS_INDEX_NO_PARENT or index of parent. */
  uint32_t first_child_id;   /**< DISKATLAS_INDEX_NO_CHILD, or head of singly-linked child list. */
  uint32_t next_sibling_id;  /**< DISKATLAS_INDEX_NO_SIBLING, or next child of same parent. */
  uint64_t subtree_size_bytes; /**< After diskatlas_index_build_tree: aggregated size for treemap (dirs sum descendants). */
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

/**
 * Iterative post-pass: wires first_child_id / next_sibling_id from parent_id, then fills
 * subtree_size_bytes bottom-up (non-directory leaves contribute node.size_bytes; directories sum children).
 * Sibling order follows increasing child index along next_sibling_id (no recursion).
 * Requires each non-root entry to have parent_id < its own index (parent inserted before child).
 * Idempotent; safe to call again after further add_node (not allowed after finalize) — call before finalize
 * or temporarily unfinalize is not supported; typical use: after last add_node, call build_tree then finalize.
 * Returns 0 on success, -1 on NULL idx, malformed parent_id, or parent_id >= child index.
 */
DISKATLAS_API int diskatlas_index_build_tree(diskatlas_index_t *idx);

/** Frees slab; leaves idx in reusable zero state (calls diskatlas_index_init semantics). */
DISKATLAS_API void diskatlas_index_clear(diskatlas_index_t *idx);

/* --- Short names mirroring logical index_* API (same linkage as symbols above where used) --- */

DISKATLAS_API void index_init(diskatlas_index_t *idx);

DISKATLAS_API int index_add_node(diskatlas_index_t *idx, const file_node_t *node,
                                 uint32_t parent_id, uint32_t *out_id);

DISKATLAS_API void index_finalize(diskatlas_index_t *idx);

DISKATLAS_API int index_build_tree(diskatlas_index_t *idx);

#if defined(_WIN32)
/* -------------------------------------------------------------------------- */
/* Raw NTFS volume — VBR / $MFT location (no MFT record parsing).             */
/* -------------------------------------------------------------------------- */

#define DISKATLAS_NTFS_MFT_LOCATION_STRUCT_VERSION 1u

typedef struct diskatlas_ntfs_mft_location {
  uint32_t struct_version; /**< DISKATLAS_NTFS_MFT_LOCATION_STRUCT_VERSION. */
  uint32_t win32_error;    /**< GetLastError() on failure; 0 when return value is 0. */
  uint64_t mft_start_lcn;
  uint64_t mft_mirror_start_lcn;
  uint32_t bytes_per_sector;
  uint32_t sectors_per_cluster;
  uint64_t cluster_size_bytes;
  /** Byte offset of $MFT from the start of the volume (cluster × size). */
  uint64_t mft_byte_offset;
  uint64_t mft_mirror_byte_offset;
  /** Decoded from VBR clusters-per-MFT-record field; 0 if indeterminate. */
  uint32_t mft_record_size_bytes;
  uint32_t reserved_u32;
  uint64_t reserved_u64[2];
} diskatlas_ntfs_mft_location_t;

/**
 * Open a raw volume/device (e.g. UTF-8 "\\\\.\\C:"), read the NTFS boot sector,
 * and return $MFT / $MFTMirr starting LCNs and byte offsets. Administrator rights
 * are often required. Does not read or parse MFT file records.
 *
 * \return 0 on success, -1 on failure (\p out->win32_error set; ERROR_BAD_FORMAT if not NTFS).
 */
DISKATLAS_API int diskatlas_ntfs_get_mft_location(const char *volume_device_path_utf8,
                                                  diskatlas_ntfs_mft_location_t *out);
#endif /* _WIN32 */

#ifdef __cplusplus
}
#endif

#endif /* DISKATLAS_H */
