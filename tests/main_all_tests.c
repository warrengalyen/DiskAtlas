#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

extern void test_wildcard_run_all(void);
extern void test_csv_roundtrip_run_all(void);
extern void test_scan_directory_run_all(void);
extern void test_index_run_all(void);
extern void test_duplicates_run_all(void);
#if defined(_WIN32)
extern void test_mft_parse_run_all(void);
#endif

int main(void) {
  UNITY_BEGIN();
  test_wildcard_run_all();
  test_csv_roundtrip_run_all();
  test_scan_directory_run_all();
  test_index_run_all();
  test_duplicates_run_all();
#if defined(_WIN32)
  test_mft_parse_run_all();
#endif
  return UNITY_END();
}
