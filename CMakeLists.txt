cmake_minimum_required(VERSION 3.24)
project(chibi_studio LANGUAGES C CXX)
set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(EMSCRIPTEN)
    set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
endif()

# This consumer never adds the engine checkout or its private header roots.
find_package(Chibi CONFIG REQUIRED)
# A distribution is compiled against one verified SDK package generation.
option(CHIBI_STUDIO_DISTRIBUTION "Require a pinned native Studio distribution" OFF)
set(sdk_manifest "${Chibi_RESOURCE_DIR}/package.json")
set(sdk_id "")
set(sdk_hash "")
if(EXISTS "${sdk_manifest}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${sdk_manifest}")
    file(READ "${sdk_manifest}" sdk_json)
    string(JSON sdk_id ERROR_VARIABLE sdk_error GET "${sdk_json}" build_id)
    if(sdk_error)
        message(FATAL_ERROR "SDK package manifest has no build identity")
    endif()
    file(SHA256 "${sdk_manifest}" sdk_hash)
endif()
if(CHIBI_STUDIO_DISTRIBUTION)
    if(NOT sdk_id OR EMSCRIPTEN OR NOT CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo)$")
        message(FATAL_ERROR "Studio distribution requires a packaged SDK and native Release/RelWithDebInfo build")
    endif()
endif()
set(CHIBI_STUDIO_SDK_BUILD_ID "${sdk_id}" CACHE INTERNAL "Configured SDK package identity" FORCE)
set(CHIBI_STUDIO_SDK_MANIFEST_SHA256 "${sdk_hash}" CACHE INTERNAL "Configured SDK manifest checksum" FORCE)
set(STUDIO_PACKAGES
    asset_authoring asset_json asset_documents asset_values authoring behavior_runtime
    capabilities component_presentation declared_components ecs engine entity_runtime execution
    execution_contract field_catalog graph_authoring graph_compilation graph_model graph_catalog image_preparation input_map json
    material_runtime model_authoring model_runtime native_types physics_runtime platform
    project_compilation project_document project_preparation prototype_document reference_index render save_runtime
    scenario_document scene_authoring scene_document scene_runtime shader_preparation source_catalog source_presentation
    spatial_runtime studio_capabilities studio_graph support text_source tilemap_runtime types ui
    ui_authoring ui_document value_document)
set(missing "")
foreach(package IN LISTS STUDIO_PACKAGES)
    if(NOT TARGET Chibi::${package})
        list(APPEND missing Chibi::${package})
    endif()
endforeach()
if(missing)
    list(JOIN missing ", " names)
    message(FATAL_ERROR "The installed SDK lacks Studio packages: ${names}")
endif()

if(NOT Chibi_RESOURCE_DIR)
    message(FATAL_ERROR "The installed SDK must export Chibi_RESOURCE_DIR")
endif()

