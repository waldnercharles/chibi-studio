# Zep (https://github.com/Rezonality/zep, MIT), the code editor kit_code draws
# through RmlUi. Only its core builds: no Qt, ImGui, tests or demos. The bridge
# creates it without threads (ZepEditorFlags::DisableThreads) and hands it a
# file system that reads and writes nothing, so it never touches disk and
# never starts a thread, natively or in the browser.
FetchContent_Declare(zep
    GIT_REPOSITORY https://github.com/Rezonality/zep.git
    GIT_TAG 7aa7ed940d5c889dc6b27a2529a740c0bb2ad6af # 2026-05-22
    GIT_SUBMODULES "" # The core-only target does not use vcpkg.
    UPDATE_DISCONNECTED TRUE)
FetchContent_GetProperties(zep)
if(NOT zep_POPULATED)
    if(POLICY CMP0169)
        cmake_policy(PUSH)
        cmake_policy(SET CMP0169 OLD)
    endif()
    FetchContent_Populate(zep)
    if(POLICY CMP0169)
        cmake_policy(POP)
    endif()
endif()
# Zep ignores the mouse wheel, so it scrolls by lines as its scroller's
# buttons do, and a line the scroll offset cuts shows its lower part instead
# of leaving a gap; a window tells where its cursor is drawn, for the host's IME and
# completion popup; a mode selects a range, for dragging the pointer; and a
# wrapped line numbers only its first row. Two Vim fixes: undo and redo read
# the command they pop by value, where Zep kept a reference into the stack past
# the pop, and ciw and ciW change the inner word, which "ci<.>" (change inside
# a pair) shadowed. For the host's view (workstream I): the glyph under the
# resting pointer, for hover; a horizontal offset for lines that do not wrap;
# and no command line or airline while the command line is empty. A marker
# that removes itself when text is typed inside it lives until it is done.
chibi_apply_patches("${zep_SOURCE_DIR}"
    "${CHIBI_DEPENDENCY_SOURCE_ROOT}/cmake/patches/zep-integer-types.patch"
    "${CHIBI_DEPENDENCY_SOURCE_ROOT}/cmake/patches/zep-wheel-scroll.patch"
    "${CHIBI_DEPENDENCY_SOURCE_ROOT}/cmake/patches/zep-cursor-info.patch"
    "${CHIBI_DEPENDENCY_SOURCE_ROOT}/cmake/patches/zep-select-range.patch"
    "${CHIBI_DEPENDENCY_SOURCE_ROOT}/cmake/patches/zep-wrapped-line-numbers.patch"
    "${CHIBI_DEPENDENCY_SOURCE_ROOT}/cmake/patches/zep-undo-pop.patch"
    "${CHIBI_DEPENDENCY_SOURCE_ROOT}/cmake/patches/zep-change-inner-word.patch"
    "${CHIBI_DEPENDENCY_SOURCE_ROOT}/cmake/patches/zep-host-view.patch"
    "${CHIBI_DEPENDENCY_SOURCE_ROOT}/cmake/patches/zep-marker-self-clear.patch")

set(ZEP_SOURCES "")
foreach(name buffer commands display editor filesystem glyph_iterator indexer keymap
        line_widgets mode mode_repl mode_search mode_standard mode_tree mode_vim
        range_markers regress scroller splits syntax syntax_markdown syntax_providers
        syntax_rainbow_brackets syntax_tree tab_window theme window
        mcommon/animation/timer mcommon/file/path mcommon/string/stringutils)
    list(APPEND ZEP_SOURCES "${zep_SOURCE_DIR}/src/${name}.cpp")
endforeach()
# editor.cpp includes the configure header Zep's own build writes; nothing in
# it is used.
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/zep/config_app.h" CONTENT "")
add_library(zep STATIC ${ZEP_SOURCES})
target_compile_features(zep PUBLIC cxx_std_17)
target_include_directories(zep SYSTEM PUBLIC "${zep_SOURCE_DIR}/include")
target_include_directories(zep PRIVATE "${zep_SOURCE_DIR}/src" "${CMAKE_CURRENT_BINARY_DIR}/zep")
target_compile_options(zep PRIVATE -w)
# The browser downloads every byte, and editing text is never a hot path.
if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    target_compile_options(zep PRIVATE -Os)
endif()

