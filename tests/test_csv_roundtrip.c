#include <string.h>

#include "unity.h"
#include "diskatlas.h"
#include "test_helpers.h"

void test_csv_import_minimal(void) {
  char path[1024];
  TEST_ASSERT_EQUAL_INT(0, da_test_fixture_path("csv/minimal_scan.csv", path, sizeof(path)));

  char err[256];
  scan_result_t *sr = diskatlas_scan_import_csv(path, err, sizeof(err));
  TEST_ASSERT_NOT_NULL(sr);
  scan_results_view_t v = scan_get_results(sr);
  TEST_ASSERT_GREATER_THAN_UINT32(0, (unsigned)v.count);
  scan_result_free(sr);
}

void test_csv_import_bad_header(void) {
  char path[1024];
  TEST_ASSERT_EQUAL_INT(0, da_test_fixture_path("csv/bad_header.csv", path, sizeof(path)));
  char err[256];
  scan_result_t *sr = diskatlas_scan_import_csv(path, err, sizeof(err));
  TEST_ASSERT_NULL(sr);
  TEST_ASSERT_TRUE(err[0] != '\0');
}

void test_csv_roundtrip_export_reimport(void) {
  char inpath[1024];
  char outpath[1024];
  TEST_ASSERT_EQUAL_INT(0, da_test_fixture_path("csv/minimal_scan.csv", inpath, sizeof(inpath)));
  da_test_ensure_tmp_dir();
  TEST_ASSERT_EQUAL_INT(0, da_test_fixture_path("tmp/roundtrip_out.csv", outpath, sizeof(outpath)));

  char err[256];
  scan_result_t *a = diskatlas_scan_import_csv(inpath, err, sizeof(err));
  TEST_ASSERT_NOT_NULL(a);

  diskatlas_csv_export_options_t opt;
  memset(&opt, 0, sizeof(opt));
  opt.struct_version = DISKATLAS_CSV_EXPORT_OPTIONS_STRUCT_VERSION;
  TEST_ASSERT_EQUAL_INT(0, diskatlas_scan_export_csv(a, outpath, &opt, err, sizeof(err)));

  scan_result_t *b = diskatlas_scan_import_csv(outpath, err, sizeof(err));
  TEST_ASSERT_NOT_NULL(b);

  da_test_compare_scan_results(a, b);

  scan_result_free(b);
  scan_result_free(a);
}

void test_csv_roundtrip_run_all(void) {
  RUN_TEST(test_csv_import_minimal);
  RUN_TEST(test_csv_import_bad_header);
  RUN_TEST(test_csv_roundtrip_export_reimport);
}
