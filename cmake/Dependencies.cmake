# Third-party dependencies.
#
# Everything is pulled with FetchContent so the tree contains no vendored
# copies.  Each dependency can be satisfied by a system package instead by
# pointing CMake at it (see README); the FIND_PACKAGE_ARGS below lets
# FetchContent prefer an installed copy when one exists.

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

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

# ---------------------------------------------------------------------- vmime
# vmime is used purely as a MIME parser.  Its own messaging stack (IMAP, POP3,
# SMTP, maildir, sendmail) is switched off: we do transport with libcurl, and
# leaving the sendmail protocol enabled makes vmime's configure step fail when
# no sendmail binary is installed.
if(IMAP2PST_WITH_MIME AND IMAP2PST_USE_SYSTEM_DEPS)
  # What a distribution package wants: the system copy, patched by whoever
  # ships it, and named as a real dependency in the package metadata.  Debian
  # and Ubuntu carry libvmime-dev; Fedora does not, so a self-contained build
  # is the fallback there.
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(VMIME REQUIRED IMPORTED_TARGET vmime)
  add_library(vmime-static INTERFACE)
  target_link_libraries(vmime-static INTERFACE PkgConfig::VMIME)
  message(STATUS "Using system vmime ${VMIME_VERSION}")
elseif(IMAP2PST_WITH_MIME)
  set(VMIME_BUILD_SHARED_LIBRARY      OFF CACHE BOOL "" FORCE)
  set(VMIME_BUILD_STATIC_LIBRARY      ON  CACHE BOOL "" FORCE)
  set(VMIME_BUILD_TESTS               OFF CACHE BOOL "" FORCE)
  set(VMIME_BUILD_SAMPLES             OFF CACHE BOOL "" FORCE)
  set(VMIME_BUILD_DOCUMENTATION       OFF CACHE BOOL "" FORCE)
  set(VMIME_INSTALL                   OFF CACHE BOOL "" FORCE)
  set(VMIME_HAVE_MESSAGING_FEATURES   OFF CACHE BOOL "" FORCE)
  set(VMIME_HAVE_SASL_SUPPORT         OFF CACHE BOOL "" FORCE)
  set(VMIME_HAVE_TLS_SUPPORT          OFF CACHE BOOL "" FORCE)
  foreach(proto POP3 SMTP IMAP MAILDIR SENDMAIL)
    set(VMIME_HAVE_MESSAGING_PROTO_${proto} OFF CACHE BOOL "" FORCE)
  endforeach()

  FetchContent_Declare(vmime
    GIT_REPOSITORY https://github.com/kisli/vmime.git
    GIT_TAG        5b0191136f84c177b737c9cf9aa7cf59d1c65ef1
    GIT_SHALLOW    FALSE
    EXCLUDE_FROM_ALL
  )
  FetchContent_MakeAvailable(vmime)

  # vmime/export.hpp picks export-static.hpp only when VMIME_STATIC is defined,
  # and the target does not publish that itself. Consumers of the static library
  # have to define it or the header pulls in the shared-build variant, which is
  # never generated here.
  target_compile_definitions(vmime-static PUBLIC VMIME_STATIC)
endif()
