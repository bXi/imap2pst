cmake_minimum_required(VERSION 3.20)
project(imap2pst
  VERSION 0.1.0
  DESCRIPTION "Migrate IMAP mailboxes into Microsoft Outlook PST files"
  HOMEPAGE_URL "https://github.com/your-org/imap2pst"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(IMAP2PST_BUILD_TESTS "Build the unit and round-trip test suite" ON)
option(IMAP2PST_USE_SYSTEM_DEPS
       "Require the system libcurl rather than building one" OFF)
option(IMAP2PST_WITH_IMAP   "Build the libcurl-backed IMAP transport" ON)
option(IMAP2PST_WITH_MIME   "Build the vmime-backed MIME parser" ON)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(Dependencies)

add_subdirectory(src)

if(IMAP2PST_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(IMAP2PST_WITH_IMAP AND IMAP2PST_WITH_MIME)
  include(Packaging)
endif()

message(STATUS "")
message(STATUS "imap2pst ${PROJECT_VERSION}")
message(STATUS "  build type ....... ${CMAKE_BUILD_TYPE}")
message(STATUS "  IMAP transport ... ${IMAP2PST_WITH_IMAP}")
message(STATUS "  MIME parser ...... ${IMAP2PST_WITH_MIME}")
message(STATUS "  tests ............ ${IMAP2PST_BUILD_TESTS}")
message(STATUS "  system deps ...... ${IMAP2PST_USE_SYSTEM_DEPS}")
message(STATUS "")
