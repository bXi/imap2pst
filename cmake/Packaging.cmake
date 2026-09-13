# Installation and package generation.
#
# Two shapes of build are supported, and they want different things:
#
#   * self-contained -- dependencies built from source and linked statically.
#     One binary that runs anywhere with a matching libc.  The cost is that
#     every CVE in libcurl or OpenSSL becomes yours to rebuild for.  All of them
#     are permissive, so this raises no licensing question.
#
#   * distribution    -- IMAP2PST_USE_SYSTEM_DEPS=ON, linking the system
#     libcurl and vmime.  This is what a .deb or .rpm should be: the package
#     manager patches the libraries, and dpkg-shlibdeps or rpm's own scanner
#     works out the dependencies from the linked binary.

include(GNUInstallDirs)

install(TARGETS imap2pst RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(FILES ${CMAKE_SOURCE_DIR}/README.md
        DESTINATION ${CMAKE_INSTALL_DOCDIR})

if(EXISTS ${CMAKE_SOURCE_DIR}/LICENSE)
  install(FILES ${CMAKE_SOURCE_DIR}/LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR})
else()
  # Not fatal, because the build is useful without it, but a package with no
  # licence file is not something anyone can redistribute.
  message(WARNING "No LICENSE file: packages built from this tree carry no "
                  "licence, which leaves anyone receiving one unable to "
                  "redistribute it.")
endif()

# Debian policy wants man pages compressed, and rpm expects it too, so the page
# is gzipped at build time when gzip is available rather than shipped raw.
if(EXISTS ${CMAKE_SOURCE_DIR}/docs/imap2pst.1)
  find_program(GZIP_EXECUTABLE gzip)
  if(GZIP_EXECUTABLE)
    set(_man_gz ${CMAKE_BINARY_DIR}/imap2pst.1.gz)
    add_custom_command(
      OUTPUT ${_man_gz}
      # -n keeps the timestamp out, so the same source gives the same bytes.
      COMMAND ${GZIP_EXECUTABLE} -9 -n -c ${CMAKE_SOURCE_DIR}/docs/imap2pst.1 > ${_man_gz}
      DEPENDS ${CMAKE_SOURCE_DIR}/docs/imap2pst.1
      COMMENT "Compressing the man page"
      VERBATIM)
    add_custom_target(imap2pst_manpage ALL DEPENDS ${_man_gz})
    install(FILES ${_man_gz} DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
  else()
    install(FILES ${CMAKE_SOURCE_DIR}/docs/imap2pst.1
            DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
  endif()
endif()

# ----------------------------------------------------------------- CPack ----

set(CPACK_PACKAGE_NAME              imap2pst)
set(CPACK_PACKAGE_VENDOR            "imap2pst contributors")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Migrate IMAP mailboxes into Microsoft Outlook PST files")
set(CPACK_PACKAGE_HOMEPAGE_URL      "${PROJECT_HOMEPAGE_URL}")
set(CPACK_PACKAGE_VERSION           ${PROJECT_VERSION})
set(CPACK_PACKAGE_CONTACT           "imap2pst maintainers <maintainer@example.com>")
set(CPACK_PACKAGE_INSTALL_DIRECTORY imap2pst)
set(CPACK_STRIP_FILES               ON)

if(EXISTS ${CMAKE_SOURCE_DIR}/LICENSE)
  set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_SOURCE_DIR}/LICENSE)
endif()
set(CPACK_RESOURCE_FILE_README ${CMAKE_SOURCE_DIR}/README.md)

# A tarball is always available; the native formats only when their tooling is
# present, so that "cpack" on a machine without rpmbuild does not simply fail.
set(CPACK_GENERATOR "TGZ")
find_program(DPKG_EXECUTABLE dpkg)
find_program(RPMBUILD_EXECUTABLE rpmbuild)
if(DPKG_EXECUTABLE)
  list(APPEND CPACK_GENERATOR "DEB")
endif()
if(RPMBUILD_EXECUTABLE)
  list(APPEND CPACK_GENERATOR "RPM")
endif()

# --- Debian ---
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SECTION mail)
set(CPACK_DEBIAN_PACKAGE_PRIORITY optional)
# Works out Depends: from what the binary actually links, which is right in both
# build shapes: a statically linked build needs little, a system-deps build
# names libcurl and vmime with their real versions.
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)

# --- RPM ---
set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
set(CPACK_RPM_PACKAGE_LICENSE "MIT")
set(CPACK_RPM_PACKAGE_GROUP "Applications/Internet")
set(CPACK_RPM_PACKAGE_AUTOREQ ON)
# /usr/share/man and friends belong to filesystem/man packages; claiming them
# makes the package conflict with anything else that installs a man page.
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
    /usr/share/man /usr/share/man/man1 /usr/share/doc)

# --- source package -------------------------------------------------------
# No licence obliges this, but shipping the exact source a binary was built
# from is worth having for a tool people point at their own mail.
set(CPACK_SOURCE_GENERATOR "TGZ")
set(CPACK_SOURCE_IGNORE_FILES
    "/\\\\.git/" "/build/" "/\\\\.venv/" "\\\\.pst$" "/\\\\.cache/")

include(CPack)
