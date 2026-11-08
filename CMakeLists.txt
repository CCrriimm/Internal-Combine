cmake_minimum_required(VERSION 3.20)
project(bs_cheat LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(comet_test SHARED src/test_dll.cpp)
target_compile_definitions(comet_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(comet_test PRIVATE user32)

add_library(comet_pyexec SHARED src/pyexec.cpp)
target_compile_definitions(comet_pyexec PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui-master")
set(MINHOOK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/minhook-master")

# Shipped file name. Nothing that ships starts with "bs" any more - the target
# keeps a stable name for CMake, the artifact is comet_*.
set_target_properties(comet_pyexec PROPERTIES OUTPUT_NAME "comet_pyexec")

add_library(comet_overlay SHARED
    src/overlay.cpp
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_dx11.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_win32.cpp"
    "${MINHOOK_DIR}/src/buffer.c"
    "${MINHOOK_DIR}/src/hook.c"
    "${MINHOOK_DIR}/src/trampoline.c"
    "${MINHOOK_DIR}/src/hde/hde64.c"
)
set_target_properties(comet_overlay PROPERTIES OUTPUT_NAME "comet_overlay")
target_include_directories(comet_overlay PRIVATE
    "${IMGUI_DIR}"
    "${IMGUI_DIR}/backends"
    "${MINHOOK_DIR}/include"
    "${MINHOOK_DIR}/src"
    "${MINHOOK_DIR}/src/hde"
    src
)
# IMGUI_USE_WCHAR32: 16-bit ImWchar cannot address the emoji planes, and player
# names in this game use them. Must be defined for every translation unit that
# includes imgui.h, which is why it lives on the target and not in one file.
# The commit this overlay was built from. A log that cannot say which build
# produced it makes every report ambiguous: the fix is either in the file the
# user is running or it is not, and timestamps do not answer that.
set(OVL_BUILD_ID "dev")
execute_process(
    COMMAND git rev-parse --short=8 HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE _ovl_sha
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
execute_process(
    COMMAND git status --porcelain --untracked-files=no
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE _ovl_dirty
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(_ovl_sha)
    set(OVL_BUILD_ID "${_ovl_sha}")
    if(_ovl_dirty)
        string(APPEND OVL_BUILD_ID "-dirty")
    endif()
endif()
message(STATUS "overlay build id: ${OVL_BUILD_ID}")

target_compile_definitions(comet_overlay PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX IMGUI_USE_WCHAR32 OVL_BUILD_ID="${OVL_BUILD_ID}")
target_link_libraries(comet_overlay PRIVATE d3d11 dxgi d3dcompiler dcomp dwmapi user32)

add_executable(injector src/injector.cpp)
target_compile_definitions(injector PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(injector PRIVATE user32 advapi32)
set_target_properties(injector PROPERTIES LINK_FLAGS "/MANIFEST:EMBED /MANIFESTUAC:\"level='requireAdministrator' uiAccess='false'\"")

add_executable(cometmap driver/bsmap.cpp)
set_target_properties(cometmap PROPERTIES OUTPUT_NAME "cometmap")
target_include_directories(cometmap PRIVATE driver)
target_compile_definitions(cometmap PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(cometmap PROPERTIES LINK_FLAGS "/MANIFEST:EMBED /MANIFESTUAC:\"level='requireAdministrator' uiAccess='false'\"")

# ---------------------------------------------------------------------------
# Single-exe loader (ImGui + D3D11 GUI, embedded payload)
# ---------------------------------------------------------------------------

set(COMET_DIST_DIR "${CMAKE_SOURCE_DIR}/dist" CACHE PATH "Single output folder for the packed loader")

enable_language(RC)

add_executable(comet_loader WIN32
    src/loader.cpp
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_dx11.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_win32.cpp"
    resources/app.rc
)
target_include_directories(comet_loader PRIVATE "${IMGUI_DIR}" "${IMGUI_DIR}/backends" src)
target_compile_definitions(comet_loader PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX IMGUI_USE_WCHAR32)
target_link_libraries(comet_loader PRIVATE d3d11 dxgi dwmapi shell32 advapi32 user32)
set_target_properties(comet_loader PROPERTIES
    WIN32_EXECUTABLE TRUE
    OUTPUT_NAME "Comet"
    RUNTIME_OUTPUT_DIRECTORY         "${COMET_DIST_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${COMET_DIST_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${COMET_DIST_DIR}"
    LINK_FLAGS "/MANIFEST:EMBED /MANIFESTUAC:\"level='requireAdministrator' uiAccess='false'\""
)

option(COMET_LOADER_PREVIEW "Build a non-elevated preview of the loader for UI testing" OFF)
if(COMET_LOADER_PREVIEW)
    add_executable(comet_loader_preview WIN32
        src/loader.cpp
        "${IMGUI_DIR}/imgui.cpp"
        "${IMGUI_DIR}/imgui_draw.cpp"
        "${IMGUI_DIR}/imgui_tables.cpp"
        "${IMGUI_DIR}/imgui_widgets.cpp"
        "${IMGUI_DIR}/backends/imgui_impl_dx11.cpp"
        "${IMGUI_DIR}/backends/imgui_impl_win32.cpp"
        resources/app.rc
    )
    target_include_directories(comet_loader_preview PRIVATE "${IMGUI_DIR}" "${IMGUI_DIR}/backends" src)
    target_compile_definitions(comet_loader_preview PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(comet_loader_preview PRIVATE d3d11 dxgi dwmapi shell32 advapi32 user32)
    set_target_properties(comet_loader_preview PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

find_program(PYTHON_EXE NAMES python python3 py)
set(LOADER_PACK_ARGS)
if(PYTHON_EXE)
    list(APPEND LOADER_PACK_ARGS --file "comet_pyexec.dll=$<TARGET_FILE:comet_pyexec>")
    list(APPEND LOADER_PACK_ARGS --file "comet_overlay.dll=$<TARGET_FILE:comet_overlay>")
    list(APPEND LOADER_PACK_ARGS --file "pyexec/cmd.py=${CMAKE_CURRENT_SOURCE_DIR}/pyexec/cmd.py")
    list(APPEND LOADER_PACK_ARGS --file "pyexec/bs_runtime.py=${CMAKE_CURRENT_SOURCE_DIR}/pyexec/bs_runtime.py")
    list(APPEND LOADER_PACK_ARGS --file "cometmap.exe=$<TARGET_FILE:cometmap>")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/driver/bskdrv.sys")
        list(APPEND LOADER_PACK_ARGS --file "driver/bskdrv.sys=${CMAKE_CURRENT_SOURCE_DIR}/driver/bskdrv.sys")
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/driver/kdmapper.exe")
        list(APPEND LOADER_PACK_ARGS --file "driver/kdmapper.exe=${CMAKE_CURRENT_SOURCE_DIR}/driver/kdmapper.exe")
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/driver/iqvw64e.sys")
        list(APPEND LOADER_PACK_ARGS --file "driver/iqvw64e.sys=${CMAKE_CURRENT_SOURCE_DIR}/driver/iqvw64e.sys")
    endif()

    add_custom_target(pack_loader ALL
        COMMAND "${PYTHON_EXE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/pack_loader.py"
                --exe "$<TARGET_FILE:comet_loader>" ${LOADER_PACK_ARGS}
        DEPENDS comet_loader comet_pyexec comet_overlay cometmap
        COMMENT "Embedding payload into ${COMET_DIST_DIR}/Comet.exe"
        VERBATIM
    )
else()
    message(WARNING "Python not found: Comet.exe will be built without embedded payload")
endif()

# Isolated real ImGui render checks; never initializes game hooks or reads user config.
option(COMET_MENU_PREVIEW "Build the isolated ImGui menu renderer" OFF)
if(COMET_MENU_PREVIEW)
    get_target_property(MENU_PREVIEW_SOURCES comet_overlay SOURCES)
    list(REMOVE_ITEM MENU_PREVIEW_SOURCES src/overlay.cpp)
    add_executable(comet_menu_preview tools/menu_preview.cpp ${MENU_PREVIEW_SOURCES})
    get_target_property(MENU_PREVIEW_INCLUDES comet_overlay INCLUDE_DIRECTORIES)
    target_include_directories(comet_menu_preview PRIVATE ${MENU_PREVIEW_INCLUDES})
    target_compile_definitions(comet_menu_preview PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(comet_menu_preview PRIVATE d3d11 dxgi d3dcompiler dcomp dwmapi user32 windowscodecs ole32)
endif()
