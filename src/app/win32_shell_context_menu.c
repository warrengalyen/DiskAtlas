#include "win32_shell_context_menu.h"
#include "scan_controller.h"

/* ---- GObject data keys --------------------------------------------------- */
#define DA_DYNAMIC_CTX_KEY  "da-dynamic-ctx"
#define DA_SHELL_CTX_KEY    "da-shell-ctx"
#define DA_SHELL_CMD_ID_KEY "da-shell-cmd-id"

/* ---- non-Win32 stubs ----------------------------------------------------- */

#if !defined(G_OS_WIN32)

void da_win32_remove_shell_menu_items(GtkMenu *menu) {
  (void)menu;
}

void da_win32_ctx_menu_refresh(AppState *app, GtkMenu *menu) {
  (void)app;
  (void)menu;
}

#else  /* G_OS_WIN32 */

/* Order matters: glib.h before windows.h to avoid min/max macro collisions. */
#include <glib.h>
#include <gdk/gdkwin32.h>

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <ole2.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>

/* Command ID range for QueryContextMenu. */
#define DA_CTX_CMD_FIRST 1u
#define DA_CTX_CMD_LAST  0x7FFFu

/* ---- internal types ------------------------------------------------------ */

typedef struct {
  IContextMenu  *cm;
  IContextMenu2 *cm2; /* QI'd from cm; used to trigger lazy submenu population. */
  HWND           hwnd;
  HMENU          hmenu; /* Must stay alive until after InvokeCommand returns. */
} DaShellCtxData;

static void da_shell_ctx_data_free(gpointer p) {
  DaShellCtxData *d = (DaShellCtxData *)p;
  if (d != NULL) {
    /* Destroy HMENU first — some shell extensions reference it during Release. */
    if (d->hmenu != NULL) {
      DestroyMenu(d->hmenu);
    }
    if (d->cm2 != NULL) {
      IContextMenu2_Release(d->cm2);
    }
    if (d->cm != NULL) {
      IContextMenu_Release(d->cm);
    }
    g_free(d);
  }
}

/* ---- helpers ------------------------------------------------------------- */

/**
 * Strip Win32 `&` accelerator characters from a wide string and return the
 * result as a UTF-8 string.  `&&` → literal `&`; `&X` → `X`.
 * Returns NULL on allocation failure; caller must g_free().
 */
static gchar *da_win32_strip_accelerators(const wchar_t *wstr) {
  if (wstr == NULL || wstr[0] == L'\0') {
    return NULL;
  }
  gchar *utf8 = g_utf16_to_utf8((const gunichar2 *)wstr, -1, NULL, NULL, NULL);
  if (utf8 == NULL) {
    return NULL;
  }
  GString *s = g_string_sized_new(strlen(utf8));
  for (const gchar *p = utf8; *p != '\0'; p++) {
    if (*p == '&') {
      p++;
      if (*p == '&') {
        g_string_append_c(s, '&');
      } else if (*p != '\0') {
        /* The character after & is the mnemonic — include it without the &. */
        g_string_append_c(s, *p);
      } else {
        break;
      }
    } else {
      g_string_append_c(s, *p);
    }
  }
  g_free(utf8);
  return g_string_free(s, FALSE);
}

/**
 * Convert an HBITMAP to a GdkPixbuf scaled to desired_w × desired_h.
 * Returns NULL if conversion fails or hbmp is a system pseudo-handle.
 * Caller owns the returned pixbuf.
 */
