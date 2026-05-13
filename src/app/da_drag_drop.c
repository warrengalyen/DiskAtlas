#include <glib.h>
#include <gdk/gdk.h>
#include <gtk/gtk.h>
#ifdef G_OS_WIN32
#include <windows.h>
#include <shlobj.h>
#include <ole2.h>
#endif

#include "da_drag_drop.h"
#include "scan_controller.h"

/* ── per-tree drag context ─────────────────────────────────────────────── */

#define DA_DND_CTX_KEY "da-dnd-ctx"

typedef struct {
  AppState    *app;
  gboolean     suppress;      /* drag started outside column 0 */
  GtkTreePath *pressed_row;   /* row under cursor at button-press */
#ifdef G_OS_WIN32
  gint         press_x;
  gint         press_y;
  gboolean     is_pressing;
#else
  GPtrArray   *drag_paths;    /* owned gchar* strings */
#endif
} DaDndCtx;

static DaDndCtx *da_dnd_ctx_new(AppState *app) {
  DaDndCtx *ctx = g_new0(DaDndCtx, 1);
  ctx->app = app;
  return ctx;
}

static void da_dnd_ctx_free(gpointer data) {
  DaDndCtx *ctx = (DaDndCtx *)data;
  if (!ctx) return;
#ifndef G_OS_WIN32
  if (ctx->drag_paths) { g_ptr_array_unref(ctx->drag_paths); ctx->drag_paths = NULL; }
#endif
  if (ctx->pressed_row) { gtk_tree_path_free(ctx->pressed_row); ctx->pressed_row = NULL; }
  g_free(ctx);
}

/* ── platform-specific helpers ─────────────────────────────────────────── */

#ifdef G_OS_WIN32

/* Build a CF_HDROP HGLOBAL (wide-char, DROPFILES header) from UTF-8 paths.
 * The caller (IDataObject::Release) owns and frees the returned handle. */
static HGLOBAL da_paths_to_hglobal(GPtrArray *paths) {
  if (!paths || paths->len == 0) return NULL;
  /* First pass: measure required buffer. */
  gsize wb = 0;
  for (guint i = 0; i < paths->len; i++) {
    const gchar *p = (const gchar *)paths->pdata[i];
    if (!p || !p[0]) continue;
    glong wl = 0;
    gunichar2 *wp = g_utf8_to_utf16(p, -1, NULL, &wl, NULL);
    if (wp) { wb += (gsize)(wl + 1) * sizeof(gunichar2); g_free(wp); }
  }
  if (!wb) return NULL;
  gsize total = sizeof(DROPFILES) + wb + sizeof(gunichar2); /* extra NUL terminator */
  HGLOBAL hg = GlobalAlloc(GHND, total);
  if (!hg) return NULL;
  DROPFILES *df   = (DROPFILES *)GlobalLock(hg);
  df->pFiles      = sizeof(DROPFILES);
  df->pt.x        = df->pt.y = 0;
  df->fNC         = FALSE;
  df->fWide       = TRUE;
  gunichar2 *dest = (gunichar2 *)((guchar *)df + sizeof(DROPFILES));
  for (guint i = 0; i < paths->len; i++) {
    const gchar *p = (const gchar *)paths->pdata[i];
    if (!p || !p[0]) continue;
    glong wl = 0;
    gunichar2 *wp = g_utf8_to_utf16(p, -1, NULL, &wl, NULL);
    if (wp) {
      memcpy(dest, wp, (gsize)(wl + 1) * sizeof(gunichar2));
      dest += wl + 1;
      g_free(wp);
    }
  }
  GlobalUnlock(hg);
  return hg;
}

/* ── Minimal Win32 COM IEnumFORMATETC ──────────────────────────────────── */

typedef struct { IEnumFORMATETCVtbl *lpVtbl; LONG ref; int pos; } DaEnumFmtEtc;

