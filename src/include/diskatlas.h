#ifndef DISKATLAS_H
#define DISKATLAS_H

#if defined(_WIN32) || defined(__CYGWIN__)
#ifdef DISKATLAS_BUILD_SHARED
#define DISKATLAS_API __declspec(dllexport)
#else
#define DISKATLAS_API __declspec(dllimport)
#endif
#elif defined(__GNUC__)
#define DISKATLAS_API __attribute__((visibility("default")))
#else
#define DISKATLAS_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

DISKATLAS_API int diskatlas_init(void);

#ifdef __cplusplus
}
#endif

#endif /* DISKATLAS_H */
