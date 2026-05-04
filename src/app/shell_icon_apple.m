#include "shell_icon.h"

#import <Cocoa/Cocoa.h>

extern GdkPixbuf *da_shell_icon_for_path_gio(const gchar *path_utf8, gint size_px);

GdkPixbuf *da_shell_icon_for_path(const gchar *path_utf8, gint size_px) {
  if (path_utf8 == NULL || path_utf8[0] == '\0' || size_px <= 0) {
    return da_shell_icon_for_path_gio(path_utf8, size_px);
  }

  @autoreleasepool {
    NSString *nsPath = [NSString stringWithUTF8String:path_utf8];
    if (nsPath == nil) {
      return da_shell_icon_for_path_gio(path_utf8, size_px);
    }

    NSImage *icon = [[NSWorkspace sharedWorkspace] iconForFile:nsPath];
    if (icon == nil) {
      return da_shell_icon_for_path_gio(path_utf8, size_px);
    }

    CGFloat w = (CGFloat)size_px;
    CGFloat h = (CGFloat)size_px;
    NSSize sz = NSMakeSize(w, h);

    NSImage *scaled =
        [NSImage imageWithSize:sz
                        flipped:NO
                 drawingHandler:^BOOL(NSRect dst) {
                   [icon drawInRect:dst
                           fromRect:NSZeroRect
                          operation:NSCompositingOperationSourceOver
                           fraction:1.0
                     respectFlipped:YES
                              hints:@{NSImageHintInterpolation : @(NSImageInterpolationHigh)}];
                   return YES;
                 }];

    NSData *tiff = [scaled TIFFRepresentation];
    if (tiff == nil || [tiff length] == 0) {
      return da_shell_icon_for_path_gio(path_utf8, size_px);
    }

    GdkPixbufLoader *loader = gdk_pixbuf_loader_new();
    GError *err = NULL;
    if (!gdk_pixbuf_loader_write(loader, (const guchar *)[tiff bytes], (gsize)[tiff length], &err)) {
      g_clear_error(&err);
      g_object_unref(loader);
      return da_shell_icon_for_path_gio(path_utf8, size_px);
    }

    if (!gdk_pixbuf_loader_close(loader, &err)) {
      g_clear_error(&err);
      g_object_unref(loader);
      return da_shell_icon_for_path_gio(path_utf8, size_px);
    }

    GdkPixbuf *pb = gdk_pixbuf_loader_get_pixbuf(loader);
    if (pb != NULL) {
      g_object_ref(pb);
    }
    g_object_unref(loader);

    if (pb == NULL) {
      return da_shell_icon_for_path_gio(path_utf8, size_px);
    }

    gint iw = gdk_pixbuf_get_width(pb);
    gint ih = gdk_pixbuf_get_height(pb);
    if (iw != size_px || ih != size_px) {
      GdkPixbuf *scaled_pb = gdk_pixbuf_scale_simple(pb, size_px, size_px, GDK_INTERP_BILINEAR);
      g_object_unref(pb);
      pb = scaled_pb;
    }

    return pb;
  }
}
