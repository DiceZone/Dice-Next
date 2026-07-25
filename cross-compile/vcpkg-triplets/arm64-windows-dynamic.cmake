# vcpkg triplet for Windows ARM64 cross-compilation
# Usage: cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=arm64-windows-dynamic

set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)

set(VCPKG_CMAKE_SYSTEM_NAME Windows)
set(VCPKG_CMAKE_SYSTEM_PROCESSOR ARM64)
