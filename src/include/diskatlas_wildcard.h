#ifndef DISKATLAS_WILDCARD_H
#define DISKATLAS_WILDCARD_H

#include "diskatlas.h"

#ifdef __cplusplus
extern "C" {
#endif

/** True if filter contains '*' or '?' (UTF-8 code points). */
DISKATLAS_API bool diskatlas_filter_has_wildcard(const char *filter_utf8);

/**
 * Match hay against filter (case-folded UTF-8 glob: '*' substring, '?' one char).
 * If basename_only, hay is matched as a path basename (segment after last / or \\).
 */
DISKATLAS_API bool diskatlas_utf8_matches_filter(const char *hay_utf8, const char *filter_utf8,
                                                 bool basename_only);

#ifdef __cplusplus
}
#endif

#endif /* DISKATLAS_WILDCARD_H */