static GdkPixbuf *da_win32_hbitmap_to_pixbuf(HBITMAP hbmp, int desired_w, int desired_h) {
  if (hbmp == NULL) {
    return NULL;
  }
  /* Skip HBMMENU_* system pseudo-handles which are small integers. */
  if ((ULONG_PTR)hbmp <= 12u) {
    return NULL;
  }

  BITMAP bm;
  memset(&bm, 0, sizeof(bm));
  if (!GetObject(hbmp, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight == 0) {
    return NULL;
  }

  int w = bm.bmWidth;
  int h = abs(bm.bmHeight);

  BITMAPINFOHEADER bi;
  memset(&bi, 0, sizeof(bi));
  bi.biSize        = sizeof(bi);
  bi.biWidth       = w;
  bi.biHeight      = -h; /* top-down */
  bi.biPlanes      = 1;
  bi.biBitCount    = 32;
  bi.biCompression = BI_RGB;

  /* Use a DIB-section so we can read the pixels reliably for any bitmap type. */
  HDC hdc_screen = GetDC(NULL);
  if (hdc_screen == NULL) {
    return NULL;
  }
  HDC hdc_mem = CreateCompatibleDC(hdc_screen);
  ReleaseDC(NULL, hdc_screen);
  if (hdc_mem == NULL) {
    return NULL;
  }

  void *bits = NULL;
  BITMAPINFO bmi;
  memset(&bmi, 0, sizeof(bmi));
  bmi.bmiHeader = bi;
  HBITMAP hbmp_dib = CreateDIBSection(hdc_mem, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
  if (hbmp_dib == NULL || bits == NULL) {
    DeleteDC(hdc_mem);
    return NULL;
  }

  /* Create a source DC, select the original bitmap, and BitBlt into the DIB. */
  HDC hdc_src = CreateCompatibleDC(NULL);
  if (hdc_src == NULL) {
    DeleteObject(hbmp_dib);
    DeleteDC(hdc_mem);
    return NULL;
  }
  HGDIOBJ old_dst = SelectObject(hdc_mem, hbmp_dib);
  HGDIOBJ old_src = SelectObject(hdc_src, hbmp);
  BitBlt(hdc_mem, 0, 0, w, h, hdc_src, 0, 0, SRCCOPY);
  SelectObject(hdc_src, old_src);
  SelectObject(hdc_mem, old_dst);
  DeleteDC(hdc_src);
  DeleteDC(hdc_mem);

  /* Determine whether the bitmap carries meaningful alpha. */
  const guchar *src = (const guchar *)bits;
  gboolean has_alpha = FALSE;
  for (int i = 0; i < w * h; i++) {
    if (src[i * 4 + 3] != 0) {
      has_alpha = TRUE;
      break;
    }
  }

  /* Copy and convert BGRA → RGBA. */
  guchar *px = g_new(guchar, w * h * 4);
  for (int i = 0; i < w * h; i++) {
    px[i * 4 + 0] = src[i * 4 + 2]; /* R */
    px[i * 4 + 1] = src[i * 4 + 1]; /* G */
    px[i * 4 + 2] = src[i * 4 + 0]; /* B */
    px[i * 4 + 3] = has_alpha ? src[i * 4 + 3] : 0xFF;
  }

  DeleteObject(hbmp_dib);

  GdkPixbuf *pb = gdk_pixbuf_new_from_data(px, GDK_COLORSPACE_RGB, TRUE, 8,
                                            w, h, w * 4,
                                            (GdkPixbufDestroyNotify)g_free, px);
  if (pb == NULL) {
    g_free(px);
    return NULL;
  }

  /* Scale to the desired icon size if necessary. */
  if ((w != desired_w || h != desired_h) && desired_w > 0 && desired_h > 0) {
    GdkPixbuf *scaled = gdk_pixbuf_scale_simple(pb, desired_w, desired_h,
                                                GDK_INTERP_BILINEAR);
    g_object_unref(pb);
    pb = scaled;
  }
  return pb;
}

/* ---- invoke handler ------------------------------------------------------ */

static void on_shell_item_activate(GtkMenuItem *item, gpointer user_data) {
  GtkMenu *root_menu = GTK_MENU(user_data);
  DaShellCtxData *ctx =
      (DaShellCtxData *)g_object_get_data(G_OBJECT(root_menu), DA_SHELL_CTX_KEY);
  if (ctx == NULL || ctx->cm == NULL) {
    return;
  }
  UINT cmd_offset =
      (UINT)GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(item), DA_SHELL_CMD_ID_KEY));

  CMINVOKECOMMANDINFO ici;
  memset(&ici, 0, sizeof(ici));
  ici.cbSize = sizeof(ici);
  ici.hwnd   = ctx->hwnd; /* required for many shell extensions */
  ici.lpVerb = MAKEINTRESOURCEA(cmd_offset);
  ici.nShow  = SW_SHOWNORMAL;
  IContextMenu_InvokeCommand(ctx->cm, &ici);
}

