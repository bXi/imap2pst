# --- pst: no dependency on libcurl or vmime, so it can be hardened alone ------
add_library(imap2pst_pst STATIC
  pst/crc.cpp
  pst/ndb.cpp
  pst/heap.cpp
  pst/ltp.cpp
  pst/streaming_table.cpp
  pst/nameid_map.cpp
  pst/pst_writer.cpp
  core/message.cpp
)
target_include_directories(imap2pst_pst PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# --- mime --------------------------------------------------------------------
# Built in layers, each testable on its own: charset recovery, transfer
# encodings, header and address parsing, the entity tree, and the mapping to a
# Message on top.  ICU does charset conversion and nothing else -- it is the
# only dependency, and it is under the Unicode licence.
add_library(imap2pst_mime_core STATIC
  mime/charset.cpp
  mime/encodings.cpp
  mime/headers.cpp
  mime/entity.cpp
  mime/message_builder.cpp
)
target_include_directories(imap2pst_mime_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(imap2pst_mime_core PUBLIC imap2pst_pst PRIVATE ICU::uc ICU::i18n)

if(IMAP2PST_WITH_MIME)
  add_library(imap2pst_mime STATIC mime/mime_parser.cpp)
  target_include_directories(imap2pst_mime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(imap2pst_mime PUBLIC imap2pst_pst imap2pst_mime_core)
endif()

# --- imap --------------------------------------------------------------------
# The response parsers are separated from the libcurl transport so tests can
# drive them with recorded fixtures.
add_library(imap2pst_imap_parse STATIC imap/imap_parse.cpp)
target_include_directories(imap2pst_imap_parse PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(imap2pst_imap_parse PUBLIC imap2pst_pst)

if(IMAP2PST_WITH_IMAP)
  add_library(imap2pst_imap STATIC imap/imap_client.cpp)
  target_include_directories(imap2pst_imap PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(imap2pst_imap PUBLIC imap2pst_imap_parse PRIVATE imap2pst_curl)
endif()

# --- pipeline and CLI --------------------------------------------------------
if(IMAP2PST_WITH_IMAP AND IMAP2PST_WITH_MIME)
  add_library(imap2pst_pipeline STATIC pipeline/pipeline.cpp)
  target_include_directories(imap2pst_pipeline PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(imap2pst_pipeline PUBLIC imap2pst_imap imap2pst_mime imap2pst_pst)

  add_executable(imap2pst main.cpp)
  target_link_libraries(imap2pst PRIVATE imap2pst_pipeline)
endif()
