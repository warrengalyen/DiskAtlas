#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "unity.h"
#include "test_helpers.h"

#include "diskatlas_ntfs_mft_parse.h"

static void load_mft_fixture(const char *rel, unsigned char **buf, size_t *len) {
  char path[1024];
  TEST_ASSERT_EQUAL_INT(0, da_test_fixture_path(rel, path, sizeof(path)));
  long n = da_test_read_file_bytes(path, buf, len);
  TEST_ASSERT_GREATER_THAN(0, n);
}

void test_mft_fixup_valid(void) {
  unsigned char *buf = NULL;
  size_t len = 0;
  load_mft_fixture("mft/record_fixup_valid.bin", &buf, &len);
  TEST_ASSERT_TRUE(da_ntfs_fixup_record(buf, len, 512));
  free(buf);
}

void test_mft_fixup_invalid_usa(void) {
  unsigned char *buf = NULL;
  size_t len = 0;
  load_mft_fixture("mft/record_fixup_invalid_usa.bin", &buf, &len);
  TEST_ASSERT_FALSE(da_ntfs_fixup_record(buf, len, 512));
  free(buf);
}

void test_mft_parse_resident_filename(void) {
  unsigned char *buf = NULL;
  size_t len = 0;
  load_mft_fixture("mft/record_resident_filename.bin", &buf, &len);
  TEST_ASSERT_TRUE(da_ntfs_fixup_record(buf, len, 512));

  da_ntfs_fn_pick_t fn;
  memset(&fn, 0, sizeof(fn));
  uint64_t mtime = 0;
  uint32_t dos = 0;
  int has_si = 0;
  uint64_t dsz = 0;
  TEST_ASSERT_TRUE(da_ntfs_parse_resident_attrs(buf, len, &fn, &mtime, &dos, &has_si, &dsz));
  TEST_ASSERT_GREATER_THAN_UINT32(0, fn.score);
  TEST_ASSERT_EQUAL_UINT64(5, fn.parent_id);
  TEST_ASSERT_EQUAL_UINT16(4, fn.name_chars);
  TEST_ASSERT_EQUAL_UINT16(4, fn.name_chars);
  TEST_ASSERT_EQUAL_UINT32((unsigned)L't', (unsigned)fn.name_buf[0]);
  TEST_ASSERT_EQUAL_UINT32((unsigned)L'e', (unsigned)fn.name_buf[1]);
  TEST_ASSERT_EQUAL_UINT32((unsigned)L's', (unsigned)fn.name_buf[2]);
  TEST_ASSERT_EQUAL_UINT32((unsigned)L't', (unsigned)fn.name_buf[3]);
  free(buf);
}

void test_mft_parse_run_all(void) {
  RUN_TEST(test_mft_fixup_valid);
  RUN_TEST(test_mft_fixup_invalid_usa);
  RUN_TEST(test_mft_parse_resident_filename);
}
