#ifndef DISKATLAS_NTFS_MFT_PARSE_H
#define DISKATLAS_NTFS_MFT_PARSE_H

#include "diskatlas.h"

#if defined(_WIN32)

#define DA_NTFS_ATTR_STANDARD_INFORMATION 0x10u
#define DA_NTFS_ATTR_FILE_NAME 0x30u
#define DA_NTFS_ATTR_DATA 0x80u
#define DA_NTFS_MFT_REF_MASK 0x0000FFFFFFFFFFFFull

typedef struct da_ntfs_fn_pick {
  uint32_t score;
  uint64_t parent_id;
  uint64_t real_size;
  uint16_t name_chars;
  wchar_t name_buf[260];
} da_ntfs_fn_pick_t;

/**
 * Apply NTFS update sequence array fixup to an MFT record buffer (in-place).
 * Returns false if USA metadata is invalid.
 */
DISKATLAS_API bool da_ntfs_fixup_record(unsigned char *rec, size_t rec_len, uint16_t sector_size);

/**
 * Parse resident $FILE_NAME / $STANDARD_INFORMATION / unnamed $DATA from one MFT record.
 * Returns true if a $FILE_NAME attribute was found (fn_out->score > 0).
 */
DISKATLAS_API bool da_ntfs_parse_resident_attrs(const unsigned char *rec, size_t rec_len,
                                                 da_ntfs_fn_pick_t *fn_out, uint64_t *mtime_ns_out,
                                                 uint32_t *dos_attr_out, int *has_si_out,
                                                 uint64_t *data_size_out);

#endif /* _WIN32 */

#endif /* DISKATLAS_NTFS_MFT_PARSE_H */
