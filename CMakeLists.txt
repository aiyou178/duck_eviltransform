cmake_minimum_required(VERSION 3.16)
project(duck_eviltransform VERSION 0.1 LANGUAGES CXX)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(DUCKDB_PLATFORM "" CACHE STRING "DuckDB platform from PRAGMA platform")
if(NOT DUCKDB_PLATFORM)
  if(APPLE)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
      set(DUCKDB_PLATFORM osx_arm64)
    else()
      set(DUCKDB_PLATFORM osx_amd64)
    endif()
  elseif(WIN32)
    set(DUCKDB_PLATFORM windows_amd64)
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    set(DUCKDB_PLATFORM linux_arm64)
  else()
    set(DUCKDB_PLATFORM linux_amd64)
  endif()
endif()

add_library(eviltransform SHARED src/extension.cpp src/jenks.cpp)
target_compile_features(eviltransform PRIVATE cxx_std_17)
target_compile_definitions(eviltransform PRIVATE DUCKDB_EXTENSION_NAME=eviltransform)
target_include_directories(eviltransform PRIVATE third_party/duckdb src "${CMAKE_CURRENT_BINARY_DIR}")
set_target_properties(eviltransform PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
if(MSVC)
  target_compile_options(eviltransform PRIVATE /W4)
else()
  target_compile_options(eviltransform PRIVATE -Wall -Wextra -Wpedantic)
endif()

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/sql/eviltransform.sql")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/sql/eviltransform.sql" EVILTRANSFORM_SQL)
configure_file(src/sql.hpp.in sql.hpp @ONLY)
add_custom_command(TARGET eviltransform POST_BUILD
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/append_metadata.py"
    "$<TARGET_FILE:eviltransform>" "${CMAKE_CURRENT_BINARY_DIR}/eviltransform.duckdb_extension"
    "${DUCKDB_PLATFORM}" "${PROJECT_VERSION}"
  VERBATIM)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/eviltransform.duckdb_extension" DESTINATION lib)
