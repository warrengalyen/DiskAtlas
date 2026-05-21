#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "diskatlas_internal.h"

#if defined(_WIN32)
#include <windows.h>
#endif

#define DA_CSV_HEADER_UI_BASE "File Name,Size,Allocated,Modified,Attributes,Files,Folders,DRIVECAPACITY,FREESPACE,USEDSPACE"
#define DA_CSV_IOBUF (256u * 1024u)

static void da_csv_export_set_err(char *errbuf, size_t errlen, const char *fmt, ...) {
  if (errbuf == NULL || errlen == 0) {
    return;
  }
  va_list ap;
  va_start(ap, fmt);
  (void)vsnprintf(errbuf, errlen, fmt, ap);
  va_end(ap);
  errbuf[errlen - 1] = '\0';
}

#if defined(_WIN32)
static wchar_t *utf8_to_wide_path(const char *utf8) {
  if (utf8 == NULL) {
    return NULL;
  }
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, NULL, 0);
  if (n <= 0) {
    n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
  }
  if (n <= 0) {
    return NULL;
  }
  wchar_t *w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
  if (w == NULL) {
    return NULL;
  }
  if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, n) <= 0) {
    free(w);
    return NULL;
  }
  return w;
}

static FILE *da_fopen_utf8_write(const char *path) {
  wchar_t *wp = utf8_to_wide_path(path);
  if (wp == NULL) {
    return NULL;
  }
  FILE *f = _wfopen(wp, L"wb");
  free(wp);
  return f;
}
#else
static FILE *da_fopen_utf8_write(const char *path) {
  return fopen(path, "wb");
}
#endif

void diskatlas_format_mtime_for_csv(uint64_t unix_ns, char *dst, size_t dstsz) {
  if (dst == NULL || dstsz == 0) {
    return;
  }
  if (unix_ns == 0) {
    dst[0] = '\0';
    return;
  }
  time_t sec = (time_t)(unix_ns / 1000000000ull);
  struct tm tm_local;
#if defined(_WIN32)
  if (localtime_s(&tm_local, &sec) != 0) {
    dst[0] = '\0';
    return;
  }
#else
  if (localtime_r(&sec, &tm_local) == NULL) {
    dst[0] = '\0';
    return;
  }
#endif
  if (strftime(dst, dstsz, "%Y-%m-%d %H:%M:%S", &tm_local) == 0) {
    dst[0] = '\0';
  }
}

static void fprint_csv_utf8_field(FILE *out, const char *s) {
  if (s == NULL) {
    s = "";
  }
  int need_quote = 0;
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    if (*p == '"' || *p == ',' || *p == '\n' || *p == '\r' || *p < 32u) {
      need_quote = 1;
      break;
    }
  }
  if (!need_quote) {
    fputs(s, out);
    return;
  }
  fputc('"', out);
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    if (*p == '"') {
      fputs("\"\"", out);
    } else {
      fputc((int)*p, out);
    }
  }
  fputc('"', out);
}

typedef struct {
  char *path;
  uint64_t subtree_files;
  uint64_t subtree_folders;
} dir_agg_t;

typedef struct {
  dir_agg_t *items;
  size_t count;
  size_t cap;
} dir_table_t;

static void dir_table_free(dir_table_t *t) {
  if (t->items) {
    for (size_t i = 0; i < t->count; i++) {
      free(t->items[i].path);
    }
    free(t->items);
  }
  t->items = NULL;
  t->count = 0;
  t->cap = 0;
}

static dir_agg_t *dir_table_find(dir_table_t *t, const char *path) {
  for (size_t i = 0; i < t->count; i++) {
    if (strcmp(t->items[i].path, path) == 0) {
      return &t->items[i];
    }
  }
  return NULL;
}

static bool dir_table_insert(dir_table_t *t, const char *path) {
  if (dir_table_find(t, path) != NULL) {
    return true;
  }
  if (t->count >= t->cap) {
    size_t nc = t->cap ? t->cap * 2u : 32u;
    dir_agg_t *ni = (dir_agg_t *)realloc(t->items, nc * sizeof(dir_agg_t));
    if (!ni) {
      return false;
    }
    t->items = ni;
    t->cap = nc;
  }
  char *copy = strdup(path);
  if (!copy) {
    return false;
  }
  t->items[t->count].path = copy;
  t->items[t->count].subtree_files = 0;
  t->items[t->count].subtree_folders = 0;
  t->count++;
  return true;
}

static const char *parent_dir_utf8(const char *path, char *buf, size_t bufsz) {
  if (path == NULL || path[0] == '\0') {
    return "";
  }
  strncpy(buf, path, bufsz - 1);
  buf[bufsz - 1] = '\0';
  char *slash = NULL;
  for (char *p = buf; *p; p++) {
    if (*p == '/' || *p == '\\') {
      slash = p;
    }
  }
  if (slash == NULL) {
    return "";
  }
  *slash = '\0';
  return buf;
}

static void bump_dir_chain(dir_table_t *t, const char *first_parent_dir, bool bump_files) {
  if (first_parent_dir == NULL || first_parent_dir[0] == '\0') {
    return;
  }
  char cur[4096];
  char par[4096];
  strncpy(cur, first_parent_dir, sizeof(cur) - 1);
  cur[sizeof(cur) - 1] = '\0';
  for (;;) {
    dir_agg_t *a = dir_table_find(t, cur);
    if (a != NULL) {
      if (bump_files) {
        a->subtree_files++;
      } else {
        a->subtree_folders++;
      }
    }
    const char *p = parent_dir_utf8(cur, par, sizeof(par));
    if (p[0] == '\0' || strcmp(p, cur) == 0) {
      break;
    }
    strncpy(cur, p, sizeof(cur) - 1);
    cur[sizeof(cur) - 1] = '\0';
  }
}

