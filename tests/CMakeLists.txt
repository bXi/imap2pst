set(IMAP2PST_FIXTURE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/fixtures)

add_library(imap2pst_test_support INTERFACE)
target_include_directories(imap2pst_test_support INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(imap2pst_test_support
  INTERFACE IMAP2PST_FIXTURE_DIR="${IMAP2PST_FIXTURE_DIR}")

include(GoogleTest)

# --- unit tests --------------------------------------------------------------
add_executable(imap2pst_tests
  test_imap_parse.cpp
  test_pst_binary.cpp
)
target_link_libraries(imap2pst_tests PRIVATE
  imap2pst_pst imap2pst_imap_parse imap2pst_test_support GTest::gtest_main)
gtest_discover_tests(imap2pst_tests)

add_executable(imap2pst_mime_core_tests test_mime_core.cpp)
target_link_libraries(imap2pst_mime_core_tests PRIVATE
  imap2pst_mime_core imap2pst_test_support GTest::gtest_main)
gtest_discover_tests(imap2pst_mime_core_tests)

if(IMAP2PST_WITH_MIME)
  add_executable(imap2pst_mime_tests test_mime.cpp)
  target_link_libraries(imap2pst_mime_tests PRIVATE
    imap2pst_mime imap2pst_test_support GTest::gtest_main)
  gtest_discover_tests(imap2pst_mime_tests)
endif()

# The client and pipeline tests use a fake transport that replays fixtures, so
# they still need no network and no IMAP server.
if(IMAP2PST_WITH_IMAP)
  add_executable(imap2pst_client_tests test_imap_client.cpp)
  target_link_libraries(imap2pst_client_tests PRIVATE
    imap2pst_imap imap2pst_test_support GTest::gtest_main)
  gtest_discover_tests(imap2pst_client_tests)
endif()

if(IMAP2PST_WITH_IMAP AND IMAP2PST_WITH_MIME)
  add_executable(imap2pst_pipeline_tests test_pipeline.cpp)
  target_link_libraries(imap2pst_pipeline_tests PRIVATE
    imap2pst_pipeline imap2pst_test_support GTest::gtest_main)
  gtest_discover_tests(imap2pst_pipeline_tests)
endif()

# --- libpff round-trip -------------------------------------------------------
# The C++ side writes a PST plus a manifest; a Python script reads it back with
# pypff and compares.  pypff is a test-environment prerequisite only, never a
# build dependency: when it is missing the script exits 77 and CTest reports the
# test as skipped rather than failed.
add_executable(pst_fixture_writer pst_fixture_writer.cpp)
target_link_libraries(pst_fixture_writer PRIVATE imap2pst_pst)

add_executable(pst_minimal_writer pst_minimal_writer.cpp)
target_link_libraries(pst_minimal_writer PRIVATE imap2pst_pst)

# --- scale probe -------------------------------------------------------------
# The default run is small enough to belong in the suite: it catches throughput
# decay and per-message memory growth without writing gigabytes.  For the real
# thing, run the probe by hand -- it has been exercised to a million messages
# and a 10 GB file:
#   tests/pst_scale_probe /tmp/big.pst 1000000 200
add_executable(pst_scale_probe pst_scale_probe.cpp)
target_link_libraries(pst_scale_probe PRIVATE imap2pst_pst)
add_test(NAME pst_scale
         COMMAND pst_scale_probe ${CMAKE_CURRENT_BINARY_DIR}/scale.pst 8000 16)
set_tests_properties(pst_scale PROPERTIES LABELS scale TIMEOUT 600)

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
  # A store with no named properties of its own still has to be readable.
  set(_minimal_pst ${CMAKE_CURRENT_BINARY_DIR}/minimal.pst)
  add_test(NAME pst_minimal_write COMMAND pst_minimal_writer ${_minimal_pst})
  set_tests_properties(pst_minimal_write PROPERTIES FIXTURES_SETUP minimal)

  add_test(NAME pst_minimal_verify_libpff
           COMMAND ${Python3_EXECUTABLE}
                   ${CMAKE_CURRENT_SOURCE_DIR}/scripts/verify_pst.py
                   --open-only ${_minimal_pst})
  set_tests_properties(pst_minimal_verify_libpff PROPERTIES
    FIXTURES_REQUIRED minimal
    SKIP_RETURN_CODE 77)
else()
  message(STATUS "Python3 not found; the libpff round-trip test is disabled")
endif()

# --- opt-in live server test -------------------------------------------------
# Needs network access and real credentials, so it is off by default and never
# runs in CI.  See tests/scripts/live_imap_test.sh for the environment it wants.
option(IMAP2PST_LIVE_TEST "Add an opt-in integration test against a real IMAP server" OFF)
if(IMAP2PST_LIVE_TEST)
  if(TARGET imap2pst)
    add_test(NAME imap_live_migration
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/live_imap_test.sh
                     $<TARGET_FILE:imap2pst>
                     ${CMAKE_CURRENT_SOURCE_DIR}/scripts/verify_pst.py
                     ${Python3_EXECUTABLE})
    set_tests_properties(imap_live_migration PROPERTIES LABELS live)
  else()
    message(WARNING "IMAP2PST_LIVE_TEST needs the imap2pst CLI; enable IMAP2PST_WITH_IMAP and IMAP2PST_WITH_MIME")
  endif()
endif()
