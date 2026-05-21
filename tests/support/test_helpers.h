#ifndef DA_TEST_HELPERS_H
#define DA_TEST_HELPERS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "diskatlas.h"
#include "unity.h"

#ifndef DISKATLAS_FIXTURES_DIR
#define DISKATLAS_FIXTURES_DIR "tests/fixtures"
#endif

/** Join DISKATLAS_FIXTURES_DIR with relative path into out (size bytes). Returns 0 on success. */
int da_test_fixture_path(const char *rel, char *out, size_t outsz);

/** Read entire file into malloc'd buffer; caller frees. Returns bytes read or -1. */
long da_test_read_file_bytes(const char *path, unsigned char **out_data, size_t *out_len);

bool da_test_wait_scan_complete(scan_result_t *result, unsigned timeout_ms);

typedef struct {
  char *path;
  uint64_t size_bytes;
  uint32_t kind;
} da_test_node_expect_t;

int da_test_load_manifest(const char *manifest_rel, da_test_node_expect_t **out_items, size_t *out_count);

void da_test_free_expectations(da_test_node_expect_t *items, size_t count);

int da_test_compare_scan_to_manifest(scan_result_t *result, const char *manifest_rel);

/** Sort nodes by path (in-place indices). */
void da_test_sort_nodes(file_node_t *nodes, size_t count);

int da_test_compare_scan_results(scan_result_t *a, scan_result_t *b);

void da_test_ensure_tmp_dir(void);

#endif /* DA_TEST_HELPERS_H */