static HRESULT STDMETHODCALLTYPE efmt_QI(IEnumFORMATETC *t, REFIID r, void **pp) {
  if (IsEqualIID(r, &IID_IUnknown) || IsEqualIID(r, &IID_IEnumFORMATETC)) {
    *pp = t; t->lpVtbl->AddRef(t); return S_OK;
  }
  *pp = NULL; return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE efmt_AddRef(IEnumFORMATETC *t) {
  return (ULONG)InterlockedIncrement(&((DaEnumFmtEtc *)t)->ref);
}
static ULONG STDMETHODCALLTYPE efmt_Release(IEnumFORMATETC *t) {
  ULONG r = (ULONG)InterlockedDecrement(&((DaEnumFmtEtc *)t)->ref);
  if (!r) CoTaskMemFree(t);
  return r;
}
static HRESULT STDMETHODCALLTYPE efmt_Next(IEnumFORMATETC *t, ULONG n, FORMATETC *el, ULONG *pc) {
  DaEnumFmtEtc *e = (DaEnumFmtEtc *)t; ULONG f = 0;
  while (f < n && e->pos < 1) {
    el[f].cfFormat = CF_HDROP; el[f].ptd = NULL;
    el[f].dwAspect = DVASPECT_CONTENT; el[f].lindex = -1; el[f].tymed = TYMED_HGLOBAL;
    f++; e->pos++;
  }
  if (pc) *pc = f;
  return f == n ? S_OK : S_FALSE;
}
static HRESULT STDMETHODCALLTYPE efmt_Skip(IEnumFORMATETC *t, ULONG n) {
  DaEnumFmtEtc *e = (DaEnumFmtEtc *)t; e->pos += (int)n;
  return e->pos <= 1 ? S_OK : S_FALSE;
}
static HRESULT STDMETHODCALLTYPE efmt_Reset(IEnumFORMATETC *t) {
  ((DaEnumFmtEtc *)t)->pos = 0; return S_OK;
}
static HRESULT STDMETHODCALLTYPE efmt_Clone(IEnumFORMATETC *t, IEnumFORMATETC **pp) {
  DaEnumFmtEtc *s = (DaEnumFmtEtc *)t;
  DaEnumFmtEtc *c = (DaEnumFmtEtc *)CoTaskMemAlloc(sizeof(*c));
  if (!c) return E_OUTOFMEMORY;
  *c = *s; c->ref = 1; *pp = (IEnumFORMATETC *)c; return S_OK;
}
static IEnumFORMATETCVtbl g_efmt_vtbl = {
  efmt_QI, efmt_AddRef, efmt_Release, efmt_Next, efmt_Skip, efmt_Reset, efmt_Clone
};

/* ── Minimal Win32 COM IDataObject (CF_HDROP only) ─────────────────────── */

typedef struct {
  IDataObjectVtbl *lpVtbl;
  LONG             ref;
  HGLOBAL          hDropFiles; /* freed in Release */
} DaDataObject;

static HRESULT STDMETHODCALLTYPE dobj_QI(IDataObject *t, REFIID r, void **pp) {
  if (IsEqualIID(r, &IID_IUnknown) || IsEqualIID(r, &IID_IDataObject)) {
    *pp = t; t->lpVtbl->AddRef(t); return S_OK;
  }
  *pp = NULL; return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE dobj_AddRef(IDataObject *t) {
  return (ULONG)InterlockedIncrement(&((DaDataObject *)t)->ref);
}
static ULONG STDMETHODCALLTYPE dobj_Release(IDataObject *t) {
  DaDataObject *o = (DaDataObject *)t;
  ULONG r = (ULONG)InterlockedDecrement(&o->ref);
  if (!r) { if (o->hDropFiles) GlobalFree(o->hDropFiles); CoTaskMemFree(o); }
  return r;
}
static HRESULT STDMETHODCALLTYPE dobj_GetData(IDataObject *t, FORMATETC *fe, STGMEDIUM *sm) {
  DaDataObject *o = (DaDataObject *)t;
  if (fe->cfFormat == CF_HDROP && (fe->tymed & TYMED_HGLOBAL)) {
    /* Return a COPY of the HGLOBAL — OLE takes ownership and frees it after
     * cross-process marshaling.  Returning the original handle causes
     * RPC_S_CALL_FAILED on the actual IDropTarget::Drop transfer. */
    SIZE_T sz     = GlobalSize(o->hDropFiles);
    HGLOBAL hcopy = GlobalAlloc(GHND, sz);
    if (!hcopy) return E_OUTOFMEMORY;
    LPVOID src = GlobalLock(o->hDropFiles);
    LPVOID dst = GlobalLock(hcopy);
    if (src && dst) memcpy(dst, src, sz);
    GlobalUnlock(o->hDropFiles);
    GlobalUnlock(hcopy);
    sm->tymed = TYMED_HGLOBAL; sm->hGlobal = hcopy; sm->pUnkForRelease = NULL;
    return S_OK;
  }
  return DV_E_FORMATETC;
}
static HRESULT STDMETHODCALLTYPE dobj_GetDataHere(IDataObject *t, FORMATETC *fe, STGMEDIUM *sm) {
  (void)t; (void)fe; (void)sm; return DATA_E_FORMATETC;
}
static HRESULT STDMETHODCALLTYPE dobj_QueryGetData(IDataObject *t, FORMATETC *fe) {
  (void)t;
  return (fe->cfFormat == CF_HDROP && (fe->tymed & TYMED_HGLOBAL)) ? S_OK : DV_E_FORMATETC;
}
static HRESULT STDMETHODCALLTYPE dobj_GetCanonical(IDataObject *t, FORMATETC *i, FORMATETC *o) {
  (void)t; (void)i; o->ptd = NULL; return E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE dobj_SetData(IDataObject *t, FORMATETC *fe, STGMEDIUM *sm, BOOL fr) {
  (void)t; (void)fe; (void)sm; (void)fr; return E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE dobj_EnumFmtEtc(IDataObject *t, DWORD dir, IEnumFORMATETC **pp) {
  (void)t;
  if (dir != DATADIR_GET) return E_NOTIMPL;
  DaEnumFmtEtc *e = (DaEnumFmtEtc *)CoTaskMemAlloc(sizeof(*e));
  if (!e) return E_OUTOFMEMORY;
  e->lpVtbl = &g_efmt_vtbl; e->ref = 1; e->pos = 0;
  *pp = (IEnumFORMATETC *)e; return S_OK;
}
static HRESULT STDMETHODCALLTYPE dobj_DAdvise(IDataObject *t, FORMATETC *fe, DWORD a,
                                              IAdviseSink *s, DWORD *d) {
  (void)t; (void)fe; (void)a; (void)s; (void)d; return OLE_E_ADVISENOTSUPPORTED;
}
static HRESULT STDMETHODCALLTYPE dobj_DUnadvise(IDataObject *t, DWORD c) {
  (void)t; (void)c; return OLE_E_ADVISENOTSUPPORTED;
}
static HRESULT STDMETHODCALLTYPE dobj_EnumDAdvise(IDataObject *t, IEnumSTATDATA **pp) {
  (void)t; (void)pp; return OLE_E_ADVISENOTSUPPORTED;
}
static IDataObjectVtbl g_dobj_vtbl = {
  dobj_QI, dobj_AddRef, dobj_Release,
  dobj_GetData, dobj_GetDataHere, dobj_QueryGetData, dobj_GetCanonical,
  dobj_SetData, dobj_EnumFmtEtc, dobj_DAdvise, dobj_DUnadvise, dobj_EnumDAdvise
};

/* ── Minimal Win32 COM IDropSource ─────────────────────────────────────── */

typedef struct { IDropSourceVtbl *lpVtbl; LONG ref; } DaDropSource;

static HRESULT STDMETHODCALLTYPE dsrc_QI(IDropSource *t, REFIID r, void **pp) {
  if (IsEqualIID(r, &IID_IUnknown) || IsEqualIID(r, &IID_IDropSource)) {
    *pp = t; t->lpVtbl->AddRef(t); return S_OK;
  }
  *pp = NULL; return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE dsrc_AddRef(IDropSource *t) {
  return (ULONG)InterlockedIncrement(&((DaDropSource *)t)->ref);
}
static ULONG STDMETHODCALLTYPE dsrc_Release(IDropSource *t) {
  ULONG r = (ULONG)InterlockedDecrement(&((DaDropSource *)t)->ref);
  if (!r) CoTaskMemFree(t);
  return r;
}
static HRESULT STDMETHODCALLTYPE dsrc_QueryContinueDrag(IDropSource *t, BOOL esc, DWORD ks) {
  (void)t;
  if (esc)                 return DRAGDROP_S_CANCEL;
  if (!(ks & MK_LBUTTON)) return DRAGDROP_S_DROP;
  return S_OK;
}
static HRESULT STDMETHODCALLTYPE dsrc_GiveFeedback(IDropSource *t, DWORD ef) {
  (void)t; (void)ef; return DRAGDROP_S_USEDEFAULTCURSORS;
}
static IDropSourceVtbl g_dsrc_vtbl = {
  dsrc_QI, dsrc_AddRef, dsrc_Release, dsrc_QueryContinueDrag, dsrc_GiveFeedback
};

/* Execute a Win32 OLE2 DoDragDrop for the given file paths.
 * Returns the negotiated DROPEFFECT_* or DROPEFFECT_NONE on failure/cancel.
 *
 * Key design notes:
 * - OleInitialize is called here because GTK3/MSYS2 only initialises OLE
 *   lazily when its own drag starts; we bypass that path entirely.
 * - IDataObject::GetData returns a COPY of the HGLOBAL so OLE's cross-process
 *   marshaling layer can take ownership without invalidating our master copy. */
static DWORD da_win32_do_drag(GPtrArray *paths, gboolean prefer_move) {
  HGLOBAL hg = da_paths_to_hglobal(paths);
  if (!hg) return DROPEFFECT_NONE;

  DaDataObject *dobj = (DaDataObject *)CoTaskMemAlloc(sizeof(*dobj));
  if (!dobj) { GlobalFree(hg); return DROPEFFECT_NONE; }
  dobj->lpVtbl = &g_dobj_vtbl; dobj->ref = 1; dobj->hDropFiles = hg;

  DaDropSource *dsrc = (DaDropSource *)CoTaskMemAlloc(sizeof(*dsrc));
  if (!dsrc) {
    dobj->lpVtbl->Release((IDataObject *)dobj);
    return DROPEFFECT_NONE;
  }
  dsrc->lpVtbl = &g_dsrc_vtbl; dsrc->ref = 1;

  /* Every OleInitialize call (S_OK or S_FALSE) needs a matching OleUninitialize. */
  OleInitialize(NULL);

  DWORD allowed = DROPEFFECT_COPY | DROPEFFECT_MOVE;
  DWORD effect  = prefer_move ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
  HRESULT hr = DoDragDrop((IDataObject *)dobj, (IDropSource *)dsrc, allowed, &effect);

  OleUninitialize();

  dsrc->lpVtbl->Release((IDropSource *)dsrc);
  dobj->lpVtbl->Release((IDataObject *)dobj);

  return (hr == DRAGDROP_S_DROP) ? effect : DROPEFFECT_NONE;
}

#else /* !G_OS_WIN32 */

/* Build a NULL-terminated gchar** of file:// URIs from a GPtrArray of paths.
 * Returns NULL if no valid URIs.  Caller must g_strfreev. */
static gchar **da_paths_to_uri_strv(GPtrArray *paths) {
  if (!paths || paths->len == 0) return NULL;
  GPtrArray *uris = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 0; i < paths->len; i++) {
    const gchar *p = (const gchar *)paths->pdata[i];
    if (!p || !p[0]) continue;
    gchar *uri = g_filename_to_uri(p, NULL, NULL);
    if (uri) g_ptr_array_add(uris, uri);
  }
  if (uris->len == 0) { g_ptr_array_free(uris, TRUE); return NULL; }
  g_ptr_array_add(uris, NULL);
  return (gchar **)g_ptr_array_free(uris, FALSE);
}

#endif /* G_OS_WIN32 */

/* ── signal handlers ─────────────────────────────────────────────────── */

static gboolean on_dnd_button_press(GtkWidget *widget, GdkEventButton *event,
                                    gpointer user_data) {
  DaDndCtx *ctx = (DaDndCtx *)user_data;
  if (!ctx || event->button != 1) return FALSE;

  if (ctx->pressed_row) { gtk_tree_path_free(ctx->pressed_row); ctx->pressed_row = NULL; }

  GtkTreeView       *tv          = GTK_TREE_VIEW(widget);
  GtkTreePath       *path        = NULL;
  GtkTreeViewColumn *clicked_col = NULL;
  gtk_tree_view_get_path_at_pos(tv, (gint)event->x, (gint)event->y,
                                &path, &clicked_col, NULL, NULL);
  ctx->pressed_row = path;
  GtkTreeViewColumn *col0 = gtk_tree_view_get_column(tv, 0);
  ctx->suppress = (clicked_col == NULL || clicked_col != col0);

#ifdef G_OS_WIN32
  ctx->press_x     = (gint)event->x;
  ctx->press_y     = (gint)event->y;
  ctx->is_pressing = TRUE;
#endif
  return FALSE;
}

#ifdef G_OS_WIN32

static gboolean on_dnd_motion_notify(GtkWidget *widget, GdkEventMotion *event,
                                     gpointer user_data) {
  DaDndCtx *ctx = (DaDndCtx *)user_data;
  if (!ctx->is_pressing || ctx->suppress) return FALSE;
  if (!gtk_drag_check_threshold(widget, ctx->press_x, ctx->press_y,
                                (gint)event->x, (gint)event->y))
    return FALSE;

  ctx->is_pressing = FALSE;

  /* Collect dragged paths from the GTK selection. */
  GPtrArray *paths = scan_controller_collect_selected_utf8_paths(ctx->app);

  /* Fallback: read path directly from the model row that was pressed. */
  if ((!paths || paths->len == 0) && ctx->pressed_row) {
    GtkTreeView  *tv    = GTK_TREE_VIEW(widget);
    GtkTreeModel *model = gtk_tree_view_get_model(tv);
    GtkTreeIter   iter;
    if (model && gtk_tree_model_get_iter(model, &iter, ctx->pressed_row)) {
      gchar *p = NULL;
      gtk_tree_model_get(model, &iter, 1, &p, -1);
      if (p && p[0]) {
        if (!paths) paths = g_ptr_array_new_with_free_func(g_free);
        g_ptr_array_add(paths, p);
      } else {
        g_free(p);
      }
    }
  }

  if (!paths || paths->len == 0) {
    if (paths) g_ptr_array_unref(paths);
    return FALSE;
  }

  gboolean prefer_move = !(event->state & GDK_CONTROL_MASK);
  DWORD effect = da_win32_do_drag(paths, prefer_move);

  if (effect == DROPEFFECT_MOVE) {
    for (guint i = 0; i < paths->len; i++) {
      const gchar *p = (const gchar *)paths->pdata[i];
      if (p && p[0]) scan_controller_mark_path_deleted(ctx->app, p);
    }
  }

  g_ptr_array_unref(paths);
  return TRUE; /* consume the event so GTK doesn't start its own drag */
}

static gboolean on_dnd_button_release(GtkWidget *widget, GdkEventButton *event,
                                      gpointer user_data) {
  (void)widget; (void)event;
  ((DaDndCtx *)user_data)->is_pressing = FALSE;
  return FALSE;
}

#else /* !G_OS_WIN32 */

static void on_dnd_drag_begin(GtkWidget *widget, GdkDragContext *context,
                               gpointer user_data) {
  DaDndCtx *ctx = (DaDndCtx *)user_data;
  if (!ctx) return;
  if (ctx->drag_paths) { g_ptr_array_unref(ctx->drag_paths); ctx->drag_paths = NULL; }

  if (ctx->suppress) { gtk_drag_set_icon_default(context); return; }

  ctx->drag_paths = scan_controller_collect_selected_utf8_paths(ctx->app);

  /* Fallback for stale GTK selection at drag-begin time. */
  if ((!ctx->drag_paths || ctx->drag_paths->len == 0) && ctx->pressed_row) {
    GtkTreeView  *tv2   = GTK_TREE_VIEW(widget);
    GtkTreeModel *model = gtk_tree_view_get_model(tv2);
    GtkTreeIter   iter;
    if (model && gtk_tree_model_get_iter(model, &iter, ctx->pressed_row)) {
      gchar *p = NULL;
      gtk_tree_model_get(model, &iter, 1, &p, -1);
      if (p && p[0]) {
        if (!ctx->drag_paths) ctx->drag_paths = g_ptr_array_new_with_free_func(g_free);
        g_ptr_array_add(ctx->drag_paths, p);
      } else { g_free(p); }
    }
  }

  if (!ctx->drag_paths || ctx->drag_paths->len == 0) {
    ctx->suppress = TRUE; gtk_drag_set_icon_default(context); return;
  }
  gtk_drag_set_icon_default(context);
}

static void on_dnd_drag_data_get(GtkWidget *widget, GdkDragContext *context,
                                  GtkSelectionData *data, guint info,
                                  guint time, gpointer user_data) {
  (void)widget; (void)context; (void)info; (void)time;
  DaDndCtx *ctx = (DaDndCtx *)user_data;
  if (!ctx || ctx->suppress || !ctx->drag_paths) return;
  gchar **uris = da_paths_to_uri_strv(ctx->drag_paths);
  if (uris) { gtk_selection_data_set_uris(data, uris); g_strfreev(uris); }
}

static void on_dnd_drag_end(GtkWidget *widget, GdkDragContext *context,
                             gpointer user_data) {
  (void)widget;
  DaDndCtx *ctx = (DaDndCtx *)user_data;
  if (!ctx || ctx->suppress || !ctx->drag_paths) goto cleanup;
  if (gdk_drag_context_get_selected_action(context) == GDK_ACTION_MOVE) {
    for (guint i = 0; i < ctx->drag_paths->len; i++) {
      const gchar *p = (const gchar *)ctx->drag_paths->pdata[i];
      if (p && p[0]) scan_controller_mark_path_deleted(ctx->app, p);
    }
  }
cleanup:
  if (ctx->drag_paths) { g_ptr_array_unref(ctx->drag_paths); ctx->drag_paths = NULL; }
  ctx->suppress = FALSE;
}

#endif /* G_OS_WIN32 */

/* ── per-tree setup ───────────────────────────────────────────────────── */

static void da_drag_drop_setup_tree(AppState *app, GtkTreeView *tv) {
  if (!tv || !GTK_IS_TREE_VIEW(tv)) return;

  gtk_drag_source_unset(GTK_WIDGET(tv));
  DaDndCtx *ctx = (DaDndCtx *)g_object_get_data(G_OBJECT(tv), DA_DND_CTX_KEY);
  if (ctx) {
    g_signal_handlers_disconnect_by_data(tv, ctx);
    g_object_set_data(G_OBJECT(tv), DA_DND_CTX_KEY, NULL);
  }
  if (!app->general_enable_drag_drop) return;

  ctx = da_dnd_ctx_new(app);
  g_object_set_data_full(G_OBJECT(tv), DA_DND_CTX_KEY, ctx, da_dnd_ctx_free);

#ifdef G_OS_WIN32
  /* On Windows we bypass GTK's OLE2 DnD entirely and use Win32 DoDragDrop
   * directly.  GTK3's OLE2 IDataObject does not enumerate CF_HDROP=15 when
   * targets are set via gtk_drag_source_set / gtk_target_list_add, so
   * Explorer's IDropTarget::DragEnter always returns DROPEFFECT_NONE.
   * Instead, we detect the drag threshold in motion-notify and invoke
   * a hand-rolled IDataObject/IDropSource that properly provides CF_HDROP. */
  gtk_widget_add_events(GTK_WIDGET(tv),
                        GDK_POINTER_MOTION_MASK | GDK_BUTTON_RELEASE_MASK);
  g_signal_connect(tv, "button-press-event",   G_CALLBACK(on_dnd_button_press),   ctx);
  g_signal_connect(tv, "motion-notify-event",  G_CALLBACK(on_dnd_motion_notify),  ctx);
  g_signal_connect(tv, "button-release-event", G_CALLBACK(on_dnd_button_release), ctx);
#else
  gtk_drag_source_set(GTK_WIDGET(tv), GDK_BUTTON1_MASK, NULL, 0,
                      GDK_ACTION_MOVE | GDK_ACTION_COPY);
  gtk_drag_source_add_uri_targets(GTK_WIDGET(tv));
  g_signal_connect(tv, "button-press-event", G_CALLBACK(on_dnd_button_press),   ctx);
  g_signal_connect(tv, "drag-begin",         G_CALLBACK(on_dnd_drag_begin),     ctx);
  g_signal_connect(tv, "drag-data-get",      G_CALLBACK(on_dnd_drag_data_get),  ctx);
  g_signal_connect(tv, "drag-end",           G_CALLBACK(on_dnd_drag_end),       ctx);
#endif
}

/* ── public API ────────────────────────────────────────────────────────── */

void da_drag_drop_setup(AppState *app) {
  if (!app) return;
  da_drag_drop_setup_tree(app, GTK_TREE_VIEW(app->tree));
  da_drag_drop_setup_tree(app, GTK_TREE_VIEW(app->tree_view));
}
