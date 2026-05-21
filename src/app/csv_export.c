#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <glib/gstdio.h>

#include "diskatlas.h"
#include "csv_export.h"
#include "file_type_view.h"
#include "format_text.h"
#include "volumes.h"

#define DA_CSV_FILE_TYPES_HEADER "Extension,File Type,Percent,Size,Allocated,Files"

/** RFC4180-style UTF-8 field (same rules as diskatlas_csv_export.c). */
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

  diskatlas_csv_export_options_t opt;
  memset(&opt, 0, sizeof(opt));
  opt.struct_version = DISKATLAS_CSV_EXPORT_OPTIONS_STRUCT_VERSION;
  opt.drive_capacity_bytes = tot;
  opt.free_space_bytes = free_b;
  opt.used_space_bytes = used_b;
  opt.reserved_space_bytes = reserved;
  if (include_reserved_space_column) {
    opt.flags = DISKATLAS_CSV_EXPORT_INCLUDE_RESERVED;
  }

  return diskatlas_scan_export_csv(app->scan, utf8_path, &opt, errbuf, errlen);
}

int da_export_file_types_csv(AppState *app, const char *utf8_path, char *errbuf, size_t errlen) {
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
  if (app->file_type_tree == NULL) {
    if (errbuf != NULL && errlen > 0) {
      (void)snprintf(errbuf, errlen, "file types view not available");
    }
    return -1;
  }
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(app->file_type_tree));
  if (model == NULL) {
    if (errbuf != NULL && errlen > 0) {
      (void)snprintf(errbuf, errlen, "no file types data");
    }
    return -1;
  }

  FILE *out = g_fopen(utf8_path, "wb");
  if (out == NULL) {
    if (errbuf != NULL && errlen > 0) {
      (void)snprintf(errbuf, errlen, "cannot open for write: %s", strerror(errno));
    }
    return -1;
  }

  enum { IOBUF = 256 * 1024 };
  char stackbuf[IOBUF];
  setvbuf(out, stackbuf, _IOFBF, sizeof stackbuf);

  if (fprintf(out, "%s\n", DA_CSV_FILE_TYPES_HEADER) < 0) {
    goto write_fail;
  }

  GtkTreeIter iter;
  gboolean valid = gtk_tree_model_get_iter_first(model, &iter);
  while (valid) {
    gchar *ext = NULL, *ftype = NULL;
    gint pct_val = 0;
    guint64 size_raw = 0, alloc_raw = 0, files_raw = 0;
    gtk_tree_model_get(model, &iter, DA_FT_COL_EXT, &ext, DA_FT_COL_TYPE, &ftype, DA_FT_COL_PCT_VAL, &pct_val,
                       DA_FT_COL_SIZE_RAW, &size_raw, DA_FT_COL_ALLOC_RAW, &alloc_raw, DA_FT_COL_FILES_RAW,
                       &files_raw, -1);

    fprint_csv_utf8_field(out, ext);
    if (fputc(',', out) < 0) {
      goto row_fail;
    }
    fprint_csv_utf8_field(out, ftype);
    if (fputc(',', out) < 0) {
      goto row_fail;
    }
    if (fprintf(out, "%d,%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n", pct_val, (uint64_t)size_raw,
                 (uint64_t)alloc_raw, (uint64_t)files_raw) < 0) {
      goto row_fail;
    }

    g_free(ext);
    g_free(ftype);
    valid = gtk_tree_model_iter_next(model, &iter);
    continue;

  row_fail:
    g_free(ext);
    g_free(ftype);
    goto write_fail;
  }

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
  fclose(out);
  return -1;
}
