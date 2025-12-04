cmake_minimum_required(VERSION 3.5)

set(TARGET_NAME cronjob)
set(CRONJOB_VERSION "2025120401")

# Add ccronexpr library
add_library(ccronexpr STATIC ccronexpr/ccronexpr.c)
target_include_directories(ccronexpr PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/ccronexpr)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)
project(${TARGET_NAME})

# Set extension sources
set(EXTENSION_SOURCES src/cronjob_extension.cpp)
include_directories(src/include ccronexpr)

# Build extension
build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES} ccronexpr/ccronexpr.c)
build_loadable_extension(${TARGET_NAME} "" ${EXTENSION_SOURCES} ccronexpr/ccronexpr.c)

# Add version definition
target_compile_definitions(${EXTENSION_NAME} PRIVATE CRONJOB_VERSION="${CRONJOB_VERSION}")
target_compile_definitions(${LOADABLE_EXTENSION_NAME} PRIVATE CRONJOB_VERSION="${CRONJOB_VERSION}")

target_link_libraries(${EXTENSION_NAME})
target_link_libraries(${LOADABLE_EXTENSION_NAME})

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
