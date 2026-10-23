cmake_minimum_required(VERSION 3.15)
cmake_policy(SET CMP0091 NEW)

set(PROJECT_NAME ScoreTransfer)
project("${PROJECT_NAME}" C CXX RC)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INSTALL_PREFIX "${CMAKE_CURRENT_BINARY_DIR}/install" CACHE PATH "" FORCE)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
option(FORCE_PDB "Generate PDB file for Release builds" OFF)
add_definitions(-DWINVER=0x0601 -D_WIN32_WINNT=0x0601)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "ScoreTransfer must be built for Windows using MSVC.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "ScoreTransfer targets th123.exe (32-bit). Configure an x86/Win32 build.")
endif()

set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /Brepro")
set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} /Brepro")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} /Brepro")
if(FORCE_PDB)
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /DEBUG /OPT:REF /OPT:ICF")
    set(CMAKE_MODULE_LINKER_FLAGS_RELEASE "${CMAKE_MODULE_LINKER_FLAGS_RELEASE} /DEBUG /OPT:REF /OPT:ICF")
    set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} /DEBUG /OPT:REF /OPT:ICF")
endif()

# Keep the SokuDev/ModTemplate SokuLib submodule, but compile only its version-hash
# translation unit. ScoreTransfer uses no other SokuLib API. Building the whole
# static library unnecessarily compiles every DLL proxy and its test programs;
# the pinned proxy source currently fails around WS2_32_DLL.hpp on the Actions
# toolchain, which is unrelated to this module's needs.
set(SOKULIB_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/SokuLib")
if(NOT EXISTS "${SOKULIB_SOURCE_DIR}/src/Hash.cpp" OR NOT EXISTS "${SOKULIB_SOURCE_DIR}/src/Hash.hpp")
    message(FATAL_ERROR "SokuLib submodule is missing or incomplete. Run: git submodule update --init --recursive")
endif()
set(SOKULIB_HASH_SOURCE "${SOKULIB_SOURCE_DIR}/src/Hash.cpp")

include(FetchContent)
FetchContent_Declare(imgui_source
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.90.9.tar.gz)
FetchContent_GetProperties(imgui_source)
if(NOT imgui_source_POPULATED)
    FetchContent_Populate(imgui_source)
endif()

add_library(scoretransfer_imgui STATIC
    "${imgui_source_SOURCE_DIR}/imgui.cpp"
    "${imgui_source_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_source_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_source_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_source_SOURCE_DIR}/backends/imgui_impl_dx9.cpp")
target_include_directories(scoretransfer_imgui PUBLIC
    "${imgui_source_SOURCE_DIR}"
    "${imgui_source_SOURCE_DIR}/backends")
target_compile_definitions(scoretransfer_imgui PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(scoretransfer_imgui PUBLIC d3d9)

# Same SQLite amalgamation strategy as the previous ScoreTransfer version / OIOverlay.
set(SQLITE_VERSION 3530400)
set(SQLITE_DIR "${CMAKE_BINARY_DIR}/sqlite")
set(SQLITE_ARCHIVE "${SQLITE_DIR}/sqlite-amalgamation-${SQLITE_VERSION}.zip")
set(SQLITE_ROOT "${SQLITE_DIR}/sqlite-amalgamation-${SQLITE_VERSION}")
if(NOT EXISTS "${SQLITE_ROOT}/sqlite3.c")
    file(MAKE_DIRECTORY "${SQLITE_DIR}")
    file(DOWNLOAD
        "https://www.sqlite.org/2026/sqlite-amalgamation-${SQLITE_VERSION}.zip"
        "${SQLITE_ARCHIVE}"
        SHOW_PROGRESS STATUS SQLITE_DOWNLOAD_STATUS)
    list(GET SQLITE_DOWNLOAD_STATUS 0 SQLITE_DOWNLOAD_RESULT)
    if(NOT SQLITE_DOWNLOAD_RESULT EQUAL 0)
        message(FATAL_ERROR "SQLite download failed: ${SQLITE_DOWNLOAD_STATUS}")
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E tar xf "${SQLITE_ARCHIVE}"
        WORKING_DIRECTORY "${SQLITE_DIR}" RESULT_VARIABLE SQLITE_EXTRACT_RESULT)
    if(NOT SQLITE_EXTRACT_RESULT EQUAL 0)
        message(FATAL_ERROR "SQLite extraction failed.")
    endif()
endif()
add_library(scoretransfer_sqlite STATIC "${SQLITE_ROOT}/sqlite3.c")
target_include_directories(scoretransfer_sqlite PUBLIC "${SQLITE_ROOT}")
target_compile_definitions(scoretransfer_sqlite PRIVATE
    SQLITE_THREADSAFE=1 SQLITE_DEFAULT_MEMSTATUS=0 SQLITE_OMIT_LOAD_EXTENSION=1)

add_library("${PROJECT_NAME}" MODULE
    src/main.cpp
    src/SharedState.cpp
    src/DatabaseWorker.cpp
    src/D3D9Hook.cpp
    src/CounterRenderer.cpp
    src/ScoreTransfer.hpp
    src/version.rc
    "${SOKULIB_HASH_SOURCE}")
set_target_properties("${PROJECT_NAME}" PROPERTIES PREFIX "" OUTPUT_NAME "${PROJECT_NAME}")
target_compile_options("${PROJECT_NAME}" PRIVATE /W4 /EHsc /utf-8 /Zi)
target_compile_definitions("${PROJECT_NAME}" PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX DIRECTINPUT_VERSION=0x0800
    CURL_STATICLIB _CRT_SECURE_NO_WARNINGS $<$<CONFIG:Debug>:_DEBUG>)
target_include_directories("${PROJECT_NAME}" PRIVATE
    include
    "${SOKULIB_SOURCE_DIR}/src"
    "${SOKULIB_SOURCE_DIR}/directx"
    "src/${PROJECT_NAME}/")
target_link_directories("${PROJECT_NAME}" PRIVATE lib)
target_link_libraries("${PROJECT_NAME}" PRIVATE
    scoretransfer_sqlite scoretransfer_imgui
    shlwapi d3d9 d3dx9 user32 ole32 windowscodecs advapi32)

# The optional standalone route consists of a small EXE and an injected bootstrap DLL.
# Both entry routes load the same ScoreTransfer target above; these targets do not
# duplicate the renderer or database implementation.
add_executable(ScoreTransferLoader standalone/ScoreTransferLauncher.cpp)
set_target_properties(ScoreTransferLoader PROPERTIES OUTPUT_NAME "ScoreTransferLoader")
target_compile_options(ScoreTransferLoader PRIVATE /W4 /EHsc /utf-8)
target_compile_definitions(ScoreTransferLoader PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(ScoreTransferLoader PRIVATE user32)

add_library(ScoreTransferBootstrap MODULE standalone/ScoreTransferBootstrap.cpp)
set_target_properties(ScoreTransferBootstrap PROPERTIES PREFIX "" OUTPUT_NAME "ScoreTransferBootstrap")
target_compile_options(ScoreTransferBootstrap PRIVATE /W4 /EHsc /utf-8)
target_compile_definitions(ScoreTransferBootstrap PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_link_libraries(ScoreTransferBootstrap PRIVATE user32)

# Install both loading modes side-by-side in a single artifact tree.
#
# artifact/ScoreTransfer/        -> standard SWRSToys module
# artifact/ScoreTransfer-loader/ -> standalone EXE + bootstrap + the same plugin DLL
# bg.png stays an external RGBA PNG in both directories; CounterRenderer.cpp and the
# uploaded bg.png asset are deliberately unchanged so their positions/alpha are preserved.
install(TARGETS "${PROJECT_NAME}"
    RUNTIME DESTINATION "ScoreTransfer"
    LIBRARY DESTINATION "ScoreTransfer")
install(FILES "$<TARGET_FILE:${PROJECT_NAME}>" DESTINATION "ScoreTransfer-loader")
install(TARGETS ScoreTransferBootstrap
    RUNTIME DESTINATION "ScoreTransfer-loader"
    LIBRARY DESTINATION "ScoreTransfer-loader")
install(TARGETS ScoreTransferLoader RUNTIME DESTINATION "ScoreTransfer-loader")

install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/bg.png" DESTINATION "ScoreTransfer")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/bg.png" DESTINATION "ScoreTransfer-loader")
install(FILES README.md THIRD_PARTY_NOTICES.md DESTINATION "ScoreTransfer")
install(FILES THIRD_PARTY_NOTICES.md DESTINATION "ScoreTransfer-loader")
install(FILES LICENSE DESTINATION "ScoreTransfer" OPTIONAL)
install(FILES LICENSE DESTINATION "ScoreTransfer-loader" OPTIONAL)
install(FILES "$<TARGET_PDB_FILE:${PROJECT_NAME}>" DESTINATION "ScoreTransfer" OPTIONAL)
install(FILES "$<TARGET_PDB_FILE:${PROJECT_NAME}>" DESTINATION "ScoreTransfer-loader" OPTIONAL)
