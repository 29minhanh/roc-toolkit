#! /bin/bash
set -xe
TOOLCHAIN="arm-linux-gnueabihf"
SYSROOT="/opt/toolchains/${TOOLCHAIN}/${TOOLCHAIN}"
CPU="cortex-a15" # armv7
scons -Q clean
for v in debug release
do
  PATH="/opt/toolchains/${TOOLCHAIN}/bin:${PATH}" \
    scons -Q --enable-werror --with-3rdparty=uv,openfec,sox,cpputest \
      host=${TOOLCHAIN} variant=$v

  find bin/${TOOLCHAIN} -name 'roc-test-*' \
    -not -name 'roc-test-lib' |\
    while read t
    do
      LD_LIBRARY_PATH="${SYSROOT}/lib:${PWD}/3rdparty/${TOOLCHAIN}/rpath" \
        python2 site_scons/site_tools/roc/wrappers/timeout.py 300 \
          qemu-arm -L "${SYSROOT}" -cpu ${CPU} $t
    done
done
