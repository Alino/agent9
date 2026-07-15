# ladybird9 shim package for find_package(mimalloc CONFIG REQUIRED).
# Ladybird links the bare target name `mimalloc` (AK/CMakeLists.txt); vcpkg's
# real package exports the same name. Installed to lib/cmake/mimalloc/.
get_filename_component(_mi_prefix "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
if(NOT TARGET mimalloc)
  add_library(mimalloc STATIC IMPORTED GLOBAL)
  set_target_properties(mimalloc PROPERTIES
    IMPORTED_LOCATION "${_mi_prefix}/lib/libmimalloc.a"
    INTERFACE_INCLUDE_DIRECTORIES "${_mi_prefix}/include")
endif()
set(mimalloc_FOUND TRUE)
