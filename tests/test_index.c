#include <string.h>

#include "unity.h"
#include "diskatlas.h"

void test_index_build_tree_subtree_sizes(void) {
  diskatlas_index_t idx;
  diskatlas_index_init(&idx);

  file_node_t root = {0};
  root.struct_version = DISKATLAS_FILE_NODE_STRUCT_VERSION;
  root.attributes = DISKATLAS_NODE_KIND_DIR;
  root.size_bytes = 0;
  root.path = "/";

  file_node_t child = {0};
  child.struct_version = DISKATLAS_FILE_NODE_STRUCT_VERSION;
  child.attributes = DISKATLAS_NODE_KIND_FILE;
  child.size_bytes = 100;
  child.path = "/a.txt";

  uint32_t id_root = 0;
  uint32_t id_child = 0;
  TEST_ASSERT_EQUAL_INT(0, diskatlas_index_add_node(&idx, &root, DISKATLAS_INDEX_NO_PARENT, &id_root));
  TEST_ASSERT_EQUAL_INT(0, diskatlas_index_add_node(&idx, &child, id_root, &id_child));
  TEST_ASSERT_EQUAL_INT(0, diskatlas_index_build_tree(&idx));

  const diskatlas_index_entry_t *e_child = diskatlas_index_get(&idx, id_child);
  TEST_ASSERT_NOT_NULL(e_child);
  TEST_ASSERT_EQUAL_UINT64(100, e_child->subtree_size_bytes);

  const diskatlas_index_entry_t *e_root = diskatlas_index_get(&idx, id_root);
  TEST_ASSERT_NOT_NULL(e_root);
  TEST_ASSERT_EQUAL_UINT64(100, e_root->subtree_size_bytes);

  diskatlas_index_clear(&idx);
}

void test_index_run_all(void) {
  RUN_TEST(test_index_build_tree_subtree_sizes);
}
