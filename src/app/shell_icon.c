#include "shell_icon.h"

#include <string.h>

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gio.h>
#include <gtk/gtk.h>

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#endif

#if defined(_WIN32)

static GdkPixbuf *hicon_to_pixbuf(HICON hIcon, gint width, gint height) {
  HDC hdc_screen = GetDC(NULL);
  if (hdc_screen == NULL) {
    return NULL;
  }
  HDC hdc_mem = CreateCompatibleDC(hdc_screen);
  if (hdc_mem == NULL) {
    ReleaseDC(NULL, hdc_screen);
    return NULL;
  }

  BITMAPINFO bi;
  memset(&bi, 0, sizeof(bi));
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = width;
  bi.bmiHeader.biHeight = -height;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;

  void *bits = NULL;
  HBITMAP hbmp = CreateDIBSection(hdc_mem, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
  if (hbmp == NULL || bits == NULL) {
    DeleteDC(hdc_mem);
    ReleaseDC(NULL, hdc_screen);
    return NULL;
  }

  HBITMAP old = SelectObject(hdc_mem, hbmp);
  memset(bits, 0, (size_t)width * (size_t)height * 4u);
  if (!DrawIconEx(hdc_mem, 0, 0, hIcon, width, height, 0, NULL, DI_NORMAL)) {
    SelectObject(hdc_mem, old);
    DeleteObject(hbmp);
    DeleteDC(hdc_mem);
    ReleaseDC(NULL, hdc_screen);
    return NULL;
  }

  GdkPixbuf *pb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, width, height);
  if (pb == NULL) {
    SelectObject(hdc_mem, old);
    DeleteObject(hbmp);
    DeleteDC(hdc_mem);
    ReleaseDC(NULL, hdc_screen);
    return NULL;
  }

  guchar *dst_base = gdk_pixbuf_get_pixels(pb);
  int rowstride = gdk_pixbuf_get_rowstride(pb);
  const guchar *src = (const guchar *)bits;
  for (gint y = 0; y < height; y++) {
    guchar *dst = dst_base + y * rowstride;
    const guchar *row = src + (size_t)y * (size_t)width * 4u;
    for (gint x = 0; x < width; x++) {
      const guchar *s = row + (size_t)x * 4u;
      guchar b = s[0], g = s[1], r = s[2], a = s[3];
      dst[x * 4 + 0] = r;
      dst[x * 4 + 1] = g;
      dst[x * 4 + 2] = b;
      dst[x * 4 + 3] = a;
    }
  }

  SelectObject(hdc_mem, old);
  DeleteObject(hbmp);
  DeleteDC(hdc_mem);
  ReleaseDC(NULL, hdc_screen);
  return pb;
}

GdkPixbuf *da_shell_icon_for_path(const gchar *path_utf8, gint size_px) {
  if (path_utf8 == NULL || path_utf8[0] == '\0' || size_px <= 0) {
    return NULL;
  }

  gunichar2 *wpath = g_utf8_to_utf16(path_utf8, -1, NULL, NULL, NULL);
  if (wpath == NULL) {
    return NULL;
  }

  SHFILEINFOW sfi;
  memset(&sfi, 0, sizeof(sfi));
  DWORD_PTR hr =
      SHGetFileInfoW((LPCWSTR)wpath, 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_SMALLICON);
  g_free(wpath);

  if (hr == 0 || sfi.hIcon == NULL) {
    return NULL;
  }

  GdkPixbuf *pb = hicon_to_pixbuf(sfi.hIcon, size_px, size_px);
  DestroyIcon(sfi.hIcon);
  return pb;
}

#else /* !defined(_WIN32) */

static GdkPixbuf *shell_icon_pixbuf_from_gicon(GtkIconTheme *theme, GIcon *icon, gint size_px) {
  GError *err = NULL;
  GtkIconInfo *ii = gtk_icon_theme_lookup_by_gicon_for_scale(theme, icon, size_px, 1,
                                                              GTK_ICON_LOOKUP_FORCE_SIZE);
  if (ii == NULL) {
    return NULL;
  }
  GdkPixbuf *pb = gtk_icon_info_load_icon(ii, &err);
  g_object_unref(ii);
  if (pb == NULL) {
    g_clear_error(&err);
    return NULL;
  }
  gint w = gdk_pixbuf_get_width(pb);
  gint h = gdk_pixbuf_get_height(pb);
  if (w != size_px || h != size_px) {
    GdkPixbuf *scaled = gdk_pixbuf_scale_simple(pb, size_px, size_px, GDK_INTERP_BILINEAR);
    g_object_unref(pb);
    pb = scaled;
  }
  return pb;
}

GdkPixbuf *da_shell_icon_for_path_gio(const gchar *path_utf8, gint size_px) {
  if (path_utf8 == NULL || path_utf8[0] == '\0' || size_px <= 0) {
    return NULL;
  }

  GtkIconTheme *theme = gtk_icon_theme_get_default();
  GFile *f = g_file_new_for_path(path_utf8);
  GError *err = NULL;
  GFileInfo *info = g_file_query_info(
      f,
      G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_ICON "," G_FILE_ATTRIBUTE_STANDARD_SYMBOLIC_ICON,
      G_FILE_QUERY_INFO_NONE, NULL, &err);
  g_object_unref(f);
  g_clear_error(&err);

  GIcon *icon = NULL;
  if (info != NULL) {
    GIcon *ic = g_file_info_get_icon(info);
    if (ic != NULL) {
      icon = g_object_ref(ic);
    }
    if (icon == NULL) {
      ic = g_file_info_get_symbolic_icon(info);
      if (ic != NULL) {
        icon = g_object_ref(ic);
      }
    }
    g_object_unref(info);
  }

  if (icon == NULL) {
    if (g_file_test(path_utf8, G_FILE_TEST_IS_DIR)) {
      gchar *ctype = g_content_type_from_mime_type("inode/directory");
      if (ctype != NULL) {
        icon = g_content_type_get_icon(ctype);
        g_free(ctype);
      }
      if (icon == NULL) {
        icon = g_themed_icon_new("folder");
      }
    } else {
      gchar *bn = g_path_get_basename(path_utf8);
      gboolean uncertain = FALSE;
      gchar *ctype = g_content_type_guess(bn, NULL, 0, &uncertain);
      g_free(bn);
      if (ctype != NULL) {
        icon = g_content_type_get_icon(ctype);
        g_free(ctype);
      }
      if (icon == NULL) {
        icon = g_themed_icon_new("text-x-generic");
      }
    }
  }

  GdkPixbuf *pb = shell_icon_pixbuf_from_gicon(theme, icon, size_px);
  g_object_unref(icon);
  return pb;
}

#if !defined(__APPLE__)
GdkPixbuf *da_shell_icon_for_path(const gchar *path_utf8, gint size_px) {
  return da_shell_icon_for_path_gio(path_utf8, size_px);
}
#endif

#endif
