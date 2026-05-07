#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <glib/gstdio.h>

#include "diskatlas.h"
#include "csv_export.h"
#include "format_text.h"
#include "volumes.h"

#define DA_CSV_UI_HEADER_BASE "File Name,Size,Allocated,Modified,Attributes,Files,Folders,DRIVECAPACITY,FREESPACE,USEDSPACE"

typedef struct {
  uint64_t subtree_files;
  uint64_t subtree_folders;
} DirAgg;

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

/** Walk from first_parent_dir up to root; bump subtree_files or subtree_folders on each dir key found. */
static void bump_dir_chain(GHashTable *dir_stats, const char *first_parent_dir, gboolean bump_files) {
  if (first_parent_dir == NULL || first_parent_dir[0] == '\0') {
    return;
  }
  gchar *cur = g_strdup(first_parent_dir);
  for (;;) {
    DirAgg *a = (DirAgg *)g_hash_table_lookup(dir_stats, cur);
    if (a != NULL) {
      if (bump_files) {
        a->subtree_files++;
      } else {
        a->subtree_folders++;
      }
    }
    gchar *par = g_path_get_dirname(cur);
    if (strcmp(par, cur) == 0) {
      g_free(par);
      g_free(cur);
      break;
    }
    g_free(cur);
    cur = par;
  }
}

int da_export_scan_csv(AppState *app, const char *utf8_path, gboolean include_reserved_space_column, char *errbuf,
                       size_t errlen) {
  if (errbuf != NULL && errlen > 0) {
    errbuf[0] = '\0';
  }
  if (app == NULL || utf8_path == NULL || utf8_path[0] == '\0') {
    if (errbuf != NULL && errlen > 0) {
      (void)snprintf(errbuf, errlen, "invalid arguments");
    }
    return -1;
  }
  if (app->scan == NULL) {
    if (errbuf != NULL && errlen > 0) {
      (void)snprintf(errbuf, errlen, "no scan data");
    }
    return -1;
  }
  scan_progress_t pr = scan_get_progress(app->scan);
  if (!pr.is_complete) {
    if (errbuf != NULL && errlen > 0) {
      (void)snprintf(errbuf, errlen, "scan is not complete");
    }
    return -1;
  }

  scan_results_view_t v = scan_get_results(app->scan);
  if (v.nodes == NULL) {
    if (errbuf != NULL && errlen > 0) {
      (void)snprintf(errbuf, errlen, "no scan nodes");
    }
    return -1;
  }

  uint64_t tot = 0, free_b = 0, used_b = 0;
  const char *vol_key = "";
  if (app->scan_root_utf8 != NULL && app->scan_root_utf8[0] != '\0') {
    vol_key = app->scan_root_utf8;
  } else if (app->csv_derived_root_utf8 != NULL && app->csv_derived_root_utf8[0] != '\0') {
    vol_key = app->csv_derived_root_utf8;
  } else if (app->csv_import_active && app->csv_import_path != NULL && app->csv_import_path[0] != '\0') {
    vol_key = app->csv_import_path;
  }
  (void)da_volume_space_for_path(vol_key, &tot, &free_b, &used_b);

  uint64_t reserved = 0;
  if (tot > used_b + free_b) {
    reserved = tot - used_b - free_b;
  }

  GHashTable *dir_stats = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  for (size_t i = 0; i < v.count; i++) {
    uint32_t kind = v.nodes[i].attributes & DISKATLAS_NODE_KIND_MASK;
    if (kind != DISKATLAS_NODE_KIND_DIR) {
      continue;
    }
    const char *p = v.nodes[i].path;
    if (p == NULL || p[0] == '\0') {
      continue;
    }
    DirAgg *ag = g_new0(DirAgg, 1);
    g_hash_table_insert(dir_stats, g_strdup(p), ag);
  }

  for (size_t i = 0; i < v.count; i++) {
    const file_node_t *n = &v.nodes[i];
    uint32_t kind = n->attributes & DISKATLAS_NODE_KIND_MASK;
    if (kind == DISKATLAS_NODE_KIND_FILE && n->path != NULL) {
      gchar *par = g_path_get_dirname(n->path);
      bump_dir_chain(dir_stats, par, TRUE);
      g_free(par);
    }
  }
  for (size_t i = 0; i < v.count; i++) {
    const file_node_t *n = &v.nodes[i];
    uint32_t kind = n->attributes & DISKATLAS_NODE_KIND_MASK;
    if (kind == DISKATLAS_NODE_KIND_DIR && n->path != NULL) {
      gchar *par = g_path_get_dirname(n->path);
      if (strcmp(par, n->path) != 0) {
        bump_dir_chain(dir_stats, par, FALSE);
      }
      g_free(par);
    }
  }

  FILE *out = g_fopen(utf8_path, "wb");
  if (out == NULL) {
    if (errbuf != NULL && errlen > 0) {
      (void)snprintf(errbuf, errlen, "cannot open for write: %s", strerror(errno));
    }
    g_hash_table_destroy(dir_stats);
    return -1;
  }

  enum { IOBUF = 256 * 1024 };
  char stackbuf[IOBUF];
  setvbuf(out, stackbuf, _IOFBF, sizeof stackbuf);

  if (include_reserved_space_column) {
    if (fprintf(out, "%s,RESERVEDSPACE\n", DA_CSV_UI_HEADER_BASE) < 0) {
      goto write_fail;
    }
  } else {
    if (fprintf(out, "%s\n", DA_CSV_UI_HEADER_BASE) < 0) {
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
      DirAgg *a = (DirAgg *)g_hash_table_lookup(dir_stats, path);
      if (a != NULL) {
        sub_files = a->subtree_files;
        sub_folders = a->subtree_folders;
      }
    }

    da_format_mtime_local(n->mtime_unix_ns, mtime_buf, sizeof mtime_buf);

    fprint_csv_utf8_field(out, path);
    if (fprintf(out, ",%" PRIu64 ",%" PRIu64 ",%s,%" PRIu32 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
                    ",%" PRIu64,
                n->size_bytes, n->allocated_bytes, mtime_buf, n->win32_attributes, sub_files, sub_folders, tot,
                free_b, used_b) < 0) {
      goto write_fail;
    }
    if (include_reserved_space_column) {
      if (fprintf(out, ",%" PRIu64 "\n", reserved) < 0) {
        goto write_fail;
      }
    } else {
      if (fputc('\n', out) < 0) {
        goto write_fail;
      }
    }
  }

  g_hash_table_destroy(dir_stats);
  if (fclose(out) != 0) {
    if (errbuf != NULL && errlen > 0) {
      (void)snprintf(errbuf, errlen, "close error: %s", strerror(errno));
    }
    return -1;
  }
  return 0;

write_fail:
  if (errbuf != NULL && errlen > 0) {
    (void)snprintf(errbuf, errlen, "write error");
  }
  g_hash_table_destroy(dir_stats);
  fclose(out);
  return -1;
}
