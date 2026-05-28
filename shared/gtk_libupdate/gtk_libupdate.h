#ifndef GTK_LIBUPDATE_H
#define GTK_LIBUPDATE_H

#include <gtk/gtk.h>

/**
 * Drop-in GTK 3 helpers for libupdate (https://github.com/warrengalyen/libupdate).
 *
 * Copy gtk_libupdate.c and this header into another project, link against libupdate,
 * add this directory to the include path, and define GTK_LIBUPDATE_HAVE_LIBUPDATE=1
 * when the update target is linked. When the macro is 0 or undefined, the check
 * entry point still works and shows an informational dialog.
 *
 * gtk_libupdate_check_for_updates copies *cfg shallowly for the duration of the
 * async check; pointer fields must stay valid until the flow finishes (string
 * literals or process-lifetime storage are fine).
 *
 * Optional manifest fields `description` and `description_format` (see
 * libupdate README) are shown when present: release notes appear in a scrollable
 * area for “update available” and, if the server includes them, when already
 * up to date. HTML descriptions are converted from libupdate’s HTML subset to
 * Pango markup for GtkLabel (not a full browser engine).
 */
typedef void (*GtkLibupdateConfigureDialogFn)(GtkWidget *dialog);

typedef struct GtkLibupdateConfig {
  /** HTTPS manifest URL (same semantics as update_options_t.update_url). Empty = disabled. */
  const char *manifest_url;
  /** Short id passed to libupdate as app_name (ASCII, no spaces). */
  const char *libupdate_app_name;
  /** Product name shown in dialogs (e.g. "My App"). */
  const char *display_name;
  /** Template for g_dir_make_tmp, e.g. "myapp_up_XXXXXX". */
  const char *temp_dir_template;
  /**
   * Optional basename for a stable copy of the download under the install dir before apply
   * (passed to libupdate as package_cache_name). NULL = use the temp download path as-is.
   */
  const char *package_cache_name;
  /** Optional: tweak GtkMessageDialog padding/spacing to match your app. */
  GtkLibupdateConfigureDialogFn configure_message_dialog;
  /**
   * Optional: extra paragraph when manifest_url is empty (e.g. how an admin enables
   * updates). If NULL, a generic message is shown.
   */
  const char *disabled_build_explanation;
} GtkLibupdateConfig;

void gtk_libupdate_check_for_updates(GtkWindow *parent, const GtkLibupdateConfig *cfg);

#endif
