cmake_minimum_required(VERSION 3.20)
set(CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
project(ImHexPlugin)

include(ImHexSDK)

add_imhex_plugin(
    NAME
        PaletteViewer
    SOURCES
        source/PaletteViewer.cpp
    INCLUDES
        include
        include/stb_image.h
        include/stb_image_write.h
)
