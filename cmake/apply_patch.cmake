# chibi_apply_patches(<checkout> <patch>...): brings a dependency's git
# checkout to its pinned commit plus the patches, applied in order. A stamp
# in the checkout records the patch set it holds, so an edited, added or
# removed patch resets the checkout's tracked files and applies them all
# again, in every build directory, instead of failing against the old set.
function(chibi_apply_patches checkout)
    set(contents "")
    foreach(patch IN LISTS ARGN)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${patch}")
        file(SHA256 "${patch}" hash)
        get_filename_component(name "${patch}" NAME)
        string(APPEND contents "${name} ${hash}\n")
    endforeach()
    set(stamp "${checkout}/.chibi-patches")
    if(EXISTS "${stamp}")
        file(READ "${stamp}" applied)
        if(applied STREQUAL contents)
            return()
        endif()
    endif()
    find_package(Git REQUIRED)
    # Reset only changed files. Checking out the entire tree rewrites even
    # unchanged APFS/reflink copies and breaks their shared storage.
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${checkout}" -c core.quotepath=false
            diff --name-only --diff-filter=MD --ignore-submodules=all
        OUTPUT_VARIABLE modified
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE reset_result
        ERROR_VARIABLE reset_error
    )
    if(NOT reset_result EQUAL 0)
        message(FATAL_ERROR "Could not inspect ${checkout}: ${reset_error}")
    endif()
    if(NOT modified STREQUAL "")
        string(REPLACE "\n" ";" modified "${modified}")
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${checkout}" checkout -- ${modified}
            RESULT_VARIABLE reset_result
            ERROR_VARIABLE reset_error
        )
        if(NOT reset_result EQUAL 0)
            message(FATAL_ERROR "Could not reset ${checkout}: ${reset_error}")
        endif()
    endif()
    foreach(patch IN LISTS ARGN)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${checkout}" apply "${patch}"
            RESULT_VARIABLE apply_result
            ERROR_VARIABLE apply_error
        )
        if(NOT apply_result EQUAL 0)
            message(FATAL_ERROR "${patch} does not apply to ${checkout}: ${apply_error}")
        endif()
    endforeach()
    file(WRITE "${stamp}" "${contents}")
endfunction()
