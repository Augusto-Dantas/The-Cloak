cmake_minimum_required(VERSION 3.20)
project(DXGIOverlay LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ─── Detect cross-compile environment ─────────────────────────────────────────
set(IS_MINGW FALSE)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU" AND WIN32)
    set(IS_MINGW TRUE)
    message(STATUS "MinGW cross-compile detected")
endif()

# ─── Windows SDK / DirectX headers ────────────────────────────────────────────
# Priority order for header search:
#   1. DXSDK_DIR env var (set this to your SDK path)
#   2. wine-development headers (apt install wine64-dev)
#   3. Bundled mingw-w64 headers (may be incomplete)
if(IS_MINGW)
    set(DX_INCLUDE_DIRS "")

    # 1. User-supplied SDK path: cmake ... -DDXSDK_DIR=/path/to/winsdk/Include
    if(DEFINED DXSDK_DIR)
        list(APPEND DX_INCLUDE_DIRS "${DXSDK_DIR}/um" "${DXSDK_DIR}/shared" "${DXSDK_DIR}/winrt")
        message(STATUS "Using Windows SDK headers from: ${DXSDK_DIR}")

    # 2. Wine development headers
    elseif(EXISTS "/usr/include/wine/windows/dxgi.h")
        list(APPEND DX_INCLUDE_DIRS "/usr/include/wine/windows")
        message(STATUS "Using wine-dev DirectX headers")

    else()
        message(WARNING
            "Could not find complete DirectX headers.\n"
            "Install wine64-dev:  sudo apt install wine64-dev\n"
            "Or pass your Windows SDK path: cmake -DDXSDK_DIR=/path/to/sdk/Include/10.x ...")
    endif()
endif()

# ─── Compiler flags ───────────────────────────────────────────────────────────
if(MSVC)
    add_compile_options(/W4 /WX- /MP /utf-8)
    add_compile_definitions(
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _WIN32_WINNT=0x0A00
        UNICODE
        _UNICODE
    )
elseif(IS_MINGW)
    add_compile_options(
        -Wall -Wextra
        -Wno-unknown-pragmas
        -municode
    )
    add_compile_definitions(
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _WIN32_WINNT=0x0A00
        UNICODE
        _UNICODE
    )
    add_link_options(-municode -static-libgcc -static-libstdc++)
endif()

# ─── dxgi_hook.dll ────────────────────────────────────────────────────────────
add_library(dxgi_hook SHARED src/dxgi_hook.cpp)

target_include_directories(dxgi_hook PRIVATE ${DX_INCLUDE_DIRS})

target_link_libraries(dxgi_hook PRIVATE
    dxgi
    d3d11
    d3d12
    dcomp
    dwrite
    d2d1
    dxguid
    uuid
)

if(IS_MINGW)
    set_target_properties(dxgi_hook PROPERTIES PREFIX "")
endif()

set_target_properties(dxgi_hook PROPERTIES
    OUTPUT_NAME "dxgi_hook"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# ─── launcher.exe ─────────────────────────────────────────────────────────────
add_executable(launcher src/launcher.cpp)

target_include_directories(launcher PRIVATE ${DX_INCLUDE_DIRS})

target_link_libraries(launcher PRIVATE uuid)

if(IS_MINGW)
    target_link_options(launcher PRIVATE -mconsole)
endif()

set_target_properties(launcher PROPERTIES
    OUTPUT_NAME "DXGIOverlayLauncher"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    WIN32_EXECUTABLE FALSE
)

# ─── Copy DLL next to launcher after build ────────────────────────────────────
add_custom_command(TARGET launcher POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:dxgi_hook>
        $<TARGET_FILE_DIR:launcher>/dxgi_hook.dll
    COMMENT "Copying dxgi_hook.dll alongside launcher"
)

# ─── Build summary ────────────────────────────────────────────────────────────
message(STATUS "")
message(STATUS "── DXGIOverlay build config ──────────────────")
message(STATUS "  Compiler   : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  System     : ${CMAKE_SYSTEM_NAME}")
message(STATUS "  Build type : ${CMAKE_BUILD_TYPE}")
if(DX_INCLUDE_DIRS)
message(STATUS "  DX headers : ${DX_INCLUDE_DIRS}")
endif()
message(STATUS "  Output dir : ${CMAKE_BINARY_DIR}/bin")
message(STATUS "──────────────────────────────────────────────")
message(STATUS "")
