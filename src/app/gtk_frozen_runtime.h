#pragma once

/* Must run before any GTK or GApplication usage (Windows: env sandbox). */
void gtk_frozen_runtime_preinit(void);
