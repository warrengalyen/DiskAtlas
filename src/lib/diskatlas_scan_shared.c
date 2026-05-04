#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#include "diskatlas_internal.h"

DISKATLAS_API int diskatlas_init(void) {
#if !defined(_WIN32)
  setlocale(LC_ALL, "");
#endif
  return 0;
}

bool diskatlas_nodes_ensure_capacity(diskatlas_scan_result_t *r) {
  if (r->node_count < r->node_cap) {
    return true;
  }
  size_t nc =
      r->node_cap ? (r->node_cap + (r->node_cap >> 1u)) + 16u : 4096u;
  file_node_t *nn = (file_node_t *)realloc(r->nodes, nc * sizeof(file_node_t));
  if (!nn) {
    return false;
  }
  size_t *no = (size_t *)realloc(r->path_offs, nc * sizeof(size_t));
  if (!no) {
    free(nn);
    return false;
  }
  r->nodes = nn;
  r->path_offs = no;
  r->node_cap = nc;
  return true;
}

void diskatlas_finalize_paths(diskatlas_scan_result_t *r) {
  if (!r->path_blob || !r->nodes || !r->path_offs) {
    return;
  }
  for (size_t i = 0; i < r->node_count; i++) {
    r->nodes[i].path = r->path_blob + r->path_offs[i];
  }
}

static const char *utf8_basename_ptr(const char *path) {
  const char *base = path ? path : "";
  for (const char *q = base; *q; q++) {
    if (*q == '/' || *q == '\\') {
      base = q + 1;
    }
  }
  return base;
}

static void fnv1a64_fold_wchar(uint64_t *h, wchar_t c) {
  *h ^= (uint8_t)(c & 0xFF);
  *h *= 1099511628211ULL;
  *h ^= (uint8_t)((unsigned)(c >> 8) & 0xFFu);
  *h *= 1099511628211ULL;
}

uint64_t diskatlas_basename_hash_ci_fold_utf8(const char *basename_utf8) {
  uint64_t h = 14695981039346656037ULL;
  if (basename_utf8 == NULL || basename_utf8[0] == '\0') {
    return h;
  }
  const char *p = basename_utf8;
  mbstate_t st;
  memset(&st, 0, sizeof(st));
  while (*p) {
    wchar_t wc = 0;
    size_t n = mbrtowc(&wc, p, 32, &st);
    if (n == (size_t)0) {
      break;
    }
    if (n == (size_t)-1 || n == (size_t)-2) {
      memset(&st, 0, sizeof(st));
      uint8_t b = (uint8_t)*p++;
      h ^= b;
      h *= 1099511628211ULL;
      continue;
    }
    wchar_t lc = towlower(wc);
    fnv1a64_fold_wchar(&h, lc);
    p += n;
  }
  return h;
}

typedef struct {
  size_t node_index;
  uint64_t size_bytes;
  uint64_t name_hash;
  uint64_t mtime_key;
} DupSortRec;

static int CmpDupSortRec(const void *a, const void *b) {
  const DupSortRec *x = (const DupSortRec *)a;
  const DupSortRec *y = (const DupSortRec *)b;
  if (x->size_bytes != y->size_bytes) {
    return (x->size_bytes > y->size_bytes) - (x->size_bytes < y->size_bytes);
  }
  if (x->name_hash != y->name_hash) {
    return (x->name_hash > y->name_hash) - (x->name_hash < y->name_hash);
  }
  if (x->mtime_key != y->mtime_key) {
    return (x->mtime_key > y->mtime_key) - (x->mtime_key < y->mtime_key);
  }
  return (x->node_index > y->node_index) - (x->node_index < y->node_index);
}

static void ClearAllDuplicateAssignments(diskatlas_scan_result_t *r) {
  if (!r->nodes) {
    return;
  }
  for (size_t i = 0; i < r->node_count; i++) {
    r->nodes[i].duplicate_group_id = DISKATLAS_DUPLICATE_GROUP_NONE;
  }
}

static bool NodeIsEligibleFileEntry(const diskatlas_scan_result_t *r, size_t i) {
  uint32_t kind = DISKATLAS_NODE_KIND_MASK & r->nodes[i].attributes;
  return kind == DISKATLAS_NODE_KIND_FILE;
}

