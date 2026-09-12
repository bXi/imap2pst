# Third-party dependencies.
#
# All of them are permissive: libcurl under its MIT-like licence, ICU under the
# Unicode licence, GoogleTest under BSD-3 and only for tests.  Nothing here
# places a condition on what licence this project may carry -- MIME parsing is
# this tool's own code precisely so that nothing does.
#
# Everything is pulled with FetchContent so the tree contains no vendored
# copies, and a system copy is preferred wherever one exists.

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# -------------------------------------------------------------------- ICU ---
# Charset conversion, and the only thing ICU is used for.  Distributed under
# the Unicode licence, which places no condition on what this project may be.
find_package(ICU REQUIRED COMPONENTS uc i18n)

# ---------------------------------------------------------------- googletest
if(IMAP2PST_BUILD_TESTS)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
    GIT_SHALLOW    TRUE
    EXCLUDE_FROM_ALL
  )
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()

# -------------------------------------------------------------------- libcurl
# Only the IMAP/IMAPS protocols are of interest; everything else is disabled to
# keep the build short.  TLS comes from the system OpenSSL.
if(IMAP2PST_WITH_IMAP)
  if(IMAP2PST_USE_SYSTEM_DEPS)
    find_package(CURL REQUIRED)
  else()
    find_package(CURL QUIET)
  endif()
  if(CURL_FOUND)
    message(STATUS "Using system libcurl ${CURL_VERSION_STRING}")
    add_library(imap2pst_curl INTERFACE)
    target_link_libraries(imap2pst_curl INTERFACE CURL::libcurl)
  else()
    set(BUILD_CURL_EXE      OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS   OFF CACHE BOOL "" FORCE)
    set(BUILD_STATIC_LIBS   ON  CACHE BOOL "" FORCE)
    set(BUILD_TESTING       OFF CACHE BOOL "" FORCE)
    set(CURL_USE_OPENSSL    ON  CACHE BOOL "" FORCE)
    set(CURL_USE_LIBPSL     OFF CACHE BOOL "" FORCE)
    set(CURL_USE_LIBSSH2    OFF CACHE BOOL "" FORCE)
    set(CURL_ZLIB           ON  CACHE STRING "" FORCE)
    set(CURL_BROTLI         OFF CACHE BOOL "" FORCE)
    set(CURL_ZSTD           OFF CACHE BOOL "" FORCE)
    set(USE_LIBIDN2         OFF CACHE BOOL "" FORCE)
    set(ENABLE_UNIX_SOCKETS OFF CACHE BOOL "" FORCE)
    foreach(proto DICT FILE FTP GOPHER LDAP LDAPS MQTT POP3 RTSP SMB SMTP
                  TELNET TFTP)
      set(CURL_DISABLE_${proto} ON CACHE BOOL "" FORCE)
    endforeach()

    # EXCLUDE_FROM_ALL keeps curl's own install rules out of ours.  Without it a
    # package built from this tree ships curl-config, the curl headers, its man
    # pages and libcurl.a alongside the binary.
    FetchContent_Declare(curl
      GIT_REPOSITORY https://github.com/curl/curl.git
      GIT_TAG        curl-8_10_1
      GIT_SHALLOW    TRUE
      EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(curl)
    add_library(imap2pst_curl INTERFACE)
    target_link_libraries(imap2pst_curl INTERFACE CURL::libcurl)
  endif()
endif()
