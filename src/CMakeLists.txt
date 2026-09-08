# --- pst: no dependency on libcurl or vmime, so it can be hardened alone ------
add_library(imap2pst_pst STATIC
  pst/crc.cpp
  pst/ndb.cpp
  core/message.cpp
)
target_include_directories(imap2pst_pst PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
