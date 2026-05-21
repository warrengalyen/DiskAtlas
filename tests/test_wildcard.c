#include "unity.h"
#include "diskatlas_wildcard.h"

typedef struct {
  const char *hay;
  const char *filter;
  bool basename_only;
  bool expect;
} wild_case_t;

static const wild_case_t k_cases[] = {
    {"hello.txt", "hello", false, true},
    {"hello.txt", "HELLO", false, true},
    {"hello.txt", "*.txt", false, true},
    {"hello.txt", "he??o.txt", false, true},
    {"hello.txt", "*.pdf", false, false},
    {"C:/dir/file.txt", "file", true, true},
    {"C:/dir/file.txt", "dir", true, false},
    {"C:/dir/file.txt", "*dir*", false, true},
    {"", "", false, true},
    {"readme.md", "README", false, true},
};

void test_wildcard_has_wildcard_detects(void) {
  TEST_ASSERT_TRUE(diskatlas_filter_has_wildcard("a*b"));
  TEST_ASSERT_TRUE(diskatlas_filter_has_wildcard("?"));
  TEST_ASSERT_FALSE(diskatlas_filter_has_wildcard("plain"));
}

void test_wildcard_table_cases(void) {
  for (size_t i = 0; i < sizeof(k_cases) / sizeof(k_cases[0]); i++) {
    bool got = diskatlas_utf8_matches_filter(k_cases[i].hay, k_cases[i].filter, k_cases[i].basename_only);
    TEST_ASSERT_EQUAL_INT(k_cases[i].expect ? 1 : 0, got ? 1 : 0);
  }
}

void test_wildcard_run_all(void) {
  RUN_TEST(test_wildcard_has_wildcard_detects);
  RUN_TEST(test_wildcard_table_cases);
}
