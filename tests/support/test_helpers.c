#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "test_helpers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#endif

static char *da_test_strdup(const char *s) {
  if (s == NULL) {
    return NULL;
  }
  size_t n = strlen(s) + 1u;
  char *out = (char *)malloc(n);
  if (out != NULL) {
    memcpy(out, s, n);
  }
  return out;
}

int da_test_fixture_path(const char *rel, char *out, size_t outsz) {
  if (rel == NULL || out == NULL || outsz == 0) {
    return -1;
  }
  int n = snprintf(out, outsz, "%s/%s", DISKATLAS_FIXTURES_DIR, rel);
  if (n < 0 || (size_t)n >= outsz) {
    return -1;
  }
  for (char *p = out; *p; p++) {
    if (*p == '\\') {
      *p = '/';
    }
  }
  return 0;
}

long da_test_read_file_bytes(const char *path, unsigned char **out_data, size_t *out_len) {
  if (out_data) {
    *out_data = NULL;
  }
  if (out_len) {
    *out_len = 0;
  }
  FILE *f = fopen(path, "rb");
  if (!f) {
    return -1;
  }
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return -1;
  }
  long sz = ftell(f);
  if (sz < 0) {
    fclose(f);
    return -1;
  }
  if (fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return -1;
  }
  unsigned char *buf = (unsigned char *)malloc((size_t)sz);
  if (!buf && sz > 0) {
    fclose(f);
    return -1;
  }
  if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
    free(buf);
    fclose(f);
    return -1;
  }
  fclose(f);
  if (out_data) {
    *out_data = buf;
  } else {
    free(buf);
  }
  if (out_len) {
    *out_len = (size_t)sz;
  }
  return sz;
}

bool da_test_wait_scan_complete(scan_result_t *result, unsigned timeout_ms) {
  unsigned elapsed = 0;
  const unsigned step = 50;
  while (elapsed < timeout_ms) {
    scan_progress_t pr = scan_get_progress(result);
    if (pr.is_complete) {
      return true;
    }
#if defined(_WIN32)
    Sleep(step);
#else
    {
      struct timespec ts = {0, (long)(step * 1000000u)};
      (void)nanosleep(&ts, NULL);
    }
#endif
    elapsed += step;
  }
  return scan_get_progress(result).is_complete;
}

static void normalize_path(char *p) {
  if (!p) {
    return;
  }
  for (; *p; p++) {
    if (*p == '\\') {
      *p = '/';
    }
  }
}

static uint32_t parse_kind_token(const char *tok) {
  if (strcmp(tok, "DIR") == 0) {
    return DISKATLAS_NODE_KIND_DIR;
  }
  if (strcmp(tok, "SYMLINK") == 0) {
    return DISKATLAS_NODE_KIND_SYMLINK;
  }
  return DISKATLAS_NODE_KIND_FILE;
}

static int cmp_expect_path(const void *a, const void *b);

int da_test_load_manifest(const char *manifest_rel, da_test_node_expect_t **out_items, size_t *out_count) {
  char path[1024];
  if (da_test_fixture_path(manifest_rel, path, sizeof(path)) != 0) {
    return -1;
  }
  FILE *f = fopen(path, "r");
  if (!f) {
    return -1;
  }

  da_test_node_expect_t *items = NULL;
  size_t count = 0;
  size_t cap = 0;
  char line[4096];
  while (fgets(line, sizeof(line), f)) {
    if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') {
      continue;
    }
    char *nl = strchr(line, '\n');
    if (nl) {
      *nl = '\0';
    }
    char *pipe1 = strchr(line, '|');
    if (!pipe1) {
      continue;
    }
    *pipe1 = '\0';
    char *pipe2 = strchr(pipe1 + 1, '|');
    if (!pipe2) {
      continue;
    }
    *pipe2 = '\0';
    const char *path_col = line;
    const char *size_col = pipe1 + 1;
    const char *kind_col = pipe2 + 1;

    if (count >= cap) {
      size_t nc = cap ? cap * 2u : 16u;
      da_test_node_expect_t *ni =
          (da_test_node_expect_t *)realloc(items, nc * sizeof(da_test_node_expect_t));
      if (!ni) {
        fclose(f);
        free(items);
        return -1;
      }
      items = ni;
      cap = nc;
    }
    items[count].path = da_test_strdup(path_col);
    items[count].size_bytes = strtoull(size_col, NULL, 10);
    items[count].kind = parse_kind_token(kind_col);
    count++;
  }
  fclose(f);
  if (count > 1) {
    qsort(items, count, sizeof(da_test_node_expect_t), cmp_expect_path);
  }
  *out_items = items;
  *out_count = count;
  return 0;
}

void da_test_free_expectations(da_test_node_expect_t *items, size_t count) {
  if (!items) {
    return;
  }
  for (size_t i = 0; i < count; i++) {
    free(items[i].path);
  }
  free(items);
}

