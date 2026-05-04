#include <stdlib.h>

#include <glib.h>
#include <gtk/gtk.h>

#include "app.h"
#include "diskatlas.h"

int main(int argc, char **argv) {
  if (diskatlas_init() != 0) {
    g_printerr("diskatlas_init failed\n");
    return 1;
  }
  return diskatlas_app_run(argc, argv);
}
