#  SPDX-License-Identifier: MIT
#
#  EmuSPLASH
#  BundledFreeImage.cmake
#
#  FreeImage 3.18 built into EmuSPLASH (a static library), with the image libraries it comes
#  with, instead of the system's libfreeimage: Debian has dropped it from testing, and a
#  libfreeimage left over from an earlier build breaks when its OpenEXR is updated
#  ("undefined symbol: _ZN7Imf_3_4..."). The source is downloaded when building the first
#  time (7 MB, kept in the build folder).
#
#  Sets FreeImage_INCLUDE_DIRS and FreeImage_LIBRARIES like FindFreeImage.cmake, and the
#  target freeimage_bundled, which es-core waits for.
#

include(ExternalProject)

set(FREEIMAGE_BUNDLED_DIR ${CMAKE_BINARY_DIR}/freeimage-bundled)
set(FREEIMAGE_BUNDLED_SOURCE ${FREEIMAGE_BUNDLED_DIR}/source)

# The libraries it comes with are old: C as it was before C23, C++14 (OpenEXR 2.2 has
# exception specifications), and GCC 14's new errors are warnings again. No warnings shown.
set(FREEIMAGE_BUNDLED_CFLAGS
    "-O3 -fPIC -fexceptions -fvisibility=hidden -std=gnu11 -w -DHAVE_UNISTD_H \
-Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types \
-Wno-error=int-conversion")
set(FREEIMAGE_BUNDLED_CXXFLAGS
    "-O3 -fPIC -fexceptions -fvisibility=hidden -std=c++14 -w -Wno-ctor-dtor-privacy")

# $(MAKE) passes the -j of the build on, with Makefiles.
if(CMAKE_GENERATOR MATCHES "Makefiles")
    set(FREEIMAGE_BUNDLED_MAKE $(MAKE))
else()
    set(FREEIMAGE_BUNDLED_MAKE make)
endif()

set(FREEIMAGE_BUNDLED_EXTRA)
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.24)
    list(APPEND FREEIMAGE_BUNDLED_EXTRA DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
endif()

ExternalProject_Add(freeimage_bundled
    PREFIX ${FREEIMAGE_BUNDLED_DIR}
    SOURCE_DIR ${FREEIMAGE_BUNDLED_SOURCE}
    URL https://downloads.sourceforge.net/project/freeimage/Source%20Distribution/3.18.0/FreeImage3180.zip
    URL_HASH SHA256=f41379682f9ada94ea7b34fe86bf9ee00935a3147be41b6569c9605a53e438fd
    ${FREEIMAGE_BUNDLED_EXTRA}
    BUILD_IN_SOURCE 1
    CONFIGURE_COMMAND ""
    # CFLAGS and CXXFLAGS through the environment, Makefile.gnu adds its own to them.
    BUILD_COMMAND ${CMAKE_COMMAND} -E env "CFLAGS=${FREEIMAGE_BUNDLED_CFLAGS}"
                  "CXXFLAGS=${FREEIMAGE_BUNDLED_CXXFLAGS}"
                  ${FREEIMAGE_BUNDLED_MAKE} -f Makefile.gnu libfreeimage.a
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS ${FREEIMAGE_BUNDLED_SOURCE}/libfreeimage.a)

set(FreeImage_INCLUDE_DIRS ${FREEIMAGE_BUNDLED_SOURCE}/Source)
# The copies of libpng, zlib, libjpeg ... in it aren't given to the other libraries.
set(FreeImage_LIBRARIES ${FREEIMAGE_BUNDLED_SOURCE}/libfreeimage.a
                        -Wl,--exclude-libs,libfreeimage.a)
set(FreeImage_FOUND TRUE)
message("-- FreeImage: built into EmuSPLASH (EMUSPLASH_BUNDLED_FREEIMAGE)")
