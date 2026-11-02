cmake_minimum_required(VERSION 3.20)
project(comp3005_db LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

enable_testing()

# Vendored single-header libraries (see DECISIONS.md). SYSTEM keeps their
# warnings out of our builds.
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

add_library(doctest INTERFACE)
target_include_directories(doctest SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

add_subdirectory(modules/common)
add_subdirectory(modules/storage)
add_subdirectory(modules/exec)

# Executables and their CLI support libraries (after every module).
add_subdirectory(apps)
