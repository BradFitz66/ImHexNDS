cmake_minimum_required(VERSION 3.20)
set(CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
project(ImHexPlugins)

include(ImHexSDK)

# All plugins land in the build root next to each other
set(IMHEX_MAIN_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

file(GLOB PLUGIN_SOURCES "${CMAKE_SOURCE_DIR}/source/*.cpp")

foreach(SOURCE_FILE ${PLUGIN_SOURCES})
    get_filename_component(PLUGIN_NAME ${SOURCE_FILE} NAME_WE)
    set(PLUGIN_GEN_DIR "${CMAKE_BINARY_DIR}/_plugins/${PLUGIN_NAME}")
    file(MAKE_DIRECTORY "${PLUGIN_GEN_DIR}")
    configure_file(
        "${CMAKE_SOURCE_DIR}/cmake/PluginEntry.cmake.in"
        "${PLUGIN_GEN_DIR}/CMakeLists.txt"
        @ONLY
    )
    add_subdirectory(
        "${PLUGIN_GEN_DIR}"
        "${PLUGIN_GEN_DIR}/out"
    )
endforeach()
