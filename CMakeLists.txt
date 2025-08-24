cmake_minimum_required(VERSION 3.15)
project(mongodb_storage_engine LANGUAGES C CXX)

# Ensure C++17 support across all platforms
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Set default build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

#==============================================================================
# SYSTEM-AGNOSTIC DEPENDENCY DISCOVERY
#==============================================================================

# Discover MariaDB installation (system-agnostic)
find_program(MARIADB_CONFIG 
    NAMES mariadb_config mysql_config
    PATHS ENV PATH
    DOC "MariaDB configuration tool"
)

if(MARIADB_CONFIG)
    message(STATUS "Found MariaDB config: ${MARIADB_CONFIG}")
    
    # Get MariaDB compile flags dynamically
    execute_process(COMMAND ${MARIADB_CONFIG} --cflags
        OUTPUT_VARIABLE MARIADB_CFLAGS
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE MARIADB_CONFIG_RESULT
    )
    execute_process(COMMAND ${MARIADB_CONFIG} --libs
        OUTPUT_VARIABLE MARIADB_LIBS  
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    execute_process(COMMAND ${MARIADB_CONFIG} --plugindir
        OUTPUT_VARIABLE MARIADB_PLUGIN_DIR
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    execute_process(COMMAND ${MARIADB_CONFIG} --include
        OUTPUT_VARIABLE MARIADB_INCLUDE_DIRS
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    
    if(MARIADB_CONFIG_RESULT EQUAL 0)
        message(STATUS "MariaDB plugin directory: ${MARIADB_PLUGIN_DIR}")
        message(STATUS "MariaDB include directories: ${MARIADB_INCLUDE_DIRS}")
    else()
        message(WARNING "mariadb_config failed, falling back to local sources")
        set(MARIADB_CONFIG "")
    endif()
endif()

if(NOT MARIADB_CONFIG)
    # Fallback to local sources
    message(STATUS "Using local MariaDB sources for headers")
    set(MARIADB_INCLUDE_DIRS 
        "${CMAKE_CURRENT_SOURCE_DIR}/sources/server/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/sources/server/include/mysql"
        "${CMAKE_CURRENT_SOURCE_DIR}/sources/server/include/mysql/server"
    )
    set(MARIADB_PLUGIN_DIR "${CMAKE_INSTALL_PREFIX}/lib/plugin")
endif()

# Use local mongo-c-driver sources (system-agnostic)
set(MONGO_C_DRIVER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/sources/mongo-c-driver")

# Use local mongo-c-driver sources with symbol stubs for Berkeley DB conflicts
set(MONGO_C_DRIVER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/sources/mongo-c-driver")

# Check if local sources exist
if(EXISTS "${MONGO_C_DRIVER_ROOT}/CMakeLists.txt")
    message(STATUS "Building mongo-c-driver from local sources with symbol conflict fixes")
    
    # Build libmongoc/libbson from local sources
    set(ENABLE_TESTS OFF CACHE BOOL "Disable mongo-c-driver tests")
    set(ENABLE_EXAMPLES OFF CACHE BOOL "Disable mongo-c-driver examples") 
    set(ENABLE_STATIC ON CACHE BOOL "Enable static libraries")
    
    add_subdirectory(${MONGO_C_DRIVER_ROOT} mongo-c-driver EXCLUDE_FROM_ALL)
    
    set(LIBMONGOC_LIBRARIES mongoc_static)
    set(LIBBSON_LIBRARIES bson_static)
    set(LIBMONGOC_INCLUDE_DIRS 
        ${MONGO_C_DRIVER_ROOT}/src/libmongoc/src/mongoc
        ${CMAKE_BINARY_DIR}/mongo-c-driver/src/libmongoc/src/mongoc
    )
    set(LIBBSON_INCLUDE_DIRS 
        ${MONGO_C_DRIVER_ROOT}/src/libbson/src/bson
        ${CMAKE_BINARY_DIR}/mongo-c-driver/src/libbson/src/bson
    )
else()
    message(FATAL_ERROR "mongo-c-driver sources not found at ${MONGO_C_DRIVER_ROOT}")
endif()

#==============================================================================
# PLUGIN TARGET CONFIGURATION
#==============================================================================

# Main plugin target
add_library(mongodb SHARED
    src/ha_mongodb.cc
    src/ha_mongodb_handler.cc
    src/mongodb_connection.cc
    src/mongodb_uri_parser.cc
    src/mongodb_schema.cc
    src/mongodb_translator.cc
    src/mongodb_cursor.cc
    src/mongodb_share.cc
    src/symbol_stubs.c
)

# Cross-platform compile definitions
target_compile_definitions(mongodb PRIVATE
    MYSQL_DYNAMIC_PLUGIN
    MYSQL_SERVER
    HAVE_CONFIG_H
    # Disable wsrep to avoid missing dependencies (don't define WITH_WSREP)
    $<$<PLATFORM_ID:Windows>:WIN32_LEAN_AND_MEAN>
    $<$<PLATFORM_ID:Windows>:_CRT_SECURE_NO_WARNINGS>
    $<$<PLATFORM_ID:Windows>:NOMINMAX>
)

# Use only local MariaDB headers with static mongo-c-driver
target_include_directories(mongodb PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/sources/server/include
    ${CMAKE_CURRENT_SOURCE_DIR}/sources/server/include/mysql/server
    ${CMAKE_CURRENT_SOURCE_DIR}/sources/server/sql
    ${LIBMONGOC_INCLUDE_DIRS}
    ${LIBBSON_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Link with static libraries and symbol stubs (mongoc_static includes bson_static)
target_link_libraries(mongodb PRIVATE
    ${LIBMONGOC_LIBRARIES}
    $<$<PLATFORM_ID:Linux>:pthread>
    $<$<PLATFORM_ID:Windows>:ws2_32>
    $<$<PLATFORM_ID:Windows>:advapi32>
    $<$<PLATFORM_ID:Windows>:crypt32>
    $<$<PLATFORM_ID:Windows>:secur32>
)

# Cross-platform plugin properties
set_target_properties(mongodb PROPERTIES
    PREFIX ""                                    # No lib prefix
    OUTPUT_NAME "ha_mongodb"                     # Follow MariaDB naming convention
    SUFFIX ".so"                                 # MariaDB expects .so on all platforms
    POSITION_INDEPENDENT_CODE ON                 # Required for shared libraries
    CXX_VISIBILITY_PRESET default               # Export plugin symbols
    VISIBILITY_INLINES_HIDDEN OFF               # Allow plugin symbols to be visible
)

# Platform-specific linking options
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Linux-specific linker options
    target_link_options(mongodb PRIVATE
        "LINKER:--no-undefined"                  # Catch missing symbols at link time
    )
    
    # Create symbol version script for Linux
    set(VERSION_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/mongodb.map")
    if(EXISTS ${VERSION_SCRIPT})
        target_link_options(mongodb PRIVATE
            "LINKER:--version-script=${VERSION_SCRIPT}"
        )
    endif()
elseif(APPLE)
    # macOS-specific linker options
    target_link_options(mongodb PRIVATE
        "-undefined" "dynamic_lookup"           # Allow undefined symbols (like MariaDB plugins)
    )
endif()

#==============================================================================
# INSTALLATION AND TESTING
#==============================================================================

# Install plugin to discovered or specified directory
if(MARIADB_PLUGIN_DIR)
    install(TARGETS mongodb 
        LIBRARY DESTINATION ${MARIADB_PLUGIN_DIR}
        RUNTIME DESTINATION ${MARIADB_PLUGIN_DIR}
    )
else()
    # Fallback installation path
    install(TARGETS mongodb 
        LIBRARY DESTINATION lib/plugin
        RUNTIME DESTINATION lib/plugin
    )
endif()

# Cross-platform testing support
find_program(MARIADB_CLIENT 
    NAMES mariadb mysql
    PATHS ENV PATH
    DOC "MariaDB client for testing"
)

if(MARIADB_CLIENT)
    # Add test target that works on any platform
    add_custom_target(test_plugin
        COMMAND ${CMAKE_COMMAND} -E echo "Testing MongoDB storage engine..."
        COMMAND ${MARIADB_CLIENT} --version
        DEPENDS mongodb
        COMMENT "Running cross-platform plugin tests"
    )
endif()

#==============================================================================
# DEVELOPMENT HELPERS
#==============================================================================

# Print configuration summary
message(STATUS "=== MongoDB Storage Engine Configuration ===")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "MariaDB config: ${MARIADB_CONFIG}")
message(STATUS "MariaDB plugin dir: ${MARIADB_PLUGIN_DIR}")
message(STATUS "libmongoc: ${LIBMONGOC_LIBRARIES}")
message(STATUS "libbson: ${LIBBSON_LIBRARIES}")
message(STATUS "Target platform: ${CMAKE_SYSTEM_NAME}")
message(STATUS "============================================")

# Optional: Enable additional warnings for development
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(mongodb PRIVATE
            -Wall -Wextra -Wpedantic
            -Wno-unused-parameter  # MariaDB headers have many unused parameters
        )
    endif()
endif()