static int DupBuildMemberTable(diskatlas_scan_result_t *r, uint32_t max_gid) {
  if (max_gid == 0 || r == NULL || r->nodes == NULL) {
    return 0;
  }

  uint32_t *hist = (uint32_t *)calloc((size_t)max_gid + 1u, sizeof(uint32_t));
  if (!hist) {
    return -1;
  }

  for (size_t i = 0; i < r->node_count; i++) {
    uint32_t g = r->nodes[i].duplicate_group_id;
    if (g == 0 || g > max_gid) {
      continue;
    }
    hist[g]++;
  }

  size_t off_count = (size_t)max_gid + 2u;
  size_t *off = (size_t *)calloc(off_count, sizeof(size_t));
  if (!off) {
    free(hist);
    return -1;
  }

  off[1] = 0;
  for (uint32_t g = 1; g <= max_gid; g++) {
    size_t cnt = (size_t)hist[g];
    off[g + 1] = off[g] + cnt;
  }
  size_t total = off[max_gid + 1];

  size_t *mem = NULL;
  if (total > 0) {
    mem = (size_t *)malloc(total * sizeof(size_t));
    if (mem == NULL) {
      free(hist);
      free(off);
      return -1;
    }
  }

  uint32_t *wr = (uint32_t *)calloc((size_t)max_gid + 1u, sizeof(uint32_t));
  if (!wr) {
    free(hist);
    free(off);
    free(mem);
    return -1;
  }

  for (size_t i = 0; i < r->node_count; i++) {
    uint32_t g = r->nodes[i].duplicate_group_id;
    if (g == 0 || g > max_gid) {
      continue;
    }
    size_t pos = off[g] + (size_t)wr[g]++;
    mem[pos] = i;
  }

  free(hist);
  free(wr);
  r->dup_group_off = off;
  r->dup_group_mem = mem;
  r->dup_max_group_id = max_gid;
  (void)total;
  return 0;
}

int diskatlas_compute_duplicate_clusters(diskatlas_scan_result_t *r, uint32_t scan_flags) {
  ClearAllDuplicateAssignments(r);
  r->dup_max_group_id = 0;
  free(r->dup_group_off);
  free(r->dup_group_mem);
  r->dup_group_off = NULL;
  r->dup_group_mem = NULL;

  if (!r || !r->nodes || r->node_count == 0) {
    return 0;
  }

  const bool use_mtime = (scan_flags & DISKATLAS_SCAN_OPTION_DUPLICATE_USE_MTIME) != 0;

  size_t nf = 0;
  for (size_t i = 0; i < r->node_count; i++) {
    if (NodeIsEligibleFileEntry(r, i)) {
      nf++;
    }
  }
  if (nf < 2) {
    return 0;
  }

  DupSortRec *rec = (DupSortRec *)malloc(nf * sizeof(DupSortRec));
  if (!rec) {
    return -1;
  }

  size_t wi = 0;
  for (size_t i = 0; i < r->node_count; i++) {
    if (!NodeIsEligibleFileEntry(r, i)) {
      continue;
    }
    const char *bn = utf8_basename_ptr(r->nodes[i].path);
    rec[wi].node_index = i;
    rec[wi].size_bytes = r->nodes[i].size_bytes;
    rec[wi].name_hash = diskatlas_basename_hash_ci_fold_utf8(bn);
    rec[wi].mtime_key = use_mtime ? r->nodes[i].mtime_unix_ns : 0;
    wi++;
  }

  qsort(rec, nf, sizeof(DupSortRec), CmpDupSortRec);

  uint32_t next_gid = 1;

  size_t rs = 0;
  while (rs < nf) {
    size_t re = rs + 1;
    while (re < nf && rec[re].size_bytes == rec[rs].size_bytes &&
           rec[re].name_hash == rec[rs].name_hash &&
           rec[re].mtime_key == rec[rs].mtime_key) {
      re++;
    }
    size_t runlen = re - rs;
    if (runlen >= 2) {
      uint32_t assign = next_gid++;
      if (assign == UINT32_MAX) {
        ClearAllDuplicateAssignments(r);
        free(rec);
        return -1;
      }
      for (size_t j = rs; j < re; j++) {
        r->nodes[rec[j].node_index].duplicate_group_id = assign;
      }
    }
    (void)runlen;
    rs = re;
  }

  free(rec);

  uint32_t max_gid = (next_gid > 0) ? next_gid - 1u : 0u;
  if (max_gid == 0) {
    r->dup_max_group_id = 0;
    return 0;
  }
  if (DupBuildMemberTable(r, max_gid) != 0) {
    ClearAllDuplicateAssignments(r);
    r->dup_max_group_id = 0;
    free(r->dup_group_off);
    free(r->dup_group_mem);
    r->dup_group_off = NULL;
    r->dup_group_mem = NULL;
    return -1;
  }

  return 0;
}
