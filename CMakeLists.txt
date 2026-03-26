cmake_minimum_required(VERSION 3.15)

project(endstone_backupper LANGUAGES C CXX)
enable_testing()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(UNIX AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR
        "Endstone plugins must be configured with Clang on Linux. "
        "Re-run CMake with -DCMAKE_CXX_COMPILER=clang++."
    )
endif()

include(FetchContent)

set(LIBCXX_ROOT "${CMAKE_SOURCE_DIR}/.deps/local-libcxx/root")
set(LIBCXX_INCLUDE_DIR "")
set(LIBCXX_LIBRARY_DIR "")

if(UNIX)
    if(EXISTS "/usr/include/c++/v1")
        set(LIBCXX_INCLUDE_DIR "/usr/include/c++/v1")
    elseif(EXISTS "${LIBCXX_ROOT}/usr/include/c++/v1")
        set(LIBCXX_INCLUDE_DIR "${LIBCXX_ROOT}/usr/include/c++/v1")
        set(LIBCXX_LIBRARY_DIR "${LIBCXX_ROOT}/usr/lib/llvm-18/lib")
    else()
        message(FATAL_ERROR
            "Unable to find libc++ headers. Install system libc++ or run scripts/setup-local-libcxx.sh."
        )
    endif()

    include_directories(SYSTEM BEFORE "${LIBCXX_INCLUDE_DIR}")
    if(LIBCXX_LIBRARY_DIR)
        link_directories("${LIBCXX_LIBRARY_DIR}")
    endif()
endif()

FetchContent_Declare(
    endstone
    GIT_REPOSITORY https://github.com/EndstoneMC/endstone.git
    GIT_TAG v0.11.2
)

FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_Declare(
    croncpp
    GIT_REPOSITORY https://github.com/mariusbancila/croncpp.git
    GIT_TAG v2023.03.30
    GIT_SHALLOW 1
)

FetchContent_Declare(
    miniz
    GIT_REPOSITORY https://github.com/richgel999/miniz.git
    GIT_TAG 3.0.2
    GIT_SHALLOW 1
)

set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
set(INSTALL_PROJECT OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(endstone nlohmann_json croncpp miniz)
set_target_properties(miniz PROPERTIES POSITION_INDEPENDENT_CODE ON)

endstone_add_plugin(
    ${PROJECT_NAME}
    src/archive_writer.cpp
    src/backup_manager.cpp
    src/backupper_config.cpp
    src/backupper_plugin.cpp
)
target_include_directories(${PROJECT_NAME} PRIVATE include ${croncpp_SOURCE_DIR}/include)
target_link_libraries(${PROJECT_NAME} PRIVATE nlohmann_json::nlohmann_json miniz)
