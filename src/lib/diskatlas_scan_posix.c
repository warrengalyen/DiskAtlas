#if !defined(_WIN32)

#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>

#include "diskatlas_internal.h"

static char *dup_str(const char *s) {
  return s ? strdup(s) : NULL;
}

static char *normalize_scan_root(const char *path) {
  char buf[16384];
  if (path == NULL || path[0] == '\0') {
    return NULL;
  }
  if (realpath(path, buf) != NULL) {
    return strdup(buf);
  }
  return strdup(path);
}

static bool charbuf_ensure(char **buf, size_t *cap, size_t min_bytes) {
  if (*cap >= min_bytes) {
    return true;
  }
  size_t nb = *cap ? *cap * 2u : 4096u;
  while (nb < min_bytes) {
    nb *= 2u;
  }
  char *p = (char *)realloc(*buf, nb);
  if (!p) {
    return false;
  }
  *buf = p;
  *cap = nb;
  return true;
}

static bool join_utf8(diskatlas_scan_result_t *r, const char *dir, const char *name) {
  size_t dl = strlen(dir);
  size_t nl = strlen(name);
  size_t need = dl + nl + 2;
  if (!charbuf_ensure(&r->join_buf, &r->join_cap, need)) {
    return false;
  }
  memcpy(r->join_buf, dir, dl);
  size_t pos = dl;
  if (pos > 0 && r->join_buf[pos - 1] != '/') {
    r->join_buf[pos++] = '/';
  }
  memcpy(r->join_buf + pos, name, nl + 1);
  return true;
}

static bool utf8_blob_append_cstr(diskatlas_scan_result_t *r, const char *utf8_path,
                                  size_t *out_anchor) {
  size_t add = strlen(utf8_path) + 1u;
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
  memcpy(r->path_blob + nl, utf8_path, add);
  *out_anchor = nl;
  r->path_blob_len = need_total;
  return true;
}

