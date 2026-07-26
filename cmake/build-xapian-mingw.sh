#!/usr/bin/env bash
set -euo pipefail

usage() {
  echo "usage: $0 configure <source> <build> <compiler-bin> <build-type>" >&2
  echo "       $0 build <build> <compiler-bin> <jobs>" >&2
  exit 2
}

as_posix_path() {
  local value="${1//\\//}"
  if [[ "${value}" =~ ^([A-Za-z]):/(.*)$ ]]; then
    local drive="${BASH_REMATCH[1],,}"
    printf '/%s/%s\n' "${drive}" "${BASH_REMATCH[2]}"
  else
    printf '%s\n' "${value}"
  fi
}

case "${1:-}" in
  configure)
    [[ $# -eq 5 ]] || usage
    source_dir="$(as_posix_path "$2")"
    build_dir="$(as_posix_path "$3")"
    compiler_bin="$(as_posix_path "$4")"
    build_type="$5"

    export PATH="${compiler_bin}:/usr/bin:${PATH}"
    for tool in make gcc g++ ar ranlib sed grep perl cmp diff; do
      command -v "${tool}" >/dev/null 2>&1 || {
        echo "Required Xapian build tool is missing: ${tool}" >&2
        exit 1
      }
    done

    case "${build_type}" in
      Debug)
        cxxflags="-O0 -g"
        ;;
      RelWithDebInfo)
        cxxflags="-O2 -g -DNDEBUG"
        ;;
      MinSizeRel)
        cxxflags="-Os -DNDEBUG"
        ;;
      *)
        cxxflags="-O2 -DNDEBUG"
        ;;
    esac

    mkdir -p "${build_dir}"
    cd "${build_dir}"
    "${source_dir}/configure" \
      --disable-shared \
      --enable-static \
      --disable-documentation \
      --disable-backend-chert \
      --disable-backend-inmemory \
      --disable-backend-remote \
      CC=gcc \
      CXX=g++ \
      AR=ar \
      RANLIB=ranlib \
      CXXFLAGS="${cxxflags}"
    ;;

  build)
    [[ $# -eq 4 ]] || usage
    build_dir="$(as_posix_path "$2")"
    compiler_bin="$(as_posix_path "$3")"
    jobs="$4"
    export PATH="${compiler_bin}:/usr/bin:${PATH}"
    command -v make >/dev/null 2>&1 || {
      echo "Required Xapian build tool is missing: make" >&2
      exit 1
    }
    cd "${build_dir}"
    make -j"${jobs}" libxapian.la
    test -f .libs/libxapian.a
    test -f include/xapian/version.h
    ;;

  *)
    usage
    ;;
esac