DISKATLAS_API int diskatlas_scan_export_csv(scan_result_t *result, const char *utf8_path,
                                            const diskatlas_csv_export_options_t *options,
                                            char *errbuf, size_t errbuf_len) {
  if (errbuf != NULL && errbuf_len > 0) {
    errbuf[0] = '\0';
  }
  if (result == NULL || utf8_path == NULL || utf8_path[0] == '\0') {
    da_csv_export_set_err(errbuf, errbuf_len, "invalid arguments");
    return -1;
  }

  scan_progress_t pr = scan_get_progress(result);
  if (!pr.is_complete) {
    da_csv_export_set_err(errbuf, errbuf_len, "scan is not complete");
    return -1;
  }

  scan_results_view_t v = scan_get_results(result);
  if (v.nodes == NULL) {
    da_csv_export_set_err(errbuf, errbuf_len, "no scan nodes");
    return -1;
  }

  uint64_t tot = 0, free_b = 0, used_b = 0, reserved = 0;
  bool include_reserved = false;
  if (options != NULL) {
    if (options->struct_version != DISKATLAS_CSV_EXPORT_OPTIONS_STRUCT_VERSION) {
      da_csv_export_set_err(errbuf, errbuf_len, "unsupported export options version");
      return -1;
    }
    tot = options->drive_capacity_bytes;
    free_b = options->free_space_bytes;
    used_b = options->used_space_bytes;
    reserved = options->reserved_space_bytes;
    include_reserved = (options->flags & DISKATLAS_CSV_EXPORT_INCLUDE_RESERVED) != 0;
  }

  dir_table_t dir_stats = {0};
  for (size_t i = 0; i < v.count; i++) {
    uint32_t kind = v.nodes[i].attributes & DISKATLAS_NODE_KIND_MASK;
    if (kind != DISKATLAS_NODE_KIND_DIR) {
      continue;
    }
    const char *p = v.nodes[i].path;
    if (p == NULL || p[0] == '\0') {
      continue;
    }
    if (!dir_table_insert(&dir_stats, p)) {
      dir_table_free(&dir_stats);
      da_csv_export_set_err(errbuf, errbuf_len, "out of memory");
      return -1;
    }
  }

  char parbuf[4096];
  for (size_t i = 0; i < v.count; i++) {
    const file_node_t *n = &v.nodes[i];
    uint32_t kind = n->attributes & DISKATLAS_NODE_KIND_MASK;
    if (kind == DISKATLAS_NODE_KIND_FILE && n->path != NULL) {
      const char *par = parent_dir_utf8(n->path, parbuf, sizeof(parbuf));
      bump_dir_chain(&dir_stats, par, true);
    }
  }
  for (size_t i = 0; i < v.count; i++) {
    const file_node_t *n = &v.nodes[i];
    uint32_t kind = n->attributes & DISKATLAS_NODE_KIND_MASK;
    if (kind == DISKATLAS_NODE_KIND_DIR && n->path != NULL) {
      const char *par = parent_dir_utf8(n->path, parbuf, sizeof(parbuf));
      if (strcmp(par, n->path) != 0) {
        bump_dir_chain(&dir_stats, par, false);
      }
    }
  }

  FILE *out = da_fopen_utf8_write(utf8_path);
  if (out == NULL) {
    dir_table_free(&dir_stats);
    da_csv_export_set_err(errbuf, errbuf_len, "cannot open for write: %s", strerror(errno));
    return -1;
  }

  char stackbuf[DA_CSV_IOBUF];
  setvbuf(out, stackbuf, _IOFBF, sizeof stackbuf);

  if (include_reserved) {
    if (fprintf(out, "%s,RESERVEDSPACE\n", DA_CSV_HEADER_UI_BASE) < 0) {
      goto write_fail;
    }
  } else {
    if (fprintf(out, "%s\n", DA_CSV_HEADER_UI_BASE) < 0) {
      goto write_fail;
    }
  }

  char mtime_buf[40];
  for (size_t i = 0; i < v.count; i++) {
    const file_node_t *n = &v.nodes[i];
    const char *path = n->path != NULL ? n->path : "";
    uint32_t kind = n->attributes & DISKATLAS_NODE_KIND_MASK;
    uint64_t sub_files = 0;
    uint64_t sub_folders = 0;
    if (kind == DISKATLAS_NODE_KIND_DIR) {
      dir_agg_t *a = dir_table_find(&dir_stats, path);
      if (a != NULL) {
        sub_files = a->subtree_files;
        sub_folders = a->subtree_folders;
      }
    }

    diskatlas_format_mtime_for_csv(n->mtime_unix_ns, mtime_buf, sizeof mtime_buf);

    fprint_csv_utf8_field(out, path);
    if (fprintf(out, ",%" PRIu64 ",%" PRIu64 ",%s,%" PRIu32 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
                    ",%" PRIu64,
                n->size_bytes, n->allocated_bytes, mtime_buf, n->win32_attributes, sub_files, sub_folders, tot,
                free_b, used_b) < 0) {
      goto write_fail;
    }
    if (include_reserved) {
      if (fprintf(out, ",%" PRIu64 "\n", reserved) < 0) {
        goto write_fail;
      }
    } else {
      if (fputc('\n', out) < 0) {
        goto write_fail;
      }
    }
  }

  dir_table_free(&dir_stats);
  if (fclose(out) != 0) {
    da_csv_export_set_err(errbuf, errbuf_len, "close error: %s", strerror(errno));
    return -1;
  }
  return 0;

write_fail:
  da_csv_export_set_err(errbuf, errbuf_len, "write error");
  dir_table_free(&dir_stats);
  fclose(out);
  return -1;
}
