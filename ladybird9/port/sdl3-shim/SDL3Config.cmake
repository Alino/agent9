# ladybird9 shim package for find_package(SDL3 CONFIG REQUIRED).
# Ladybird links SDL3::SDL3 (LibWeb, webcontentservice); the library is the
# gamepad-only shim in port/sdl3-shim/ backed by the real SDL3 3.2.24 headers.
# Installed to lib/cmake/SDL3/.
get_filename_component(_sdl3_prefix "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
if(NOT TARGET SDL3::SDL3)
  add_library(SDL3::SDL3 STATIC IMPORTED GLOBAL)
  set_target_properties(SDL3::SDL3 PROPERTIES
    IMPORTED_LOCATION "${_sdl3_prefix}/lib/libSDL3.a"
    INTERFACE_INCLUDE_DIRECTORIES "${_sdl3_prefix}/include")
endif()
set(SDL3_FOUND TRUE)