/* ---- recursive menu builder ---------------------------------------------- */

/**
 * Populate @a gtk_shell from the Win32 @a hmenu, connecting activate handlers
 * back to @a root_menu (which carries the IContextMenu GObject data).
 * @a cm2 is used to trigger lazy submenu population via WM_INITMENUPOPUP.
 * @a mark_dynamic tags top-level items for later removal.
 */
static void da_win32_populate_gtk_menu_from_hmenu(GtkMenuShell *gtk_shell,
                                                  HMENU hmenu,
                                                  GtkMenu *root_menu,
                                                  IContextMenu2 *cm2,
                                                  gboolean mark_dynamic) {
  int count = GetMenuItemCount(hmenu);
  for (int idx = 0; idx < count; idx++) {
    MENUITEMINFOW mii;
    memset(&mii, 0, sizeof(mii));
    mii.cbSize = sizeof(mii);
    mii.fMask  = MIIM_FTYPE | MIIM_ID | MIIM_STRING | MIIM_SUBMENU | MIIM_BITMAP | MIIM_STATE;
    wchar_t buf[512];
    buf[0]         = L'\0';
    mii.dwTypeData = buf;
    mii.cch        = (UINT)(G_N_ELEMENTS(buf) - 1u);

    if (!GetMenuItemInfoW(hmenu, (UINT)idx, TRUE /* by position */, &mii)) {
      continue;
    }

    /* ----- separator ----- */
    if (mii.fType & MFT_SEPARATOR) {
      GtkWidget *sep = gtk_separator_menu_item_new();
      gtk_widget_show(sep);
      if (mark_dynamic) {
        g_object_set_data(G_OBJECT(sep), DA_DYNAMIC_CTX_KEY, GINT_TO_POINTER(1));
      }
      gtk_menu_shell_append(gtk_shell, sep);
      continue;
    }

    if (buf[0] == L'\0') {
      continue;
    }

    gchar *label = da_win32_strip_accelerators(buf);
    if (label == NULL) {
      continue;
    }

    /* ----- optional 16×16 icon from hbmpItem ----- */
    GdkPixbuf *pixbuf = NULL;
    if (mii.hbmpItem != NULL && mii.hbmpItem != HBMMENU_CALLBACK) {
      pixbuf = da_win32_hbitmap_to_pixbuf(mii.hbmpItem, 16, 16);
    }

    /* Build the GtkMenuItem widget. */
    GtkWidget *mi;
    if (pixbuf != NULL) {
      GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
      GtkWidget *img = gtk_image_new_from_pixbuf(pixbuf);
      g_object_unref(pixbuf);
      gtk_widget_set_valign(img, GTK_ALIGN_CENTER);
      gtk_box_pack_start(GTK_BOX(box), img, FALSE, FALSE, 0);
      GtkWidget *lbl = gtk_label_new(label);
      gtk_widget_set_halign(lbl, GTK_ALIGN_START);
      gtk_box_pack_start(GTK_BOX(box), lbl, TRUE, TRUE, 0);
      mi = gtk_menu_item_new();
      gtk_container_add(GTK_CONTAINER(mi), box);
    } else {
      mi = gtk_menu_item_new_with_label(label);
    }
    g_free(label);

    /* ----- submenu or command ----- */
    if (mii.hSubMenu != NULL) {
      /* Trigger lazy population: the shell fills the submenu HMENU in response
       * to WM_INITMENUPOPUP, which TrackPopupMenu would normally deliver but
       * we must send manually since we use GTK instead of the Win32 message pump. */
      if (cm2 != NULL) {
        IContextMenu2_HandleMenuMsg(cm2, WM_INITMENUPOPUP,
                                    (WPARAM)mii.hSubMenu,
                                    MAKELPARAM((UINT)idx, 0));
      }
      GtkWidget *submenu = gtk_menu_new();
      da_win32_populate_gtk_menu_from_hmenu(GTK_MENU_SHELL(submenu),
                                            mii.hSubMenu, root_menu,
                                            cm2, FALSE);
      gtk_menu_item_set_submenu(GTK_MENU_ITEM(mi), submenu);
    } else {
      /* wID is in [CMD_FIRST, CMD_LAST]; offset passed to InvokeCommand is 0-based. */
      UINT cmd_offset = mii.wID - DA_CTX_CMD_FIRST;
      g_object_set_data(G_OBJECT(mi), DA_SHELL_CMD_ID_KEY,
                        GUINT_TO_POINTER((guint)cmd_offset));
      g_signal_connect(mi, "activate", G_CALLBACK(on_shell_item_activate), root_menu);
    }

    /* Respect disabled / grayed state from the shell. */
    if (mii.fState & (MFS_DISABLED | MFS_GRAYED)) {
      gtk_widget_set_sensitive(mi, FALSE);
    }

    if (mark_dynamic) {
      g_object_set_data(G_OBJECT(mi), DA_DYNAMIC_CTX_KEY, GINT_TO_POINTER(1));
    }
    gtk_widget_show_all(mi);
    gtk_menu_shell_append(gtk_shell, mi);
  }
}

