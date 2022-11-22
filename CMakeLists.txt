cmake_minimum_required(VERSION 3.14)
project(Megapixels C)

set(CMAKE_C_STANDARD 11)

# Use the package PkgConfig to detect GTK+ headers/library files
FIND_PACKAGE(PkgConfig REQUIRED)
PKG_CHECK_MODULES(GTK3 REQUIRED gtk+-3.0)

# Setup CMake to use GTK+, tell the compiler where to look for headers
# and to the linker where to look for libraries
INCLUDE_DIRECTORIES(${GTK3_INCLUDE_DIRS})
LINK_DIRECTORIES(${GTK3_LIBRARY_DIRS})

# Add other flags to the compiler
ADD_DEFINITIONS(${GTK3_CFLAGS_OTHER})

add_executable(megapixels src/camera.c src/camera.h src/camera_config.c src/camera_config.h src/device.c src/device.h
        src/flash.c src/flash.h src/gl_util.c src/gl_util.h src/gles2_debayer.c src/gles2_debayer.h src/ini.c src/ini.h
        src/io_pipeline.c src/io_pipeline.h src/main.c src/main.h src/matrix.c src/matrix.h src/mode.c src/mode.h
        src/pipeline.c src/pipeline.h src/process_pipeline.c src/process_pipeline.h src/zbar_pipeline.c src/zbar_pipeline.h)
target_link_libraries(megapixels ${GTK3_LIBRARIES})

add_executable(megapixels-config-test tools/config_test.c src/camera_config.c src/camera_config.h src/ini.c src/ini.h src/device.c src/device.h src/mode.h src/mode.c src/matrix.h src/matrix.c)
target_include_directories(megapixels-config-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(megapixels-config-test ${GTK3_LIBRARIES})
