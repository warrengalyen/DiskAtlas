#ifndef FILE_TYPE_VIEW_H
#define FILE_TYPE_VIEW_H

#include "app_state.h"

/* ---- GtkListStore column indices for the File Type stats treeview ---- */
#define DA_FT_COL_COLOR     0  /* G_TYPE_UINT   — packed 0xRRGGBBAA */
#define DA_FT_COL_EXT       1  /* G_TYPE_STRING — ".mp3", "Makefile" */
#define DA_FT_COL_TYPE      2  /* G_TYPE_STRING — MIME category name e.g. "Audio" */
#define DA_FT_COL_PCT_VAL   3  /* G_TYPE_INT    — 0-100 for progress bar */
#define DA_FT_COL_PCT_LBL   4  /* G_TYPE_STRING — "13.9 %" */
#define DA_FT_COL_SIZE      5  /* G_TYPE_STRING — formatted size */
#define DA_FT_COL_ALLOC     6  /* G_TYPE_STRING — formatted allocated */
#define DA_FT_COL_FILES     7  /* G_TYPE_STRING — formatted file count */
#define DA_FT_COL_SIZE_RAW  8  /* G_TYPE_UINT64 — raw size bytes (sort key) */
#define DA_FT_COL_ALLOC_RAW 9  /* G_TYPE_UINT64 — raw alloc bytes (sort key) */
#define DA_FT_COL_FILES_RAW 10 /* G_TYPE_UINT64 — raw file count (sort key) */
#define DA_FT_COL_COLOR_PB  11 /* G_TYPE_OBJECT — GdkPixbuf* rendered color swatch */
#define DA_FT_COL_ICON_PB   12 /* G_TYPE_OBJECT — GdkPixbuf* OS file-type icon */
#define DA_FT_N_COLS        13

/**
 * Create the GtkListStore, set it as the model on app->file_type_tree, and
 * configure all view columns (color swatch, Extension, File Type, Percent,
 * Size, Allocated, Files).  Must be called once after the widget is loaded.
 */
void da_file_type_view_setup(AppState *app);

/**
 * Aggregate all non-directory scan nodes by file extension (or basename for
 * extension-less files), compute per-group stats, and populate the list store.
 * No-op when no scan is loaded or app->file_type_tree is NULL.
 */
void da_file_type_view_populate(AppState *app);

/** Clear all rows from the file type list store. */
void da_file_type_view_clear(AppState *app);

#endif /* FILE_TYPE_VIEW_H */