/* ---- public API ---------------------------------------------------------- */

void da_win32_remove_shell_menu_items(GtkMenu *menu) {
  if (menu == NULL || !GTK_IS_MENU(menu)) {
    return;
  }
  GList *children = gtk_container_get_children(GTK_CONTAINER(menu));
  for (GList *l = children; l != NULL; l = l->next) {
    GtkWidget *child = GTK_WIDGET(l->data);
    if (g_object_get_data(G_OBJECT(child), DA_DYNAMIC_CTX_KEY) != NULL) {
      gtk_container_remove(GTK_CONTAINER(menu), child);
    }
  }
  g_list_free(children);
  /* Releasing IContextMenu via the GObject destroy notify. */
  g_object_set_data_full(G_OBJECT(menu), DA_SHELL_CTX_KEY, NULL, NULL);
}

void da_win32_ctx_menu_refresh(AppState *app, GtkMenu *menu) {
  if (menu == NULL || !GTK_IS_MENU(menu)) {
    return;
  }
  da_win32_remove_shell_menu_items(menu);

  if (app == NULL || !app->general_win32_explorer_context_menu) {
    return;
  }

  /* Ensure COM is initialized for this thread (STA required by shell APIs). */
  CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

  /* Collect selected UTF-8 paths. */
  GPtrArray *paths = scan_controller_collect_selected_utf8_paths(app);
  if (paths == NULL) {
    return;
  }

  /* Get the parent window's HWND — required by many shell extensions. */
  HWND hwnd = NULL;
  if (app->window != NULL) {
    GdkWindow *gdk_win = gtk_widget_get_window(app->window);
    if (gdk_win != NULL) {
      hwnd = (HWND)gdk_win32_window_get_handle(gdk_win);
    }
  }

  /* Build absolute PIDLs from UTF-8 paths.
   * Filter to paths that share the same parent directory as the first path;
   * use string comparison (reliable and avoids COM interface-pointer equality
   * pitfalls). */
  gchar *first_dir = g_path_get_dirname((const gchar *)g_ptr_array_index(paths, 0));

  GPtrArray *pidls = g_ptr_array_new(); /* elements are PIDLIST_ABSOLUTE */
  for (guint i = 0; i < paths->len; i++) {
    const gchar *utf8 = (const gchar *)g_ptr_array_index(paths, i);
    gchar *dir = g_path_get_dirname(utf8);
    gboolean same_parent = (g_ascii_strcasecmp(dir, first_dir) == 0);
    g_free(dir);
    if (!same_parent) {
      continue;
    }
    gunichar2 *wpath = g_utf8_to_utf16(utf8, -1, NULL, NULL, NULL);
    if (wpath == NULL) {
      continue;
    }
    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW((LPCWSTR)wpath);
    g_free(wpath);
    if (pidl != NULL) {
      g_ptr_array_add(pidls, pidl);
    }
  }
  g_free(first_dir);
  g_ptr_array_free(paths, TRUE);

  if (pidls->len == 0) {
    g_ptr_array_free(pidls, FALSE);
    return;
  }

  /* Bind to the parent folder and collect child PIDLs.
   * SHBindToParent sets *ppidlLast to an interior pointer inside pidl_abs,
   * valid as long as pidl_abs is not freed. */
  IShellFolder *psf = NULL;
  PCUITEMID_CHILD *children_arr = g_new(PCUITEMID_CHILD, pidls->len);
  guint n_children = 0;

  for (guint i = 0; i < pidls->len; i++) {
    PIDLIST_ABSOLUTE pidl_abs = (PIDLIST_ABSOLUTE)g_ptr_array_index(pidls, i);
    IShellFolder *psf_parent  = NULL;
    PCUITEMID_CHILD child     = NULL;
    if (FAILED(SHBindToParent(pidl_abs, &IID_IShellFolder,
                              (void **)&psf_parent, &child))) {
      continue;
    }
    if (psf == NULL) {
      psf = psf_parent;
    } else {
      IShellFolder_Release(psf_parent);
    }
    children_arr[n_children++] = child;
  }

  IContextMenu *pcm = NULL;
  if (psf != NULL && n_children > 0) {
    IShellFolder_GetUIObjectOf(psf, hwnd, n_children,
                               (LPCITEMIDLIST *)children_arr,
                               &IID_IContextMenu, NULL, (void **)&pcm);
    IShellFolder_Release(psf);
  }
  g_free(children_arr);

  /* Free absolute PIDLs only after GetUIObjectOf (child pointers are interior). */
  for (guint i = 0; i < pidls->len; i++) {
    ILFree((PIDLIST_ABSOLUTE)g_ptr_array_index(pidls, i));
  }
  g_ptr_array_free(pidls, FALSE);

  if (pcm == NULL) {
    return;
  }

  HMENU hm = CreatePopupMenu();
  if (hm == NULL) {
    IContextMenu_Release(pcm);
    return;
  }

  IContextMenu_QueryContextMenu(pcm, hm, 0,
                                DA_CTX_CMD_FIRST, DA_CTX_CMD_LAST,
                                CMF_NORMAL | CMF_EXPLORE);

  if (GetMenuItemCount(hm) <= 0) {
    DestroyMenu(hm);
    IContextMenu_Release(pcm);
    return;
  }

  /* QI for IContextMenu2 so we can drive WM_INITMENUPOPUP for lazy submenus
   * (e.g. "Send to", "Give access to").  Falls back to NULL gracefully. */
  IContextMenu2 *pcm2 = NULL;
  IContextMenu_QueryInterface(pcm, &IID_IContextMenu2, (void **)&pcm2);

  /* Trigger lazy population for the root menu itself. */
  if (pcm2 != NULL) {
    IContextMenu2_HandleMenuMsg(pcm2, WM_INITMENUPOPUP,
                                (WPARAM)hm, MAKELPARAM(0, 0));
  }

  /* Store IContextMenu, IContextMenu2, HWND, and HMENU on the GtkMenu.
   * The HMENU must remain alive until after InvokeCommand returns;
   * da_shell_ctx_data_free (called after activate fires) destroys everything. */
  DaShellCtxData *ctx = g_new0(DaShellCtxData, 1);
  ctx->cm    = pcm;
  ctx->cm2   = pcm2; /* may be NULL; ownership transferred */
  ctx->hwnd  = hwnd;
  ctx->hmenu = hm;   /* ownership transferred — do NOT call DestroyMenu here */
  g_object_set_data_full(G_OBJECT(menu), DA_SHELL_CTX_KEY, ctx, da_shell_ctx_data_free);

  /* Separator between our static items and the shell items. */
  GtkWidget *sep = gtk_separator_menu_item_new();
  gtk_widget_show(sep);
  g_object_set_data(G_OBJECT(sep), DA_DYNAMIC_CTX_KEY, GINT_TO_POINTER(1));
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), sep);

  /* Recursively populate GtkMenu from the Win32 HMENU.
   * cm2 is passed so each nested submenu can also be lazily populated. */
  da_win32_populate_gtk_menu_from_hmenu(GTK_MENU_SHELL(menu), hm, menu, pcm2, TRUE);
  /* hm is intentionally NOT destroyed here — see ctx->hmenu above. */
}

#endif /* G_OS_WIN32 */
