#include "gtk_frozen_runtime.h"

#ifdef _WIN32
#include <glib.h>

void gtk_frozen_runtime_preinit(void) {
  /* ---------------------------------------------------------
   * Base portable prefixing
   * --------------------------------------------------------- */
  g_setenv("GTK_DATA_PREFIX", ".", TRUE);
  g_setenv("GTK_EXE_PREFIX", ".", TRUE);
  g_setenv("XDG_DATA_DIRS", "share", TRUE);

  /* ---------------------------------------------------------
   * PIXBUF SANDBOX (strict)
   * --------------------------------------------------------- */
  g_setenv("GDK_PIXBUF_MODULE_FILE", "lib/pixbuf-sandbox/loaders.cache", TRUE);

  g_setenv("GDK_PIXBUF_MODULEDIR", "lib/pixbuf-sandbox/loaders", TRUE);

  g_setenv("GDK_PIXBUF_DISABLE_EXTERNAL_MODULES", "1", TRUE);

  /* ---------------------------------------------------------
   * GTK SCHEMA ISOLATION
   * --------------------------------------------------------- */
  g_setenv("GSETTINGS_SCHEMA_DIR", "share/glib-2.0/schemas", TRUE);

  /* ---------------------------------------------------------
   * GTK PATH HARDENING
   * --------------------------------------------------------- */
  g_setenv("GTK_PATH", ".", TRUE);
}
#endif