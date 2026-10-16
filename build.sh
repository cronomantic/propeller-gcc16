#!/usr/bin/env bash
# build.sh -- build propgcc-16 (GCC 16.2.0 for the Propeller 1) from this
# repository into $PREFIX.
#
#   git submodule update --init       # deps/: binutils, C library, loader, OpenSpin
#   ./build.sh                        # PREFIX=$HOME/propgcc-16, WORK=./work
#   PREFIX=/opt/propgcc-16 WORK=/tmp/pg16 JOBS=8 ./build.sh
#
# Steps: GCC 16.2.0 (downloaded once into $WORK, checked against its
# SHA-256) gets the back end of backend/ and the registration patch of
# patches/; then binutils, the compiler and libgcc, the C library,
# OpenSpin and the loader are built and installed.  Logs go to
# $WORK/logs.  Needs a C/C++ compiler, make, flex, bison, texinfo, curl
# or wget, and the GMP, MPFR and MPC headers.
#
# The 2016-2018 sources in deps/ are built with relaxed diagnostics, as
# in the port's original instructions.  The C library is built without
# -j: its makefile races in parallel.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PREFIX=${PREFIX:-$HOME/propgcc-16}
WORK=${WORK:-$ROOT/work}
JOBS=${JOBS:-$(nproc 2>/dev/null || echo 4)}
case "$(uname -s)" in                           # the loader's makefile wants the host
    Linux*)               LOADER_OS=${LOADER_OS:-linux} ;;
    MINGW*|MSYS*|CYGWIN*) LOADER_OS=${LOADER_OS:-msys} ;;
    Darwin*)              LOADER_OS=${LOADER_OS:-macosx} ;;
    *)                    LOADER_OS=${LOADER_OS:-linux} ;;
esac
GCC_VERSION=16.2.0
GCC_TARBALL=gcc-$GCC_VERSION.tar.xz
GCC_URL=https://ftp.gnu.org/gnu/gcc/gcc-$GCC_VERSION/$GCC_TARBALL
GCC_SHA256=e6738e29597f733270731aa90600f37ffdc045079dfc27ec7e8192cc81085c3e
OLD_C="-std=gnu17 -Wno-error -Wno-implicit-function-declaration -Wno-implicit-int \
-Wno-incompatible-pointer-types -Wno-int-conversion -Wno-return-mismatch"
OLD_LIB="-std=gnu17 -Wno-error -Wno-implicit-function-declaration -Wno-implicit-int \
-Wno-int-conversion -Wno-incompatible-pointer-types -Wno-builtin-declaration-mismatch \
-Wno-unused-but-set-variable"

LOGS=$WORK/logs
mkdir -p "$WORK" "$LOGS" "$PREFIX"
export PATH=$PREFIX/bin:$PATH

step() { echo "=== $1"; }
fail() { echo "build.sh: $1" >&2; exit 1; }
run() {                                         # run LOG command...: quiet unless it fails
    local log=$LOGS/$1; shift
    if ! "$@" > "$log" 2>&1; then
        tail -20 "$log" >&2
        fail "failed, see $log"
    fi
}

for d in binutils-propeller proplib propeller-load OpenSpin; do
    [ -n "$(ls -A "$ROOT/deps/$d" 2>/dev/null)" ] ||
        fail "deps/$d is empty: run 'git submodule update --init' first"
done

step "GCC $GCC_VERSION sources"
cd "$WORK"
if [ ! -f "$GCC_TARBALL" ]; then
    if command -v curl > /dev/null; then curl -sSLO "$GCC_URL"; else wget -q "$GCC_URL"; fi
fi
echo "$GCC_SHA256  $GCC_TARBALL" | sha256sum -c --quiet - || fail "$GCC_TARBALL: wrong SHA-256"
rm -rf "gcc-$GCC_VERSION"
tar xf "$GCC_TARBALL"
cp -a "$ROOT/backend/." "gcc-$GCC_VERSION/"
(cd "gcc-$GCC_VERSION" && for p in "$ROOT"/patches/gcc-$GCC_VERSION/*.patch; do patch -p1 -s < "$p"; done)

step "binutils"
rm -rf build-binutils && mkdir build-binutils && cd build-binutils
run binutils-configure.log "$ROOT/deps/binutils-propeller/configure" --target=propeller-elf \
    --prefix="$PREFIX" --disable-nls --disable-shared --disable-gdb --disable-werror \
    CFLAGS="-O2 -g $OLD_C"
run binutils-make.log make -j"$JOBS"
run binutils-install.log make install
cd ..

step "gcc and libgcc"
rm -rf build-gcc && mkdir build-gcc && cd build-gcc
run gcc-configure.log "../gcc-$GCC_VERSION/configure" --target=propeller-elf --prefix="$PREFIX" \
    --disable-nls --disable-shared --enable-languages=c --without-headers \
    --disable-libssp --disable-libquadmath --disable-libatomic --disable-libgomp \
    --with-pkgversion="propgcc-16"
run gcc-make.log make -j"$JOBS" all-gcc
run gcc-install.log make install-gcc
run libgcc-make.log make -j"$JOBS" all-target-libgcc
run libgcc-install.log make install-target-libgcc
cd ..

step "C library"
# Built from a copy: its makefile writes next to its sources.  Its install
# rule ends by copying licence files from a sibling gcc/ directory that
# does not exist here, so the last two copies are done by hand.
rm -rf proplib build-lib && cp -a "$ROOT/deps/proplib" proplib
cd proplib
run lib-make.log make PREFIX="$PREFIX" BUILD="$WORK/build-lib" LTO="$OLD_LIB"
make PREFIX="$PREFIX" BUILD="$WORK/build-lib" LTO="$OLD_LIB" install > "$LOGS/lib-install.log" 2>&1 || true
cp "$WORK/build-lib/lib/obj/cog/crt0_cog.o" "$WORK/build-lib/lib/obj/cog/crtend_cog.o" \
   "$WORK/build-lib/lib/obj/cog/libcog.a" "$PREFIX/propeller-elf/lib/"
cp "$WORK/gcc-$GCC_VERSION/COPYING.RUNTIME" "$WORK/gcc-$GCC_VERSION/COPYING3" \
   "$PREFIX/propeller-elf/lib/"
cd ..

step "OpenSpin and the loader"
# propeller-load compiles a Spin helper while it builds: openspin first
rm -rf OpenSpin && cp -a "$ROOT/deps/OpenSpin" OpenSpin
run openspin.log make -C OpenSpin
cp OpenSpin/build/openspin* "$PREFIX/bin/"             # openspin.exe on Windows
rm -rf propeller-load build-loader && cp -a "$ROOT/deps/propeller-load" propeller-load
run loader-make.log make -C propeller-load OS="$LOADER_OS" TARGET="$PREFIX" BUILDROOT="$WORK/build-loader"
run loader-install.log make -C propeller-load OS="$LOADER_OS" TARGET="$PREFIX" BUILDROOT="$WORK/build-loader" install

step "done"
propeller-elf-gcc --version | head -1
