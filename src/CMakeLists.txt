# --- pst: no dependency on libcurl or vmime, so it can be hardened alone ------
add_library(imap2pst_pst STATIC
  pst/crc.cpp
  pst/ndb.cpp
  pst/heap.cpp
  pst/ltp.cpp
  pst/nameid_map.cpp
  pst/pst_writer.cpp
  core/message.cpp
)
target_include_directories(imap2pst_pst PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# --- mime --------------------------------------------------------------------
if(IMAP2PST_WITH_MIME)
  add_library(imap2pst_mime STATIC mime/mime_parser.cpp)
  target_include_directories(imap2pst_mime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(imap2pst_mime PUBLIC imap2pst_pst PRIVATE vmime-static)
endif()
