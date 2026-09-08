set(IMAP2PST_FIXTURE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/fixtures)

add_library(imap2pst_test_support INTERFACE)
target_include_directories(imap2pst_test_support INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(imap2pst_test_support
  INTERFACE IMAP2PST_FIXTURE_DIR="${IMAP2PST_FIXTURE_DIR}")

include(GoogleTest)