include(FetchContent)
set(CHIBI_DEPENDENCY_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
include(cmake/apply_patch.cmake)
include(cmake/zep_dependencies.cmake)
include(cmake/studio_sources.cmake)
add_library(studio STATIC ${STUDIO_SOURCES})
target_include_directories(studio PRIVATE src include)
# Help ▸ About names the engine release Studio was built against.
target_compile_definitions(studio PRIVATE CHIBI_STUDIO_ENGINE_VERSION="${Chibi_VERSION}")
option(CHIBI_STUDIO_TRACY "Profile Studio zones with an installed Tracy client" OFF)
option(CHIBI_STUDIO_WEB_TRACE "Profile Studio zones with browser User Timing" OFF)
if(CHIBI_STUDIO_TRACY AND CHIBI_STUDIO_WEB_TRACE)
    message(FATAL_ERROR "Select one Studio profiling backend")
endif()
if(CHIBI_STUDIO_TRACY)
    find_package(Tracy CONFIG REQUIRED)
    target_link_libraries(studio PRIVATE Tracy::TracyClient)
    target_compile_definitions(studio PRIVATE CHIBI_ENABLE_TRACY)
endif()
if(CHIBI_STUDIO_WEB_TRACE)
    if(NOT EMSCRIPTEN)
        message(FATAL_ERROR "Studio web tracing requires Emscripten")
    endif()
    target_compile_definitions(studio PRIVATE CHIBI_ENABLE_WEB_TRACE)
endif()
foreach(package IN LISTS STUDIO_PACKAGES)
    target_link_libraries(studio PRIVATE Chibi::${package})
endforeach()
target_link_libraries(studio PRIVATE zep)

add_executable(chibi-studio src/host/main.c src/host/state_file.c src/host/stacktrace.c src/studio_registry.c)
target_include_directories(chibi-studio PRIVATE src include)
target_link_libraries(chibi-studio PRIVATE studio Chibi::graph_model Chibi::native_types Chibi::support
    Chibi::platform Chibi::render Chibi::project_preparation Chibi::graph_catalog Chibi::studio_graph)
set_target_properties(chibi-studio PROPERTIES LINKER_LANGUAGE CXX
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(EMSCRIPTEN)
    include(cmake/studio_web.cmake)
else()
# `chibi-dev PROJECT` opens Studio; `chibi PROJECT` plays.
set_target_properties(chibi-studio PROPERTIES OUTPUT_NAME chibi-dev)
if(WIN32)
    # The SDK's manifest makes the runtime's file names UTF-8.
    enable_language(RC)
    target_sources(chibi-studio PRIVATE "${Chibi_RESOURCE_DIR}/windows/chibi.rc")
endif()
# Build and installed hosts use the same executable-relative Studio layout.
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/share/chibi")
foreach(resource shaders licenses)
    if(EXISTS "${Chibi_RESOURCE_DIR}/${resource}")
        file(COPY "${Chibi_RESOURCE_DIR}/${resource}" DESTINATION "${CMAKE_BINARY_DIR}/share/chibi")
        install(DIRECTORY "${Chibi_RESOURCE_DIR}/${resource}/" DESTINATION "share/chibi/${resource}")
    endif()
endforeach()
if(EXISTS "${Chibi_RESOURCE_DIR}/engine.json")
    file(COPY "${Chibi_RESOURCE_DIR}/engine.json" DESTINATION "${CMAKE_BINARY_DIR}/share/chibi")
endif()
install(FILES "${Chibi_RESOURCE_DIR}/engine.json" DESTINATION share/chibi OPTIONAL)
# Export Game runs the SDK's chibi, with the compiler and player beside it. The
# build copies them next to chibi-dev; an installed Studio finds chibi on PATH.
get_filename_component(sdk_bin "${Chibi_RESOURCE_DIR}/../../bin" ABSOLUTE)
foreach(tool chibi chibi-compile chibi-player)
    file(COPY "${sdk_bin}/${tool}${CMAKE_EXECUTABLE_SUFFIX}" DESTINATION "${CMAKE_BINARY_DIR}/bin")
endforeach()
# Reconfigure when authored project files change, are added, or are removed.
# Replace the disposable staged copy so deleted sources cannot survive a rebuild.
file(GLOB_RECURSE studio_project_files CONFIGURE_DEPENDS LIST_DIRECTORIES false
    "${CMAKE_CURRENT_SOURCE_DIR}/projects/studio/*")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${studio_project_files})
file(REMOVE_RECURSE "${CMAKE_BINARY_DIR}/share/chibi/studio")
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/projects/studio" DESTINATION "${CMAKE_BINARY_DIR}/share/chibi")
file(COPY src/editor/rmlui/resources/ DESTINATION "${CMAKE_BINARY_DIR}/share/chibi/ui")
install(TARGETS chibi-studio RUNTIME DESTINATION bin)
install(DIRECTORY projects/studio/ DESTINATION share/chibi/studio)
install(DIRECTORY src/editor/rmlui/resources/ DESTINATION share/chibi/ui)
endif()

option(CHIBI_STUDIO_BUILD_ACCEPTANCE "Build the finite native Studio integration selector" OFF)
if(CHIBI_STUDIO_BUILD_ACCEPTANCE)
    if(EMSCRIPTEN)
        message(FATAL_ERROR "Studio acceptance currently requires a native hidden renderer")
    endif()
    add_executable(studio-acceptance
        tests/acceptance/main.c tests/acceptance/host.c tests/acceptance/application.c
        tests/acceptance/debugger.c
        src/host/state_file.c src/host/stacktrace.c src/studio_registry.c)
    target_include_directories(studio-acceptance PRIVATE src include)
    target_link_libraries(studio-acceptance PRIVATE studio
        Chibi::array_capabilities Chibi::text_capabilities)
    foreach(package IN LISTS STUDIO_PACKAGES)
        target_link_libraries(studio-acceptance PRIVATE Chibi::${package})
    endforeach()
    set_target_properties(studio-acceptance PROPERTIES LINKER_LANGUAGE CXX
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()
