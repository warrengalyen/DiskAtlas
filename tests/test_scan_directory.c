#include <string.h>

#include "unity.h"
#include "diskatlas.h"
#include "test_helpers.h"

void test_scan_directory_fixture_tree(void) {
  char root[1024];
  TEST_ASSERT_EQUAL_INT(0, da_test_fixture_path("scan_tree", root, sizeof(root)));

  scan_options_t opt;
  memset(&opt, 0, sizeof(opt));
  opt.struct_version = DISKATLAS_SCAN_OPTIONS_STRUCT_VERSION;
  opt.flags = DISKATLAS_SCAN_OPTION_SKIP_DUPLICATE_CLUSTERING;
  opt.max_depth = 0;

  scan_result_t *sr = scan_start(root, &opt);
  TEST_ASSERT_NOT_NULL(sr);
  TEST_ASSERT_TRUE(da_test_wait_scan_complete(sr, 30000));

  scan_progress_t pr = scan_get_progress(sr);
  TEST_ASSERT_TRUE(pr.is_complete);

#if defined(_WIN32)
  da_test_compare_scan_to_manifest(sr, "expected/scan_tree_manifest_win.txt");
#else
  da_test_compare_scan_to_manifest(sr, "expected/scan_tree_manifest_posix.txt");
#endif

  scan_result_free(sr);
}

void test_scan_directory_run_all(void) {
  RUN_TEST(test_scan_directory_fixture_tree);
}
