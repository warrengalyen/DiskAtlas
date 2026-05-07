#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <time.h>

#include "diskatlas_internal.h"

#if defined(_WIN32)
#ifndef DISKATLAS_CSV_H
#define DISKATLAS_CSV_H
#endif
#include <windows.h>
#endif

#define DA_CSV_HEADER_UI_BASE "File Name,Size,Allocated,Modified,Attributes,Files,Folders,DRIVECAPACITY,FREESPACE,USEDSPACE"
#define DA_CSV_LINE_CAP ((size_t)512u * 1024u * 1024u)
#define DA_CSV_IOBUF (256u * 1024u)

static void da_csv_set_err(char *errbuf, size_t errlen, const char *fmt, ...) {
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

static FILE *da_fopen_utf8(const char *path, const wchar_t *wmode) {
  wchar_t *wp = utf8_to_wide_path(path);
  if (wp == NULL) {
    return NULL;
  }
  FILE *f = _wfopen(wp, wmode);
  free(wp);
  return f;
}
#else
static FILE *da_fopen_utf8(const char *path, const char *mode) {
  return fopen(path, mode);
}
#endif

static bool da_path_blob_append_utf8(diskatlas_scan_result_t *r, const char *utf8_path, size_t *out_anchor) {
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
    if (nb == NULL) {
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

static int da_parse_mtime_field_to_ns(const char *s, uint64_t *out_ns) {
  if (s == NULL || s[0] == '\0') {
    *out_ns = 0;
    return 0;
  }
  /* Same as export: local wall time "YYYY-MM-DD HH:MM:SS" (see da_format_mtime_local). */
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
  if (sscanf(s, "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &sec) != 6) {
    return -1;
  }
  struct tm tm;
  memset(&tm, 0, sizeof(tm));
  tm.tm_year = y - 1900;
  tm.tm_mon = mo - 1;
  tm.tm_mday = d;
  tm.tm_hour = h;
  tm.tm_min = mi;
  tm.tm_sec = sec;
  tm.tm_isdst = -1;
  time_t t = mktime(&tm);
  if (t == (time_t)-1) {
    return -1;
  }
  *out_ns = (uint64_t)t * 1000000000ull;
  return 0;
}

static uint32_t da_kind_from_win32_attrs(uint32_t wa) {
  if ((wa & 0x400u) != 0u) {
    return DISKATLAS_NODE_KIND_SYMLINK;
  }
  if ((wa & 0x10u) != 0u) {
    return DISKATLAS_NODE_KIND_DIR;
  }
  return DISKATLAS_NODE_KIND_FILE;
}

/** 1 = 10 columns, 2 = 11 columns with RESERVEDSPACE, -1 = unknown. */
static int da_csv_detect_format(const char *body) {
  const char *base = DA_CSV_HEADER_UI_BASE;
  size_t lb = strlen(base);
  if (strncmp(body, base, lb) != 0) {
    return -1;
  }
  if (body[lb] == '\0') {
    return 1;
  }
  if (strcmp(body + lb, ",RESERVEDSPACE") == 0) {
    return 2;
  }
  return -1;
}

static int da_csv_read_line(FILE *fp, char **buf, size_t *cap) {
  size_t pos = 0;
  for (;;) {
    if (pos + 2u > *cap) {
      if (*cap >= DA_CSV_LINE_CAP) {
        return -1;
      }
      size_t nc = *cap ? (*cap * 2u) : (256u * 1024u);
      if (nc < pos + 2u) {
        nc = pos + 2u;
      }
      if (nc > DA_CSV_LINE_CAP) {
        nc = DA_CSV_LINE_CAP;
      }
      char *nb = (char *)realloc(*buf, nc);
      if (nb == NULL) {
        return -2;
      }
      *buf = nb;
      *cap = nc;
    }
    int c = fgetc(fp);
    if (c == EOF) {
      if (pos == 0) {
        return 0;
      }
      (*buf)[pos] = '\0';
      return 1;
    }
    if (c == '\r') {
      int c2 = fgetc(fp);
      if (c2 != '\n') {
        if (c2 != EOF) {
          (void)ungetc(c2, fp);
        }
      }
      (*buf)[pos] = '\0';
      return 1;
    }
    if (c == '\n') {
      (*buf)[pos] = '\0';
      return 1;
    }
    (*buf)[pos++] = (char)c;
  }
}

static void da_csv_chomp(char *s) {
  size_t n = strlen(s);
  while (n > 0 && (s[n - 1] == '\r' || s[n - 1] == '\n')) {
    s[--n] = '\0';
  }
}

/** Parse one CSV line into fields; writes NULs into line. Returns field count or -1 on syntax error. */
static int da_csv_split_line(char *line, char **cols, int maxcols) {
  char *p = line;
  int n = 0;
  while (n < maxcols) {
    if (*p == '\0') {
      return n;
    }
    if (*p == '"') {
      p++;
      char *dst = p;
      cols[n] = dst;
      int closed = 0;
      while (*p != '\0') {
        if (*p == '"') {
          if (p[1] == '"') {
            *dst++ = '"';
            p += 2;
            continue;
          }
          *dst = '\0';
          p++;
          if (*p == ',') {
            p++;
          } else if (*p != '\0') {
            return -1;
          }
          n++;
          closed = 1;
          break;
        }
        *dst++ = *p++;
      }
      if (!closed) {
        return -1;
      }
      continue;
    }
    cols[n] = p;
    while (*p != '\0' && *p != ',') {
      p++;
    }
    if (*p == ',') {
      *p++ = '\0';
    }
    n++;
  }
  return *p != '\0' ? -1 : n;
}

static void da_csv_import_free_stub(diskatlas_scan_result_t *r) {
  if (r == NULL) {
    return;
  }
  free(r->path_blob);
  free(r->path_offs);
  free(r->nodes);
  free(r->dup_group_off);
  free(r->dup_group_mem);
  free(r);
}

DISKATLAS_API scan_result_t *diskatlas_scan_import_csv(const char *utf8_path, char *errbuf, size_t errlen) {
  if (utf8_path == NULL || utf8_path[0] == '\0') {
    da_csv_set_err(errbuf, errlen, "invalid path");
    return NULL;
  }

#if defined(_WIN32)
  FILE *fp = da_fopen_utf8(utf8_path, L"rb");
#else
  FILE *fp = da_fopen_utf8(utf8_path, "rb");
#endif  /* DISKATLAS_CSV_H */
  if (fp == NULL) {
    da_csv_set_err(errbuf, errlen, "cannot open %s: %s", utf8_path, strerror(errno));
    return NULL;
  }

  char iobuf[DA_CSV_IOBUF];
  setvbuf(fp, iobuf, _IOFBF, sizeof(iobuf));

  char *line = NULL;
  size_t linecap = 0;
  int lr = da_csv_read_line(fp, &line, &linecap);
  if (lr < 0) {
    da_csv_set_err(errbuf, errlen, lr == -1 ? "line exceeds maximum length" : "out of memory");
    free(line);
    fclose(fp);
    return NULL;
  }
  if (lr == 0 || line == NULL) {
    da_csv_set_err(errbuf, errlen, "empty file");
    free(line);
    fclose(fp);
    return NULL;
  }

  /* UTF-8 BOM */
  const char *body = line;
  if ((unsigned char)body[0] == 0xefu && (unsigned char)body[1] == 0xbbu && (unsigned char)body[2] == 0xbfu) {
    body += 3;
  }
  int fmt = da_csv_detect_format(body);
  if (fmt < 0) {
    da_csv_set_err(errbuf, errlen,
                   "missing or invalid header (expected DiskAtlas CSV export: File Name,Size,…)");
    free(line);
    fclose(fp);
    return NULL;
  }

  diskatlas_scan_result_t *r = (diskatlas_scan_result_t *)calloc(1, sizeof(diskatlas_scan_result_t));
  if (r == NULL) {
    da_csv_set_err(errbuf, errlen, "out of memory");
    free(line);
    fclose(fp);
    return NULL;
  }

  const int expect_nf = (fmt == 1) ? 10 : 11;
  char *cols[16];
  uint64_t row_ix = 0;

  while ((lr = da_csv_read_line(fp, &line, &linecap)) != 0) {
    if (lr < 0) {
      da_csv_set_err(errbuf, errlen, lr == -1 ? "line exceeds maximum length" : "out of memory");
      da_csv_import_free_stub(r);
      free(line);
      fclose(fp);
      return NULL;
    }
    da_csv_chomp(line);
    if (line[0] == '\0') {
      continue;
    }
    if (line[0] == '#') {
      continue;
    }

    int nf = da_csv_split_line(line, cols, 16);
    if (nf < 0) {
      da_csv_set_err(errbuf, errlen, "CSV syntax error at data row %" PRIu64, row_ix + 1u);
      da_csv_import_free_stub(r);
      free(line);
      fclose(fp);
      return NULL;
    }
    if (nf != expect_nf) {
      da_csv_set_err(errbuf, errlen, "expected %d columns at row %" PRIu64, expect_nf, row_ix + 1u);
      da_csv_import_free_stub(r);
      free(line);
      fclose(fp);
      return NULL;
    }

    char *endp = NULL;
    uint32_t attr = 0;
    uint64_t sz = 0;
    uint64_t alc = 0;
    uint64_t mt = 0;
    uint32_t w32 = 0;
    const char *path_col = cols[0];

    errno = 0;
    sz = strtoull(cols[1], &endp, 10);
    if (endp == cols[1] || errno != 0) {
      da_csv_set_err(errbuf, errlen, "bad Size at row %" PRIu64, row_ix + 1u);
      da_csv_import_free_stub(r);
      free(line);
      fclose(fp);
      return NULL;
    }
    errno = 0;
    alc = strtoull(cols[2], &endp, 10);
    if (endp == cols[2] || errno != 0) {
      da_csv_set_err(errbuf, errlen, "bad Allocated at row %" PRIu64, row_ix + 1u);
      da_csv_import_free_stub(r);
      free(line);
      fclose(fp);
      return NULL;
    }
    if (da_parse_mtime_field_to_ns(cols[3], &mt) != 0) {
      da_csv_set_err(errbuf, errlen,
                     "bad Modified at row %" PRIu64 " (use YYYY-MM-DD HH:MM:SS, same as export)", row_ix + 1u);
      da_csv_import_free_stub(r);
      free(line);
      fclose(fp);
      return NULL;
    }
    errno = 0;
    unsigned long waul = strtoul(cols[4], &endp, 10);
    if (endp == cols[4] || errno != 0 || waul > 0xffffffffu) {
      da_csv_set_err(errbuf, errlen, "bad Attributes at row %" PRIu64, row_ix + 1u);
      da_csv_import_free_stub(r);
      free(line);
      fclose(fp);
      return NULL;
    }
    w32 = (uint32_t)waul;
    attr = da_kind_from_win32_attrs(w32);

    if (path_col[0] == '\0') {
      da_csv_set_err(errbuf, errlen, "empty path at row %" PRIu64, row_ix + 1u);
      da_csv_import_free_stub(r);
      free(line);
      fclose(fp);
      return NULL;
    }

    if (!diskatlas_nodes_ensure_capacity(r)) {
      da_csv_set_err(errbuf, errlen, "out of memory (nodes)");
      da_csv_import_free_stub(r);
      free(line);
      fclose(fp);
      return NULL;
    }
    size_t anchor = 0;
    if (!da_path_blob_append_utf8(r, path_col, &anchor)) {
      da_csv_set_err(errbuf, errlen, "out of memory (path blob)");
      da_csv_import_free_stub(r);
      free(line);
      fclose(fp);
      return NULL;
    }

    file_node_t node;
    memset(&node, 0, sizeof(node));
    node.struct_version = DISKATLAS_FILE_NODE_STRUCT_VERSION;
    node.attributes = attr;
    node.size_bytes = sz;
    node.allocated_bytes = alc;
    node.mtime_unix_ns = mt;
    node.duplicate_group_id = 0;
    node.win32_attributes = w32;
    node.path = NULL;

    r->path_offs[r->node_count] = anchor;
    r->nodes[r->node_count] = node;
    r->node_count++;
    row_ix++;
  }

  free(line);
  fclose(fp);

  diskatlas_finalize_paths(r);

  if (diskatlas_dup_materialize_tables(r) != 0) {
    da_csv_set_err(errbuf, errlen, "failed to build duplicate group index");
    da_csv_import_free_stub(r);
    return NULL;
  }

  uint64_t folders = 0;
  uint64_t files = 0;
  uint64_t bytes_acc = 0;
  for (size_t i = 0; i < r->node_count; i++) {
    uint32_t k = r->nodes[i].attributes & DISKATLAS_NODE_KIND_MASK;
    if (k == DISKATLAS_NODE_KIND_DIR) {
      folders++;
    } else if (k == DISKATLAS_NODE_KIND_FILE) {
      files++;
    }
    bytes_acc += r->nodes[i].size_bytes;
  }
  atomic_store_explicit(&r->folders_recorded, (uint_least64_t)folders, memory_order_relaxed);
  atomic_store_explicit(&r->files_recorded, (uint_least64_t)files, memory_order_relaxed);
  atomic_store_explicit(&r->bytes_accounted, (uint_least64_t)bytes_acc, memory_order_relaxed);
  atomic_store_explicit(&r->entry_visits, (uint_least64_t)r->node_count, memory_order_relaxed);
  atomic_store_explicit(&r->complete, 1u, memory_order_release);

  return (scan_result_t *)r;
}
