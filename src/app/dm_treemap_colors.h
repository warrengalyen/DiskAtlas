#ifndef DM_TREEMAP_COLORS_H
#define DM_TREEMAP_COLORS_H

#include <cairo.h>
#include <glib.h>
#include <stdint.h>

#include "diskatlas.h"

/** Area below this (px²) uses solid fill only (in addition to min width/height). */
#define DM_TREEMAP_AREA_SOLID_THRESHOLD (256.0)

/** Default strengths match INI/app defaults; MIME classify uses these until UI overrides via refresh. */
#define DM_TREEMAP_DEFAULT_GRADIENT_STRENGTH (0.16)
#define DM_TREEMAP_DEFAULT_SHADOW_STRENGTH (0.12)
#define DM_TREEMAP_DEFAULT_BORDER_STRENGTH (0.12)

typedef struct {
  gboolean enable_tile_gradients;
  gboolean enable_tile_borders;
  double gradient_strength;
  double border_strength;
  int gradient_min_tile_size;
} DmTreemapStyle;

/** Recommended defaults: gradients + borders on; strengths 0.16 / 0.12; min tile 8 px. */
#define DM_TREEMAP_STYLE_INIT_DEFAULT                                                               \
  ((DmTreemapStyle){                                                                               \
      .enable_tile_gradients = TRUE,                                                               \
      .enable_tile_borders = TRUE,                                                                 \
      .gradient_strength = DM_TREEMAP_DEFAULT_GRADIENT_STRENGTH,                                    \
      .border_strength = DM_TREEMAP_DEFAULT_BORDER_STRENGTH,                                         \
      .gradient_min_tile_size = 8,                                                                 \
  })

void dm_file_node_compute_gradient_colors(file_node_t *node, double gradient_strength,
                                          double shadow_strength);

/** Recompute gradient_light_rgba / gradient_dark_rgba from existing mime_color_rgba for all file nodes. */
void dm_file_nodes_refresh_gradient_colors(file_node_t *nodes, size_t count, double gradient_strength,
                                           double shadow_strength);

gboolean dm_treemap_tile_use_radial_gradient(double width, double height, const DmTreemapStyle *style);

/** Inner border uses same size thresholds as radial gradient eligibility. */
gboolean dm_treemap_tile_show_border(double width, double height, const DmTreemapStyle *style);

/** Cairo radial fill using cached node RGBA only (no blending in renderer). */
void dm_treemap_draw_gradient_tile(cairo_t *cr, const file_node_t *node, double x, double y, double width,
                                   double height, const DmTreemapStyle *style);

/** Directory tiles: fixed gray base; uses same radial machinery with synthetic cached colors on stack. */
void dm_treemap_draw_dir_gradient_tile(cairo_t *cr, double x, double y, double width, double height,
                                       const DmTreemapStyle *style);

#endif /* DM_TREEMAP_COLORS_H */
