#include <string.h>

#include "unity.h"
#include "diskatlas.h"
#include "test_helpers.h"

void test_duplicates_scan_same_basename_size(void) {
  char root[1024];
  TEST_ASSERT_EQUAL_INT(0, da_test_fixture_path("dup_tree", root, sizeof(root)));

  scan_options_t opt;
  memset(&opt, 0, sizeof(opt));
  opt.struct_version = DISKATLAS_SCAN_OPTIONS_STRUCT_VERSION;
  opt.flags = 0;
  opt.max_depth = 0;

  scan_result_t *sr = scan_start(root, &opt);
  TEST_ASSERT_NOT_NULL(sr);
  TEST_ASSERT_TRUE(da_test_wait_scan_complete(sr, 30000));

  uint32_t max_gid = diskatlas_dup_max_group_id(sr);
  TEST_ASSERT_GREATER_THAN_UINT32(0, max_gid);

  size_t members = 0;
  const size_t *mem = diskatlas_dup_group_members(sr, max_gid, &members);
  TEST_ASSERT_NOT_NULL(mem);
  TEST_ASSERT_GREATER_OR_EQUAL_UINT32(2, (unsigned)members);

  scan_result_free(sr);
}

void test_duplicates_run_all(void) {
  RUN_TEST(test_duplicates_scan_same_basename_size);
}
