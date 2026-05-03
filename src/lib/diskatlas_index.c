#include <stdlib.h>
#include <string.h>

#include "diskatlas.h"

#ifndef DISKATLAS_INDEX_INITIAL_CAP
#define DISKATLAS_INDEX_INITIAL_CAP ((size_t)256u)
#endif

DISKATLAS_API void diskatlas_index_init(diskatlas_index_t *idx) {
  if (!idx) {
    return;
  }
  memset(idx, 0, sizeof(*idx));
  idx->struct_version = DISKATLAS_INDEX_STRUCT_VERSION;
}

DISKATLAS_API void diskatlas_index_clear(diskatlas_index_t *idx) {
  if (!idx) {
    return;
  }
  free(idx->entries);
  diskatlas_index_init(idx);
}

DISKATLAS_API size_t diskatlas_index_count(const diskatlas_index_t *idx) {
  return idx ? idx->count : 0;
}

DISKATLAS_API const diskatlas_index_entry_t *diskatlas_index_get(const diskatlas_index_t *idx,
                                                                 size_t i) {
  if (!idx || i >= idx->count) {
    return NULL;
  }
  return &idx->entries[i];
}

DISKATLAS_API int diskatlas_index_add_node(diskatlas_index_t *idx, const file_node_t *node,
                                           uint32_t parent_id, uint32_t *out_id) {
  if (!idx || !node || idx->finalized) {
    return -1;
  }
  if (idx->struct_version != DISKATLAS_INDEX_STRUCT_VERSION) {
    return -1;
  }
  if (parent_id != DISKATLAS_INDEX_NO_PARENT && (size_t)parent_id >= idx->count) {
    return -1;
  }
  if (idx->count >= (size_t)UINT32_MAX) {
    return -1;
  }

  size_t new_count = idx->count + 1u;
  if (idx->capacity < new_count) {
    const size_t el = sizeof(diskatlas_index_entry_t);
    size_t next_cap = idx->capacity == 0 ? DISKATLAS_INDEX_INITIAL_CAP : idx->capacity;

    while (next_cap < new_count) {
      if (next_cap >= SIZE_MAX / 2u || next_cap * 2u < next_cap) {
        if (new_count > SIZE_MAX / el) {
          return -1;
        }
        next_cap = new_count;
        break;
      }
      next_cap *= 2u;
    }

    if (next_cap > SIZE_MAX / el) {
      return -1;
    }

    diskatlas_index_entry_t *ne =
        (diskatlas_index_entry_t *)realloc(idx->entries, next_cap * el);
    if (!ne) {
      return -1;
    }

    idx->entries = ne;
    idx->capacity = next_cap;
  }

  uint32_t new_id = (uint32_t)idx->count;
  diskatlas_index_entry_t *slot = &idx->entries[idx->count];
  memset(slot, 0, sizeof(*slot));
  slot->node = *node;
  slot->parent_id = parent_id;

  idx->count = new_count;
  if (out_id) {
    *out_id = new_id;
  }
  return 0;
}

DISKATLAS_API void diskatlas_index_finalize(diskatlas_index_t *idx) {
  if (!idx) {
    return;
  }
  idx->finalized = 1;

  if (idx->count == 0) {
    free(idx->entries);
    idx->entries = NULL;
    idx->capacity = 0;
    return;
  }

  if (idx->capacity == idx->count) {
    return;
  }

  const size_t el = sizeof(diskatlas_index_entry_t);
  if (idx->count > SIZE_MAX / el) {
    return;
  }

  size_t nbytes = idx->count * el;
  diskatlas_index_entry_t *ne =
      (diskatlas_index_entry_t *)realloc(idx->entries, nbytes);
  if (!ne) {
    return;
  }

  idx->entries = ne;
  idx->capacity = idx->count;
}

DISKATLAS_API void index_init(diskatlas_index_t *idx) {
  diskatlas_index_init(idx);
}

DISKATLAS_API int index_add_node(diskatlas_index_t *idx, const file_node_t *node,
                                 uint32_t parent_id, uint32_t *out_id) {
  return diskatlas_index_add_node(idx, node, parent_id, out_id);
}

DISKATLAS_API void index_finalize(diskatlas_index_t *idx) {
  diskatlas_index_finalize(idx);
}
