cmake_minimum_required(VERSION 3.20)
project(comet_cheat LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui-master")
set(MINHOOK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/minhook-master")

# ---------------------------------------------------------------------------
# comet_pyexec.dll  (was bs_pyexec)
# ---------------------------------------------------------------------------
add_library(comet_pyexec SHARED src/pyexec.cpp)
target_compile_definitions(comet_pyexec PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX
    COMET_OBFUSCATE=1          # enables XOR string obfuscation in pyexec
)
set_target_properties(comet_pyexec PROPERTIES OUTPUT_NAME "comet_pyexec")

# ---------------------------------------------------------------------------
# comet_overlay.dll  (was bs_overlay)
# ---------------------------------------------------------------------------
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
target_include_directories(comet_overlay PRIVATE
    "${IMGUI_DIR}"
    "${IMGUI_DIR}/backends"
    "${MINHOOK_DIR}/include"
    "${MINHOOK_DIR}/src"
    "${MINHOOK_DIR}/src/hde"
    src
)
target_compile_definitions(comet_overlay PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX
    COMET_OBFUSCATE=1
)
target_link_libraries(comet_overlay PRIVATE d3d11 dxgi d3dcompiler dcomp dwmapi user32)
set_target_properties(comet_overlay PROPERTIES OUTPUT_NAME "comet_overlay")

# ---------------------------------------------------------------------------
# injector.exe  (name unchanged — it's a standalone tool, no bs_ prefix)
# ---------------------------------------------------------------------------
add_executable(injector src/injector.cpp)
target_compile_definitions(injector PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(injector PRIVATE user32 advapi32)
set_target_properties(injector PROPERTIES
    LINK_FLAGS "/MANIFEST:EMBED /MANIFESTUAC:\"level='requireAdministrator' uiAccess='false'\""
)

# ---------------------------------------------------------------------------
# bsmap.exe  (name unchanged — embedded into Comet.exe, not distributed loose)
# ---------------------------------------------------------------------------
add_executable(bsmap driver/bsmap.cpp)
target_include_directories(bsmap PRIVATE driver)
target_compile_definitions(bsmap PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(bsmap PROPERTIES
    LINK_FLAGS "/MANIFEST:EMBED /MANIFESTUAC:\"level='requireAdministrator' uiAccess='false'\""
)

# ---------------------------------------------------------------------------
# Comet.exe  (was CiyE.exe / bs_loader)
# Output: dist/Comet.exe
# ---------------------------------------------------------------------------
set(CIYE_DIST_DIR "${CMAKE_SOURCE_DIR}/dist" CACHE PATH "Output folder for the packed loader")

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
target_include_directories(comet_loader PRIVATE
    "${IMGUI_DIR}" "${IMGUI_DIR}/backends" src
)
target_compile_definitions(comet_loader PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX
    COMET_OBFUSCATE=1
)
target_link_libraries(comet_loader PRIVATE d3d11 dxgi dwmapi shell32 advapi32 user32)
set_target_properties(comet_loader PROPERTIES
    WIN32_EXECUTABLE TRUE
    OUTPUT_NAME "Comet"
    RUNTIME_OUTPUT_DIRECTORY         "${CIYE_DIST_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CIYE_DIST_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CIYE_DIST_DIR}"
    LINK_FLAGS "/MANIFEST:EMBED /MANIFESTUAC:\"level='requireAdministrator' uiAccess='false'\""
)

# ---------------------------------------------------------------------------
# pack_loader: embed comet_pyexec + comet_overlay + pyexec scripts + bsmap
# into Comet.exe.  Runs automatically after all targets are built.
# ---------------------------------------------------------------------------
find_program(PYTHON_EXE NAMES python python3 py)
set(LOADER_PACK_ARGS)
if(PYTHON_EXE)
    list(APPEND LOADER_PACK_ARGS --file "comet_pyexec.dll=$<TARGET_FILE:comet_pyexec>")
    list(APPEND LOADER_PACK_ARGS --file "comet_overlay.dll=$<TARGET_FILE:comet_overlay>")
    list(APPEND LOADER_PACK_ARGS --file "pyexec/cmd.py=${CMAKE_CURRENT_SOURCE_DIR}/pyexec/cmd.py")
    list(APPEND LOADER_PACK_ARGS --file "pyexec/bs_runtime.py=${CMAKE_CURRENT_SOURCE_DIR}/pyexec/bs_runtime.py")
    list(APPEND LOADER_PACK_ARGS --file "bsmap.exe=$<TARGET_FILE:bsmap>")
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
        DEPENDS comet_loader comet_pyexec comet_overlay bsmap
        COMMENT "Embedding payload into ${CIYE_DIST_DIR}/Comet.exe"
        VERBATIM
    )
else()
    message(WARNING "Python not found: Comet.exe will be built without embedded payload")
endif()

# ---------------------------------------------------------------------------
# Optional preview targets (unchanged functionality)
# ---------------------------------------------------------------------------
option(BS_LOADER_PREVIEW  "Non-elevated loader preview"      OFF)
option(BS_MENU_PREVIEW    "Isolated ImGui menu renderer"     OFF)

if(BS_LOADER_PREVIEW)
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
    set_target_properties(comet_loader_preview PROPERTIES WIN32_EXECUTABLE TRUE OUTPUT_NAME "comet_preview")
endif()

if(BS_MENU_PREVIEW)
    get_target_property(MENU_PREVIEW_SOURCES comet_overlay SOURCES)
    list(REMOVE_ITEM MENU_PREVIEW_SOURCES src/overlay.cpp)
    add_executable(comet_menu_preview tools/menu_preview.cpp ${MENU_PREVIEW_SOURCES})
    get_target_property(MENU_PREVIEW_INCLUDES comet_overlay INCLUDE_DIRECTORIES)
    target_include_directories(comet_menu_preview PRIVATE ${MENU_PREVIEW_INCLUDES})
    target_compile_definitions(comet_menu_preview PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(comet_menu_preview PRIVATE d3d11 dxgi d3dcompiler dcomp dwmapi user32 windowscodecs ole32)
endif()
