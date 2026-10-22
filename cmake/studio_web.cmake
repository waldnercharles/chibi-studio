# Browser application ownership: installed SDK archives, Studio bootstrap/assets.
if(NOT DEFINED Chibi_WEB_THREADS OR NOT DEFINED Chibi_WEB_WORKERS)
    message(FATAL_ERROR "The Emscripten SDK must export its worker ABI configuration")
endif()
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CHIBI_STUDIO_WEB_PROJECTS "" CACHE STRING "Extra browser sample project directories")
set(CHIBI_STUDIO_COLLAB_URL "" CACHE STRING "Public collaboration room service HTTP(S) URL")
if(CHIBI_STUDIO_COLLAB_URL AND NOT CHIBI_STUDIO_COLLAB_URL MATCHES "^https?://")
    message(FATAL_ERROR "Collaboration service URL must use HTTP(S)")
endif()
if(Chibi_WEB_THREADS)
    if(NOT Chibi_WEB_WORKERS MATCHES "^[0-9]+$" OR
       Chibi_WEB_WORKERS LESS 2 OR Chibi_WEB_WORKERS GREATER 8)
        message(FATAL_ERROR "Installed SDK worker budget must be between 2 and 8")
    endif()
    math(EXPR pool "${Chibi_WEB_WORKERS} - 1")
    foreach(target studio zep chibi-studio)
        target_compile_options(${target} PRIVATE -pthread -msimd128)
        target_compile_definitions(${target} PRIVATE CHIBI_WEB_WORKERS=${Chibi_WEB_WORKERS})
    endforeach()
    target_link_options(chibi-studio PRIVATE -pthread -msimd128
        "-sPTHREAD_POOL_SIZE=${pool}" -sPTHREAD_POOL_SIZE_STRICT=2)
    set(bootstrap web_isolation.js)
    set(bootstrap_name chibi-isolation.js)
else()
    set(bootstrap web_default.js)
    set(bootstrap_name chibi-default.js)
endif()
set(web_root "${CMAKE_CURRENT_SOURCE_DIR}/web")
set(shell "${CMAKE_CURRENT_BINARY_DIR}/studio_shell.html")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${web_root}/web_shell.html")
file(READ "${web_root}/web_shell.html" shell_text)
string(REPLACE "{{{ SCRIPT }}}" "<template>{{{ SCRIPT }}}</template><script src=\"${bootstrap_name}\"></script>"
    shell_text "${shell_text}")
file(WRITE "${shell}" "${shell_text}")
configure_file("${web_root}/${bootstrap}" "${CMAKE_BINARY_DIR}/bin/${bootstrap_name}" COPYONLY)
set(collab_url "${CHIBI_STUDIO_COLLAB_URL}")
string(REPLACE "\\" "\\\\" collab_url "${collab_url}")
string(REPLACE "\"" "\\\"" collab_url "${collab_url}")
string(REPLACE "\n" "\\n" collab_url "${collab_url}")
string(REPLACE "\r" "\\r" collab_url "${collab_url}")
string(REPLACE "\t" "\\t" collab_url "${collab_url}")
string(JSON CHIBI_COLLAB_CONFIG SET "{}" roomUrl "\"${collab_url}\"")
configure_file("${web_root}/collab/config.js.in" "${CMAKE_CURRENT_BINARY_DIR}/collab_config.js" @ONLY)
set(preloads
    "${Chibi_RESOURCE_DIR}/shaders@/resources/shaders"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/editor/rmlui/resources@/resources/ui"
    "${CMAKE_CURRENT_SOURCE_DIR}/projects/studio@/projects/studio"
    "${CMAKE_CURRENT_SOURCE_DIR}/projects/starter@/projects/starter")
set(project_names starter studio)
foreach(project IN LISTS CHIBI_STUDIO_WEB_PROJECTS)
    get_filename_component(project "${project}" ABSOLUTE)
    get_filename_component(name "${project}" NAME)
    if(NOT EXISTS "${project}/project.chibi" OR NOT EXISTS "${project}/content")
        message(FATAL_ERROR "Browser sample must have project.chibi and content: ${project}")
    endif()
    if(name IN_LIST project_names OR NOT name MATCHES "^[A-Za-z0-9_-]+$")
        message(FATAL_ERROR "Browser sample folder names must be unique identifiers: ${name}")
    endif()
    list(APPEND project_names "${name}")
    list(APPEND preloads "${project}/project.chibi@/projects/${name}/project.chibi"
                         "${project}/content@/projects/${name}/content")
endforeach()
set(web_assets "${web_root}/collab/client.js" "${shell}" "${CMAKE_CURRENT_BINARY_DIR}/collab_config.js")
foreach(preload IN LISTS preloads)
    string(REGEX REPLACE "@.*$" "" source "${preload}")
    file(GLOB_RECURSE files CONFIGURE_DEPENDS "${source}/*")
    list(APPEND web_assets ${files})
    if(NOT IS_DIRECTORY "${source}")
        list(APPEND web_assets "${source}")
    endif()
    target_link_options(chibi-studio PRIVATE "--preload-file=${preload}")
endforeach()
foreach(script web_storage.js web_cache.js collab/storage.js web_launch.js web_frames.js)
    target_link_options(chibi-studio PRIVATE "--pre-js=${web_root}/${script}")
    list(APPEND web_assets "${web_root}/${script}")
endforeach()
set_property(TARGET chibi-studio APPEND PROPERTY LINK_DEPENDS ${web_assets})
set_target_properties(chibi-studio PROPERTIES SUFFIX ".html")
target_link_options(chibi-studio PRIVATE "--shell-file=${shell}"
    "--pre-js=${CMAKE_CURRENT_BINARY_DIR}/collab_config.js"
    "--pre-js=${web_root}/collab/client.js" -lidbfs.js
    -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=1048576
    "-sEXPORTED_FUNCTIONS=['_main','_free']"
    "-sEXPORTED_RUNTIME_METHODS=['FS','UTF8ToString','stringToNewUTF8']")
install(FILES "$<TARGET_FILE:chibi-studio>"
    "${CMAKE_BINARY_DIR}/bin/chibi-studio.js"
    "${CMAKE_BINARY_DIR}/bin/chibi-studio.wasm"
    "${CMAKE_BINARY_DIR}/bin/chibi-studio.data"
    "${CMAKE_BINARY_DIR}/bin/${bootstrap_name}" DESTINATION .)
if(Chibi_WEB_THREADS)
    install(FILES "${CMAKE_BINARY_DIR}/bin/chibi-studio.worker.js" DESTINATION . OPTIONAL)
endif()
