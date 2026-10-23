# Derived from microsoft/vcpkg's brotli port at the project's pinned baseline.
# Keep other dependencies unchanged; only backport the upstream decoder fix.
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO google/brotli
    REF v${VERSION}
    SHA512 f94542afd2ecd96cc41fd21a805a3da314281ae558c10650f3e6d9ca732b8425bba8fde312823f0a564c7de3993bdaab5b43378edab65ebb798cefb6fd702256
    HEAD_REF master
    PATCHES
        install.patch
        pkgconfig.patch
        block-switch-security.patch
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS -DBROTLI_DISABLE_TESTS=ON
)
vcpkg_cmake_install()
vcpkg_copy_pdbs()
vcpkg_fixup_pkgconfig()
vcpkg_cmake_config_fixup(CONFIG_PATH share/unofficial-brotli PACKAGE_NAME unofficial-brotli)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/tools" "${CURRENT_PACKAGES_DIR}/debug/share")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/share/man")

if(VCPKG_TARGET_IS_EMSCRIPTEN)
    set(TOOL_SUFFIX ".js")
endif()
vcpkg_copy_tools(TOOL_NAMES "brotli${TOOL_SUFFIX}" SEARCH_DIR "${CURRENT_PACKAGES_DIR}/tools/brotli")
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