static bool stk_push(diskatlas_scan_result_t *r, char *owned_path, uint32_t depth) {
  if (r->stk_len == r->stk_cap) {
    size_t nc = r->stk_cap ? (r->stk_cap + (r->stk_cap >> 1u)) + 8u : 256u;
    char **np = (char **)realloc(r->stk_paths, nc * sizeof(char *));
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

static void stk_drain_all(diskatlas_scan_result_t *r) {
  while (r->stk_len > 0) {
    char *p = r->stk_paths[--r->stk_len];
    free(p);
  }
}

static bool should_descend(uint32_t child_depth_dir, uint32_t max_depth) {
  if (max_depth == 0) {
    return true;
  }
  return child_depth_dir <= max_depth;
}

static uint64_t stat_mtime_unix_ns(const struct stat *st) {
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
  const struct timespec *ts = &st->st_mtimespec;
#else
  const struct timespec *ts = &st->st_mtim;
#endif
  return (uint64_t)ts->tv_sec * 1000000000ULL + (uint64_t)ts->tv_nsec;
}

static void init_volume_cluster_from_path(diskatlas_scan_result_t *r, const char *path) {
  r->vol_cluster_bytes = 4096;
  if (!path || !path[0]) {
    return;
  }
  struct statvfs vfs;
  memset(&vfs, 0, sizeof(vfs));
  if (statvfs(path, &vfs) == 0 && vfs.f_frsize > 0) {
    r->vol_cluster_bytes = (uint64_t)vfs.f_frsize;
  }
}

static uint64_t allocated_from_stat(const struct stat *st, uint64_t cluster) {
  if (S_ISREG(st->st_mode)) {
    uint64_t sz = (uint64_t)st->st_size;
    uint64_t cl = cluster ? cluster : 4096;
    return ((sz + cl - 1ull) / cl) * cl;
  }
  if (st->st_blocks > 0) {
    return (uint64_t)st->st_blocks * 512ull;
  }
  return 0;
}

static bool record_node(diskatlas_scan_result_t *r, const char *full_utf8, const struct stat *st,
                        uint32_t kind_mask) {
  if (!diskatlas_nodes_ensure_capacity(r)) {
    return false;
  }
  size_t anchor = 0;
  if (!utf8_blob_append_cstr(r, full_utf8, &anchor)) {
    return false;
  }

  file_node_t node;
  memset(&node, 0, sizeof(node));
  node.struct_version = DISKATLAS_FILE_NODE_STRUCT_VERSION;
  node.attributes = kind_mask & DISKATLAS_NODE_KIND_MASK;
  node.win32_attributes = 0;
  node.duplicate_group_id = DISKATLAS_DUPLICATE_GROUP_NONE;
  node.path = NULL;

  if ((kind_mask & DISKATLAS_NODE_KIND_MASK) == DISKATLAS_NODE_KIND_DIR) {
    node.size_bytes = 0;
    node.allocated_bytes = 0;
    node.mtime_unix_ns = stat_mtime_unix_ns(st);
  } else if ((kind_mask & DISKATLAS_NODE_KIND_MASK) == DISKATLAS_NODE_KIND_SYMLINK) {
    node.size_bytes = (uint64_t)st->st_size;
    node.allocated_bytes = 0;
    node.mtime_unix_ns = stat_mtime_unix_ns(st);
  } else {
    node.size_bytes = (uint64_t)st->st_size;
    node.mtime_unix_ns = stat_mtime_unix_ns(st);
    node.allocated_bytes = allocated_from_stat(st, r->vol_cluster_bytes);
  }

  r->path_offs[r->node_count] = anchor;
  r->nodes[r->node_count] = node;
  r->node_count++;

  {
    uint32_t k = kind_mask & DISKATLAS_NODE_KIND_MASK;
    if (k == DISKATLAS_NODE_KIND_DIR) {
      atomic_fetch_add_explicit(&r->folders_recorded, 1, memory_order_relaxed);
    } else if (k == DISKATLAS_NODE_KIND_FILE) {
      atomic_fetch_add_explicit(&r->files_recorded, 1, memory_order_relaxed);
    }
  }

  atomic_fetch_add_explicit(&r->bytes_accounted,
                            (uint_least64_t)node.size_bytes,
                            memory_order_relaxed);
  return true;
}

static void scan_posix_tree(diskatlas_scan_result_t *r, char *root_path,
                            const scan_options_t *opts) {
  init_volume_cluster_from_path(r, root_path);
  char *root_owned = root_path;
  if (!stk_push(r, root_owned, 0)) {
    free(root_owned);
    r->scan_root_owned = NULL;
    goto scan_done;
  }
  r->scan_root_owned = NULL;
  root_owned = NULL;

  const uint32_t max_depth_req = opts->max_depth;
  const bool include_hidden = (opts->flags & DISKATLAS_SCAN_OPTION_INCLUDE_HIDDEN) != 0;
  const bool follow = (opts->flags & DISKATLAS_SCAN_OPTION_FOLLOW_SYMLINKS) != 0;

  while (r->stk_len > 0) {
    if (atomic_load_explicit(&r->cancel, memory_order_relaxed) != 0) {
      stk_drain_all(r);
      break;
    }

    char *dir_path = r->stk_paths[r->stk_len - 1];
    uint32_t depth = r->stk_depths[r->stk_len - 1];
    r->stk_len--;

    atomic_fetch_add_explicit(&r->entry_visits, 1, memory_order_relaxed);

    DIR *d = opendir(dir_path);
    if (!d) {
      free(dir_path);
      continue;
    }

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
      if (atomic_load_explicit(&r->cancel, memory_order_relaxed) != 0) {
        break;
      }
      const char *nm = de->d_name;
      if (nm[0] == '.' && nm[1] == '\0') {
        continue;
      }
      if (nm[0] == '.' && nm[1] == '.' && nm[2] == '\0') {
        continue;
      }
      if (!include_hidden && nm[0] == '.') {
        continue;
      }

      if (!join_utf8(r, dir_path, nm)) {
        closedir(d);
        free(dir_path);
        stk_drain_all(r);
        goto scan_done;
      }

      struct stat lst;
      if (lstat(r->join_buf, &lst) != 0) {
        continue;
      }

      bool descend = false;
      if (S_ISLNK(lst.st_mode)) {
        if (follow) {
          struct stat fst;
          if (stat(r->join_buf, &fst) == 0) {
            if (S_ISDIR(fst.st_mode)) {
              if (!record_node(r, r->join_buf, &fst, DISKATLAS_NODE_KIND_DIR)) {
                closedir(d);
                free(dir_path);
                stk_drain_all(r);
                goto scan_done;
              }
              descend = true;
            } else if (S_ISREG(fst.st_mode)) {
              if (!record_node(r, r->join_buf, &fst, DISKATLAS_NODE_KIND_FILE)) {
                closedir(d);
                free(dir_path);
                stk_drain_all(r);
                goto scan_done;
              }
            } else {
              if (!record_node(r, r->join_buf, &lst, DISKATLAS_NODE_KIND_SYMLINK)) {
                closedir(d);
                free(dir_path);
                stk_drain_all(r);
                goto scan_done;
              }
            }
          } else {
            if (!record_node(r, r->join_buf, &lst, DISKATLAS_NODE_KIND_SYMLINK)) {
              closedir(d);
              free(dir_path);
              stk_drain_all(r);
              goto scan_done;
            }
          }
        } else {
          if (!record_node(r, r->join_buf, &lst, DISKATLAS_NODE_KIND_SYMLINK)) {
            closedir(d);
            free(dir_path);
            stk_drain_all(r);
            goto scan_done;
          }
        }
      } else if (S_ISDIR(lst.st_mode)) {
        if (!record_node(r, r->join_buf, &lst, DISKATLAS_NODE_KIND_DIR)) {
          closedir(d);
          free(dir_path);
          stk_drain_all(r);
          goto scan_done;
        }
        descend = true;
      } else if (S_ISREG(lst.st_mode)) {
        if (!record_node(r, r->join_buf, &lst, DISKATLAS_NODE_KIND_FILE)) {
          closedir(d);
          free(dir_path);
          stk_drain_all(r);
          goto scan_done;
        }
      } else {
        continue;
      }

      if (descend) {
        uint32_t child_depth = depth + 1u;
        if (should_descend(child_depth, max_depth_req)) {
          char *child_copy = dup_str(r->join_buf);
          if (!child_copy || !stk_push(r, child_copy, child_depth)) {
            free(child_copy);
            closedir(d);
            free(dir_path);
            stk_drain_all(r);
            goto scan_done;
          }
        }
      }
    }

    closedir(d);
    free(dir_path);
  }

scan_done:
  diskatlas_finalize_paths(r);
  (void)diskatlas_compute_duplicate_clusters(r, r->options_copy.flags);
  atomic_store_explicit(&r->complete, 1, memory_order_release);
}

static void *diskatlas_posix_worker(void *param) {
  diskatlas_scan_result_t *r = (diskatlas_scan_result_t *)param;
  char *root = r->scan_root_owned;
  scan_posix_tree(r, root, &r->options_copy);
  return NULL;
}

void diskatlas_impl_join_worker(diskatlas_scan_result_t *r) {
  if (!r || !r->worker_started) {
    return;
  }
  (void)pthread_join(r->worker, NULL);
  r->worker_started = 0;
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

  if (r->scan_root_owned) {
    free(r->scan_root_owned);
    r->scan_root_owned = NULL;
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

  char *full = normalize_scan_root(path);
  if (!full) {
    free(r);
    return NULL;
  }

  memcpy(&r->options_copy, opts, sizeof(scan_options_t));
  r->scan_root_owned = full;

  if (pthread_create(&r->worker, NULL, diskatlas_posix_worker, r) != 0) {
    diskatlas_impl_free_heap(r);
    free(r);
    return NULL;
  }
  r->worker_started = 1;
  return (scan_result_t *)r;
}

#endif /* !_WIN32 */
