cmake_minimum_required(VERSION 3.25)
project(everything_sm VERSION 0.1.0 LANGUAGES CXX)
option(ESM_BUILD_TESTS "Build unit tests" ON)
option(ESM_BUILD_BENCHMARKS "Build benchmarks" ON)
option(ESM_BUILD_CONTENT_SEARCH "Build the isolated Xapian content-search prototype" OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_library(esm_core
    src/core/index.cpp
    src/core/gui_settings.cpp
    src/core/file_list.cpp
    src/core/saved_search.cpp
    src/core/query.cpp
    src/core/ntfs_catalog.cpp
    src/core/journal_replay.cpp
    src/core/ipc_protocol.cpp
    src/platform/windows/directory_scanner.cpp
    src/platform/windows/directory_watcher.cpp
    src/platform/windows/file_metadata.cpp
    src/platform/windows/ntfs_enumerator.cpp
    src/platform/windows/volume_discovery.cpp
    src/platform/windows/usn_journal.cpp
    src/platform/windows/journal_checkpoint.cpp
    src/platform/windows/live_index.cpp
    src/platform/windows/metadata_snapshot.cpp
    src/platform/windows/metadata_wal.cpp
    src/platform/windows/named_pipe.cpp
)
target_include_directories(esm_core PUBLIC include)
target_link_libraries(esm_core PUBLIC advapi32)
target_compile_definitions(esm_core PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
  target_compile_options(esm_core PRIVATE /W4 /permissive- /utf-8)
else()
  target_compile_options(esm_core PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_executable(esm_cli src/apps/cli/main.cpp)
target_link_libraries(esm_cli PRIVATE esm_core)
add_executable(esm_server src/apps/server/main.cpp)
target_link_libraries(esm_server PRIVATE esm_core)
add_executable(esm_service src/apps/service/main.cpp)
add_executable(esm_launcher WIN32
    src/apps/launcher/main.cpp
    src/apps/gui/resources.rc
)
target_link_libraries(esm_launcher PRIVATE advapi32)
target_link_libraries(esm_service PRIVATE esm_core advapi32)
add_executable(esm_gui WIN32
    src/apps/gui/main.cpp
    src/apps/gui/dialogs.cpp
    src/apps/gui/resources.rc
)
target_link_libraries(esm_gui PRIVATE esm_core comctl32 comdlg32 shell32 ole32 uuid)
if(MINGW)
  target_link_options(esm_cli PRIVATE -static)
  target_link_options(esm_server PRIVATE -static)
  target_link_options(esm_service PRIVATE -municode -static)
  target_link_options(esm_launcher PRIVATE -municode -mwindows -static)
  target_link_options(esm_gui PRIVATE -municode -mwindows -static)
endif()

if(ESM_BUILD_CONTENT_SEARCH)
  include(cmake/BuildXapian.cmake)
  esm_add_xapian_dependency()
  message(STATUS "Content search uses ${ESM_XAPIAN_DESCRIPTION}")

  add_library(esm_content_protocol
      src/content/content_protocol.cpp
      src/platform/windows/content_named_pipe.cpp
  )
  target_include_directories(esm_content_protocol PUBLIC include)
  target_link_libraries(esm_content_protocol PUBLIC advapi32)
  target_compile_definitions(esm_content_protocol PUBLIC
      UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

  add_library(esm_xapian_content
      src/content/xapian_content_index.cpp
      src/platform/windows/content_roots.cpp
  )
  target_include_directories(esm_xapian_content PUBLIC include)
  target_link_libraries(esm_xapian_content
      PRIVATE esm_xapian_dependency z rpcrt4 ws2_32)
  target_compile_definitions(esm_xapian_content PUBLIC
      UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

  add_executable(esm_content_service
      src/apps/content_service/main.cpp
  )
  target_link_libraries(esm_content_service PRIVATE
      esm_core esm_content_protocol esm_xapian_content)

  add_executable(esm_content_lab WIN32
      src/apps/content_lab/main.cpp
      src/apps/gui/resources.rc
  )
  target_link_libraries(esm_content_lab PRIVATE
      esm_content_protocol comctl32 shell32)

  add_executable(esm_content_cli
      src/apps/content_cli/main.cpp
  )
  target_link_libraries(esm_content_cli PRIVATE esm_content_protocol)

  if(ESM_BUILD_TESTS)
    add_executable(esm_content_tests tests/content_test.cpp)
    target_link_libraries(esm_content_tests PRIVATE
        esm_content_protocol esm_xapian_content)
    add_test(NAME esm_content_tests COMMAND esm_content_tests)
  endif()

  set(ESM_CONTENT_TARGETS
      esm_content_protocol
      esm_xapian_content
      esm_content_service
      esm_content_lab
      esm_content_cli)
  if(TARGET esm_content_tests)
    list(APPEND ESM_CONTENT_TARGETS esm_content_tests)
  endif()
  foreach(ESM_CONTENT_TARGET IN LISTS ESM_CONTENT_TARGETS)
    if(MSVC)
      target_compile_options(${ESM_CONTENT_TARGET}
          PRIVATE /W4 /permissive- /utf-8)
    else()
      target_compile_options(${ESM_CONTENT_TARGET}
          PRIVATE -Wall -Wextra -Wpedantic)
    endif()
  endforeach()

  if(MINGW)
    target_link_options(esm_content_service PRIVATE -municode -static)
    target_link_options(esm_content_lab PRIVATE -municode -mwindows -static)
    target_link_options(esm_content_cli PRIVATE -municode -static)
    if(TARGET esm_content_tests)
      target_link_options(esm_content_tests PRIVATE -static)
    endif()
  endif()
endif()

if(ESM_BUILD_TESTS)
  enable_testing()
  add_executable(esm_tests tests/test_main.cpp)
  target_link_libraries(esm_tests PRIVATE esm_core)
  add_dependencies(esm_tests esm_server)
  if(MINGW)
    # Keep the test runner on the same UCRT runtime as the shipped binaries.
    # A dynamically linked runner can accidentally load mingw64 DLLs from PATH
    # instead of the configured ucrt64 toolchain and fail with 0xc0000139.
    target_link_options(esm_tests PRIVATE -static)
  endif()
  add_test(NAME esm_tests COMMAND esm_tests)
endif()
if(ESM_BUILD_BENCHMARKS)
  add_executable(esm_benchmark benchmarks/search_benchmark.cpp)
  target_link_libraries(esm_benchmark PRIVATE esm_core psapi)
  add_executable(esm_catalog_benchmark benchmarks/catalog_benchmark.cpp)
  target_link_libraries(esm_catalog_benchmark PRIVATE esm_core psapi)
  add_executable(esm_snapshot_benchmark benchmarks/snapshot_benchmark.cpp)
  target_link_libraries(esm_snapshot_benchmark PRIVATE esm_core psapi)
  add_executable(esm_real_search_benchmark benchmarks/real_search_benchmark.cpp)
  target_link_libraries(esm_real_search_benchmark PRIVATE esm_core psapi)
endif()












