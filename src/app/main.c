#include <gtk/gtk.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "diskatlas.h"

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE instance, HINSTANCE prev_instance,
                   LPSTR cmd_line, int cmd_show)
#else
int main(int argc, char **argv)
#endif
{
#ifdef _WIN32
  (void)instance;
  (void)prev_instance;
  (void)cmd_line;
  (void)cmd_show;
#endif

  if (diskatlas_init() != 0) {
    return EXIT_FAILURE;
  }

  gtk_disable_setlocale();

#ifdef _WIN32
  /* gtk_init allocates/fills argv equivalent on Win32 GUI builds */
  int argc = 0;
  char **argv = NULL;
#endif

  if (!gtk_init_check(&argc, &argv)) {
    return EXIT_FAILURE;
  }

  GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title(GTK_WINDOW(window), "DiskAtlas");
  gtk_window_set_default_size(GTK_WINDOW(window), 800, 600);
  g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
  gtk_widget_show_all(window);

  gtk_main();

  return EXIT_SUCCESS;
}
