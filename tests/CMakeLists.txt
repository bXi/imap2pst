set(IMAP2PST_FIXTURE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/fixtures)

add_library(imap2pst_test_support INTERFACE)
target_include_directories(imap2pst_test_support INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(imap2pst_test_support
  INTERFACE IMAP2PST_FIXTURE_DIR="${IMAP2PST_FIXTURE_DIR}")

include(GoogleTest)

# --- unit tests --------------------------------------------------------------
add_executable(imap2pst_tests test_pst_binary.cpp)
target_link_libraries(imap2pst_tests PRIVATE
  imap2pst_pst imap2pst_test_support GTest::gtest_main)
gtest_discover_tests(imap2pst_tests)

# --- libpff round-trip -------------------------------------------------------
# The C++ side writes a PST plus a manifest; a Python script reads it back with
# pypff and compares.  pypff is a test-environment prerequisite only, never a
# build dependency: when it is missing the script exits 77 and CTest reports the
# test as skipped rather than failed.
add_executable(pst_fixture_writer pst_fixture_writer.cpp)
target_link_libraries(pst_fixture_writer PRIVATE imap2pst_pst)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  set(_roundtrip_pst  ${CMAKE_CURRENT_BINARY_DIR}/roundtrip.pst)
  set(_roundtrip_json ${CMAKE_CURRENT_BINARY_DIR}/roundtrip.json)

  add_test(NAME pst_roundtrip_write
           COMMAND pst_fixture_writer ${_roundtrip_pst} ${_roundtrip_json})
  set_tests_properties(pst_roundtrip_write PROPERTIES FIXTURES_SETUP roundtrip)

  add_test(NAME pst_roundtrip_verify_libpff
           COMMAND ${Python3_EXECUTABLE}
                   ${CMAKE_CURRENT_SOURCE_DIR}/scripts/verify_pst.py
                   ${_roundtrip_pst} ${_roundtrip_json})
  set_tests_properties(pst_roundtrip_verify_libpff PROPERTIES
    FIXTURES_REQUIRED roundtrip
    SKIP_RETURN_CODE 77)
else()
  message(STATUS "Python3 not found; the libpff round-trip test is disabled")
endif()
