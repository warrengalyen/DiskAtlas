set(DISKATLAS_TEST_SRCS
  main_all_tests.c
  support/test_helpers.c
  test_wildcard.c
  test_csv_roundtrip.c
  test_scan_directory.c
  test_index.c
  test_duplicates.c
)

if(WIN32)
  list(APPEND DISKATLAS_TEST_SRCS test_mft_parse.c)
endif()

add_executable(diskatlas_tests ${DISKATLAS_TEST_SRCS})

target_include_directories(diskatlas_tests PRIVATE
  "${CMAKE_SOURCE_DIR}/src/include"
  "${CMAKE_SOURCE_DIR}/src/lib"
  "${CMAKE_CURRENT_SOURCE_DIR}/support"
)

file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}/tests/fixtures" DISKATLAS_FIXTURES_DIR_CMAKE)
string(REPLACE "\\" "/" DISKATLAS_FIXTURES_DIR_NORM "${DISKATLAS_FIXTURES_DIR_CMAKE}")
target_compile_definitions(diskatlas_tests PRIVATE
  "DISKATLAS_FIXTURES_DIR=\"${DISKATLAS_FIXTURES_DIR_NORM}\""
)

target_link_libraries(diskatlas_tests PRIVATE unity::framework diskatlas_core Threads::Threads)

if(NOT WIN32)
  target_link_libraries(diskatlas_tests PRIVATE m)
endif()

diskatlas_set_exe_bin_output(diskatlas_tests)

add_test(NAME diskatlas_tests COMMAND diskatlas_tests)
set_tests_properties(diskatlas_tests PROPERTIES
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
