cmake_minimum_required(VERSION 3.25)

project(ClippyStorageHost VERSION 0.5.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_library(clippy_sqlite STATIC vendor/sqlite/sqlite3.c)
target_include_directories(clippy_sqlite PUBLIC vendor/sqlite)
target_compile_definitions(clippy_sqlite PRIVATE
    SQLITE_DQS=0
    SQLITE_DEFAULT_FOREIGN_KEYS=1
    SQLITE_ENABLE_API_ARMOR
    SQLITE_LIKE_DOESNT_MATCH_BLOBS
    SQLITE_OMIT_DEPRECATED
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_OMIT_SHARED_CACHE
    SQLITE_THREADSAFE=1
    SQLITE_TRUSTED_SCHEMA=0
)
if(MSVC)
    target_compile_options(clippy_sqlite PRIVATE /W0)
endif()

add_executable(ClippyStorageHost
    src/config.cpp
    src/database.cpp
    src/legacy_migration.cpp
    src/main.cpp
    src/postgres.cpp
    src/util.cpp
)

target_include_directories(ClippyStorageHost PRIVATE
    src
    vendor/httplib
    vendor/nlohmann
)

target_compile_definitions(ClippyStorageHost PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    CPPHTTPLIB_NO_DEFAULT_USER_AGENT
)

target_link_libraries(ClippyStorageHost PRIVATE clippy_sqlite)
if(WIN32)
    target_link_libraries(ClippyStorageHost PRIVATE bcrypt ws2_32)
elseif(UNIX)
    find_package(OpenSSL REQUIRED)
    target_link_libraries(ClippyStorageHost PRIVATE dl OpenSSL::Crypto)
endif()

if(MSVC)
    target_compile_options(ClippyStorageHost PRIVATE /W4 /permissive- /EHsc /utf-8)
endif()

set_target_properties(ClippyStorageHost PROPERTIES
    OUTPUT_NAME "ClippyStorageHost"
)

