# Authoring consumers ship the SDK's native compiler beside their executable,
# so SDK-relative compilation keeps working after a standalone Studio moves.
get_filename_component(sdk_libexec "${Chibi_RESOURCE_DIR}/../../libexec/chibi" ABSOLUTE)
set(asset_compiler "${sdk_libexec}/chibi-asset-compiler${CMAKE_EXECUTABLE_SUFFIX}")
if(NOT EXISTS "${asset_compiler}")
    message(FATAL_ERROR "The installed SDK lacks the native asset compiler: ${asset_compiler}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${asset_compiler}")
file(COPY "${asset_compiler}" DESTINATION "${CMAKE_BINARY_DIR}/bin")
install(PROGRAMS "${asset_compiler}" DESTINATION bin)
