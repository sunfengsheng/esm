include_guard(GLOBAL)
include(ExternalProject)

set(ESM_XAPIAN_PROVIDER "BUNDLED" CACHE STRING
    "Xapian provider for content search: BUNDLED or SYSTEM")
set_property(CACHE ESM_XAPIAN_PROVIDER PROPERTY STRINGS BUNDLED SYSTEM)
set(ESM_XAPIAN_BUILD_JOBS "4" CACHE STRING
    "Parallel jobs used while building the bundled Xapian library")

function(esm_add_xapian_dependency)
  set(bundled_version "1.4.31")
  string(TOUPPER "${ESM_XAPIAN_PROVIDER}" provider)

  if(provider STREQUAL "BUNDLED")
    if(NOT MINGW)
      message(FATAL_ERROR
          "ESM_XAPIAN_PROVIDER=BUNDLED currently requires a MinGW toolchain. "
          "Use ESM_XAPIAN_PROVIDER=SYSTEM with a compatible static library "
          "when building with another compiler.")
    endif()

    set(source_dir "${CMAKE_SOURCE_DIR}/third_party/xapian-core")
    foreach(required_file configure configure.ac COPYING include/xapian.h)
      if(NOT EXISTS "${source_dir}/${required_file}")
        message(FATAL_ERROR
            "Bundled Xapian source is incomplete: missing "
            "${source_dir}/${required_file}")
      endif()
    endforeach()
    file(READ "${source_dir}/configure.ac" configure_ac)
    if(NOT configure_ac MATCHES
        "AC_INIT\\(\\[xapian-core\\], \\[${bundled_version}\\]")
      message(FATAL_ERROR
          "third_party/xapian-core is not Xapian ${bundled_version}")
    endif()

    get_filename_component(compiler_bin_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    find_program(msys2_bash
        NAMES bash.exe bash
        HINTS
          "$ENV{MSYS2_ROOT}/usr/bin"
          "C:/msys64/usr/bin"
        REQUIRED)

    set(binary_dir "${CMAKE_BINARY_DIR}/_deps/xapian-${bundled_version}-build")
    set(static_library "${binary_dir}/.libs/libxapian.a")
    file(MAKE_DIRECTORY "${binary_dir}/include")
    ExternalProject_Add(esm_xapian_external
        SOURCE_DIR "${source_dir}"
        BINARY_DIR "${binary_dir}"
        DOWNLOAD_COMMAND ""
        UPDATE_COMMAND ""
        PATCH_COMMAND ""
        CONFIGURE_COMMAND
          "${msys2_bash}"
          "${CMAKE_SOURCE_DIR}/cmake/build-xapian-mingw.sh"
          configure
          "${source_dir}"
          "${binary_dir}"
          "${compiler_bin_dir}"
          "${CMAKE_BUILD_TYPE}"
        BUILD_COMMAND
          "${msys2_bash}"
          "${CMAKE_SOURCE_DIR}/cmake/build-xapian-mingw.sh"
          build
          "${binary_dir}"
          "${compiler_bin_dir}"
          "${ESM_XAPIAN_BUILD_JOBS}"
        INSTALL_COMMAND ""
        BUILD_BYPRODUCTS "${static_library}"
        BUILD_ALWAYS TRUE
        USES_TERMINAL_CONFIGURE TRUE
        USES_TERMINAL_BUILD TRUE)

    add_library(esm_xapian_dependency STATIC IMPORTED GLOBAL)
    set_target_properties(esm_xapian_dependency PROPERTIES
        IMPORTED_LOCATION "${static_library}"
        INTERFACE_INCLUDE_DIRECTORIES "${binary_dir}/include;${source_dir}/include")
    add_dependencies(esm_xapian_dependency esm_xapian_external)
    set(ESM_XAPIAN_DESCRIPTION
        "bundled Xapian ${bundled_version} source (${source_dir})"
        PARENT_SCOPE)
  elseif(provider STREQUAL "SYSTEM")
    get_filename_component(compiler_bin_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    get_filename_component(toolchain_prefix "${compiler_bin_dir}" DIRECTORY)
    find_path(XAPIAN_INCLUDE_DIR xapian.h
        HINTS "${toolchain_prefix}/include"
        REQUIRED)
    find_file(XAPIAN_STATIC_LIBRARY
        NAMES libxapian.a
        HINTS "${toolchain_prefix}/lib"
        REQUIRED)

    add_library(esm_xapian_dependency STATIC IMPORTED GLOBAL)
    set_target_properties(esm_xapian_dependency PROPERTIES
        IMPORTED_LOCATION "${XAPIAN_STATIC_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${XAPIAN_INCLUDE_DIR}")
    set(ESM_XAPIAN_DESCRIPTION
        "system static Xapian (${XAPIAN_STATIC_LIBRARY})"
        PARENT_SCOPE)
  else()
    message(FATAL_ERROR
        "Unknown ESM_XAPIAN_PROVIDER='${ESM_XAPIAN_PROVIDER}'. "
        "Expected BUNDLED or SYSTEM.")
  endif()
endfunction()