static int cmp_node_path(const void *a, const void *b) {
  const file_node_t *na = (const file_node_t *)a;
  const file_node_t *nb = (const file_node_t *)b;
  const char *pa = na->path ? na->path : "";
  const char *pb = nb->path ? nb->path : "";
  char ba[4096];
  char bb[4096];
  strncpy(ba, pa, sizeof(ba) - 1);
  strncpy(bb, pb, sizeof(bb) - 1);
  normalize_path(ba);
  normalize_path(bb);
  return strcmp(ba, bb);
}

void da_test_sort_nodes(file_node_t *nodes, size_t count) {
  if (nodes && count > 1) {
    qsort(nodes, count, sizeof(file_node_t), cmp_node_path);
  }
}

int da_test_compare_scan_to_manifest(scan_result_t *result, const char *manifest_rel) {
  da_test_node_expect_t *exp = NULL;
  size_t exp_count = 0;
  TEST_ASSERT_EQUAL_INT(0, da_test_load_manifest(manifest_rel, &exp, &exp_count));
  TEST_ASSERT_GREATER_THAN(0, (int)exp_count);

  scan_results_view_t v = scan_get_results(result);
  TEST_ASSERT_NOT_NULL(v.nodes);

  file_node_t *sorted = (file_node_t *)malloc(v.count * sizeof(file_node_t));
  if (!sorted) {
    da_test_free_expectations(exp, exp_count);
    return -1;
  }
  memcpy(sorted, v.nodes, v.count * sizeof(file_node_t));
  da_test_sort_nodes(sorted, v.count);

  TEST_ASSERT_EQUAL_UINT32((unsigned)exp_count, (unsigned)v.count);
  for (size_t i = 0; i < exp_count; i++) {
    char got[4096];
    strncpy(got, sorted[i].path ? sorted[i].path : "", sizeof(got) - 1);
    normalize_path(got);
    char want[4096];
    strncpy(want, exp[i].path, sizeof(want) - 1);
    normalize_path(want);
    const char *suffix = got;
    size_t wl = strlen(want);
    size_t gl = strlen(got);
    if (wl > 0 && gl >= wl && strcmp(got + gl - wl, want) == 0) {
      suffix = got + gl - wl;
    }
    TEST_ASSERT_EQUAL_STRING(want, suffix);
    TEST_ASSERT_EQUAL_UINT64(exp[i].size_bytes, sorted[i].size_bytes);
    TEST_ASSERT_EQUAL_UINT32(exp[i].kind, sorted[i].attributes & DISKATLAS_NODE_KIND_MASK);
  }

  free(sorted);
  da_test_free_expectations(exp, exp_count);
  return 0;
}

static int cmp_expect_path(const void *a, const void *b) {
  const da_test_node_expect_t *ea = (const da_test_node_expect_t *)a;
  const da_test_node_expect_t *eb = (const da_test_node_expect_t *)b;
  char ba[4096];
  char bb[4096];
  strncpy(ba, ea->path ? ea->path : "", sizeof(ba) - 1);
  strncpy(bb, eb->path ? eb->path : "", sizeof(bb) - 1);
  normalize_path(ba);
  normalize_path(bb);
  return strcmp(ba, bb);
}

int da_test_compare_scan_results(scan_result_t *a, scan_result_t *b) {
  scan_results_view_t va = scan_get_results(a);
  scan_results_view_t vb = scan_get_results(b);
  TEST_ASSERT_EQUAL_UINT32((unsigned)va.count, (unsigned)vb.count);

  file_node_t *sa = (file_node_t *)malloc(va.count * sizeof(file_node_t));
  file_node_t *sb = (file_node_t *)malloc(vb.count * sizeof(file_node_t));
  TEST_ASSERT_NOT_NULL(sa);
  TEST_ASSERT_NOT_NULL(sb);
  memcpy(sa, va.nodes, va.count * sizeof(file_node_t));
  memcpy(sb, vb.nodes, vb.count * sizeof(file_node_t));
  da_test_sort_nodes(sa, va.count);
  da_test_sort_nodes(sb, vb.count);

  for (size_t i = 0; i < va.count; i++) {
    char pa[4096];
    char pb[4096];
    strncpy(pa, sa[i].path ? sa[i].path : "", sizeof(pa) - 1);
    strncpy(pb, sb[i].path ? sb[i].path : "", sizeof(pb) - 1);
    normalize_path(pa);
    normalize_path(pb);
    TEST_ASSERT_EQUAL_STRING(pa, pb);
    TEST_ASSERT_EQUAL_UINT64(sa[i].size_bytes, sb[i].size_bytes);
    TEST_ASSERT_EQUAL_UINT32(sa[i].attributes & DISKATLAS_NODE_KIND_MASK,
                             sb[i].attributes & DISKATLAS_NODE_KIND_MASK);
    TEST_ASSERT_EQUAL_UINT32(sa[i].win32_attributes, sb[i].win32_attributes);
  }
  free(sa);
  free(sb);
  return 0;
}

void da_test_ensure_tmp_dir(void) {
  char path[1024];
  if (da_test_fixture_path("tmp", path, sizeof(path)) != 0) {
    return;
  }
#if defined(_WIN32)
  CreateDirectoryA(path, NULL);
#else
  mkdir(path, 0755);
#endif
}
