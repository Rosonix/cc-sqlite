#!/bin/bash

set -e

ROOT_DIR=$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )
TOOLCHAIN="${ROOT_DIR}/../mipsel-toolchain"
ROOTFS="${ROOT_DIR}/../rootfs"
OUT_DIR="${ROOTFS}/usr"

export CROSS_COMPILE=mipsel-linux

export CC="${TOOLCHAIN}/bin/${CROSS_COMPILE}-gcc"
export CXX="${TOOLCHAIN}/bin/${CROSS_COMPILE}-g++"
export AR="${TOOLCHAIN}/bin/${CROSS_COMPILE}-ar"
export AS="${TOOLCHAIN}/bin/${CROSS_COMPILE}-as"
export LD="${TOOLCHAIN}/bin/${CROSS_COMPILE}-ld"
export NM="${TOOLCHAIN}/bin/${CROSS_COMPILE}-nm"
export RANLIB="${TOOLCHAIN}/bin/${CROSS_COMPILE}-ranlib"

export CPPFLAGS=" -I${ROOTFS}/include -I${ROOTFS}/usr/include"
export LDFLAGS=" -L${ROOTFS}/lib -L${ROOTFS}/usr/lib"
export LIBS=" -lpthread -ldl"

git rev-parse --git-dir >/dev/null || exit 1
git log -1 --format=format:%ci%n | sed -e 's/ [-+].*$//;s/ /T/;s/^/D /' > manifest
echo $(git log -1 --format=format:%H) > manifest.uuid

autoreconf
./configure --prefix="${OUT_DIR}" --target=${CROSS_COMPILE} --host=${CROSS_COMPILE}
make
make install
${CC} -shared -o "${OUT_DIR}/lib/libsqlite3.so" -fPIC shell.c sqlite3.c -lpthread -ldl
${CC} -o "${OUT_DIR}/bin/sqlite3" shell.c sqlite3.c -lpthread -ldl
