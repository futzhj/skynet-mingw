#!/usr/bin/env bash
set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
DEPS_DIR="${ROOT}/.build/lua-deps"
LUA_INC="${ROOT}/3rd/lua"
LUA_LIB="${LUA_INC}/liblua.a"
OUTPUT_DIR="${LUA_CLIB_PATH:-luaclib}"
CC_BIN="${CC:-gcc}"
CXX_BIN="${CXX:-g++}"
AR_BIN="${AR:-ar}"

case "${OUTPUT_DIR}" in
  /*) ;;
  *) OUTPUT_DIR="${ROOT}/${OUTPUT_DIR#./}" ;;
esac

test -f "${LUA_INC}/lua.h"
test -f "${LUA_LIB}"
mkdir -p "${DEPS_DIR}" "${OUTPUT_DIR}"

fetch_repo() {
  local dest="$1"
  local repo="$2"
  local ref="$3"
  local marker="${dest}/.source-ref"

  if [[ -f "${marker}" ]] && [[ "$(<"${marker}")" == "${ref}" ]]; then
    return
  fi

  rm -rf "${dest}"
  mkdir -p "${dest}"
  curl -fsSL --retry 3 --retry-delay 1 \
    "https://github.com/${repo}/archive/${ref}.tar.gz" |
    tar -xzf - --strip-components=1 -C "${dest}"
  printf '%s\n' "${ref}" > "${marker}"
}

fetch_lsqlite3() {
  local dest="${DEPS_DIR}/lsqlite3"
  local marker="${dest}/.source-ref"
  local archive="${DEPS_DIR}/lsqlite3_v096.zip"
  local checksum="ECC6E7636A54F021BCA5B4A01B35AF06FD7A6FC8B21C4B3ECCD4FDB5DD32AD82"

  if [[ -f "${marker}" ]] && [[ "$(<"${marker}")" == "v0.9.6" ]] &&
     [[ -n "$(find "${dest}" -type f -name lsqlite3.c -print -quit)" ]]; then
    return
  fi

  rm -rf "${dest}"
  mkdir -p "${dest}"
  curl -fsSL --retry 3 --retry-delay 1 \
    'https://lua.sqlite.org/home/zip/lsqlite3_v096.zip?uuid=v0.9.6' \
    -o "${archive}"
  printf '%s  %s\n' "${checksum}" "${archive}" | sha256sum -c -
  unzip -q "${archive}" -d "${dest}"
  printf '%s\n' 'v0.9.6' > "${marker}"
  rm -f "${archive}"
}

fetch_repo "${DEPS_DIR}/luafilesystem" \
  lunarmodules/luafilesystem 154181f4d40641b0763f65ea6caf337fccf87603
fetch_repo "${DEPS_DIR}/lua-zlib" \
  brimworks/lua-zlib 82132912c82337abcafaab2b586090d7783713e3
fetch_repo "${DEPS_DIR}/lua-lz4" \
  witchu/lua-lz4 3454e500f4ccad625c760d6999d200308984dcca
fetch_repo "${DEPS_DIR}/lua-cmsgpack" \
  antirez/lua-cmsgpack 57b1f90cf6cec46450e87289ed5a676165d31071
fetch_repo "${DEPS_DIR}/lua-cjson" \
  mpx/lua-cjson 718f27293a981fb5e9e662e9aec0b7cf78317da6
fetch_repo "${DEPS_DIR}/lua-openssl" \
  zhaozg/lua-openssl eae55e1f7969a802d79bac5e198e7169f5f938f8
fetch_repo "${DEPS_DIR}/lua-openssl/deps/auxiliar" \
  zhaozg/lua-auxiliar 32bf4073ebbd949ef76bbfdd0e973d735a70526d
fetch_repo "${DEPS_DIR}/lua-openssl/deps/lua-compat" \
  keplerproject/lua-compat-5.3 1f6b82a08574b66995181889b786fd22a003f7d3
fetch_repo "${DEPS_DIR}/lua-iconv" \
  lunarmodules/lua-iconv 07f4d564006671ccb7d99922b43186b6aa8f1403
fetch_repo "${DEPS_DIR}/lua-rocksdb" \
  zaherm/lua-rocksdb b3bb28f7959330dd6882e05f1be528d14925cd86
fetch_repo "${DEPS_DIR}/lua-leveldb" \
  marcopompili/lua-leveldb 81255fd83b50a297b02fba164afe9c4f5ccf3749
fetch_lsqlite3

COMMON_CFLAGS=(-std=gnu99 -O2 -Wall -fPIC -DLUA_COMPAT_APIINTCASTS -I"${LUA_INC}")
SHARED_FLAGS=(-shared -Wl,--export-all-symbols)

build_c_module() {
  local name="$1"
  shift
  local sources=()
  local libs=()

  while (($#)); do
    if [[ "$1" == '--' ]]; then
      shift
      libs=("$@")
      break
    fi
    sources+=("$1")
    shift
  done

  "${CC_BIN}" "${COMMON_CFLAGS[@]}" "${SHARED_FLAGS[@]}" \
    "${sources[@]}" -o "${OUTPUT_DIR}/${name}.so" "${LUA_LIB}" "${libs[@]}"
}

build_c_module lfs "${DEPS_DIR}/luafilesystem/src/lfs.c" --
build_c_module zlib "${DEPS_DIR}/lua-zlib/lua_zlib.c" -- -lz
build_c_module lz4 \
  -include string.h \
  "${DEPS_DIR}/lua-lz4/lua_lz4.c" \
  "${DEPS_DIR}/lua-lz4/lz4/lz4.c" \
  "${DEPS_DIR}/lua-lz4/lz4/lz4hc.c" \
  "${DEPS_DIR}/lua-lz4/lz4/lz4frame.c" \
  "${DEPS_DIR}/lua-lz4/lz4/xxhash.c" --
build_c_module cmsgpack "${DEPS_DIR}/lua-cmsgpack/lua_cmsgpack.c" -- -lm
build_c_module cjson \
  "${DEPS_DIR}/lua-cjson/lua_cjson.c" \
  "${DEPS_DIR}/lua-cjson/strbuf.c" \
  "${DEPS_DIR}/lua-cjson/fpconv.c" -- -lm
build_c_module iconv "${DEPS_DIR}/lua-iconv/luaiconv.c" -- -liconv

LSQLITE_SRC="$(find "${DEPS_DIR}/lsqlite3" -type f -name lsqlite3.c -print -quit)"
test -n "${LSQLITE_SRC}"
build_c_module lsqlite3 "${LSQLITE_SRC}" -- -lsqlite3

ROCKSDB_LIBS=(-lrocksdb)
if pkg-config --exists rocksdb 2>/dev/null; then
  read -r -a ROCKSDB_LIBS <<< "$(pkg-config --libs rocksdb)"
fi
ROCKSDB_SRCS=(
  "${DEPS_DIR}/lua-rocksdb/src/lrocksdb.c"
  "${DEPS_DIR}/lua-rocksdb/src/lrocksdb_helpers.c"
  "${DEPS_DIR}/lua-rocksdb/src/lrocksdb_options.c"
  "${DEPS_DIR}/lua-rocksdb/src/lrocksdb_backup_engine.c"
  "${DEPS_DIR}/lua-rocksdb/src/lrocksdb_writebatch.c"
  "${DEPS_DIR}/lua-rocksdb/src/lrocksdb_iter.c"
)
"${CC_BIN}" "${COMMON_CFLAGS[@]}" \
  -I"${DEPS_DIR}/lua-rocksdb/src" "${SHARED_FLAGS[@]}" \
  "${ROCKSDB_SRCS[@]}" -o "${OUTPUT_DIR}/rocksdb.so" \
  "${LUA_LIB}" "${ROCKSDB_LIBS[@]}"

OPENSSL_DIR="${DEPS_DIR}/lua-openssl"
OPENSSL_CFLAGS="$(pkg-config --cflags openssl)"
OPENSSL_LIBS="$(pkg-config --libs openssl)"
make -C "${OPENSSL_DIR}" clean >/dev/null 2>&1 || true
make -C "${OPENSSL_DIR}" all \
  CC="${CC_BIN}" AR="${AR_BIN}" \
  LUA_CFLAGS="-I${LUA_INC}" LUA_LIBDIR="${OUTPUT_DIR}" \
  OPENSSL_CFLAGS="${OPENSSL_CFLAGS}" OPENSSL_LIBS='' \
  LDFLAGS="${LUA_LIB} ${OPENSSL_LIBS} -lws2_32 -lcrypt32"
cp -f "${OPENSSL_DIR}/openssl.so" "${OUTPUT_DIR}/openssl.so"

LEVELDB_DIR="${DEPS_DIR}/lua-leveldb"
LEVELDB_BUILD="${DEPS_DIR}/leveldb-build"
rm -rf "${LEVELDB_BUILD}"
mkdir -p "${LEVELDB_BUILD}"
LEVELDB_CFLAGS=(-O2 -Wall -fPIC -DLUA_COMPAT_APIINTCASTS -I"${LUA_INC}" -I"${LEVELDB_DIR}/src")
"${CC_BIN}" "${COMMON_CFLAGS[@]}" -I"${LEVELDB_DIR}/src" \
  -c "${LEVELDB_DIR}/src/lua.c" -o "${LEVELDB_BUILD}/lua.o"
for source in batch db iter opt utils lua-leveldb; do
  "${CXX_BIN}" -std=gnu++11 "${LEVELDB_CFLAGS[@]}" \
    -c "${LEVELDB_DIR}/src/${source}.cc" \
    -o "${LEVELDB_BUILD}/${source}.o"
done
LEVELDB_LIBS=(-lleveldb -lsnappy -lpthread)
if pkg-config --exists leveldb 2>/dev/null; then
  read -r -a LEVELDB_LIBS <<< "$(pkg-config --libs leveldb)"
fi
"${CXX_BIN}" "${SHARED_FLAGS[@]}" -o "${OUTPUT_DIR}/lualeveldb.so" \
  "${LEVELDB_BUILD}"/*.o "${LUA_LIB}" "${LEVELDB_LIBS[@]}"

for module in lfs zlib lz4 cmsgpack cjson sproto lpeg openssl iconv lsqlite3 rocksdb lualeveldb; do
  test -f "${OUTPUT_DIR}/${module}.so"
done
