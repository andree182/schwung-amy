cmake_minimum_required(VERSION 3.10)
project(amy-move-plugin CXX C)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Definitions
add_definitions(
    -DLINUX=1
    -DNDEBUG=1
    -DAMY_WAVETABLE=1
    -DAMY_MOVE=1
    -DAMY_NO_MINIAUDIO=1
)

# Define paths
set(AMY_ORIG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/dsp/amy/src")
set(AMY_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/amy_src")

# Copy the entire submodule source folder to the build directory
file(COPY "${AMY_ORIG_DIR}/" DESTINATION "${AMY_BUILD_DIR}")

file(APPEND "${AMY_BUILD_DIR}/amy.h" "
/* --- CMake Injected Overrides --- */
#undef AMY_SAMPLE_RATE
#undef AMY_BLOCK_SIZE
#undef BLOCK_SIZE_BITS
#define AMY_SAMPLE_RATE 44100
#define AMY_BLOCK_SIZE 128
#define BLOCK_SIZE_BITS 7 // log2 of BLOCK_SIZE
")

# Header search paths (pointing to the new build directory)
include_directories(
    src/dsp
    ${AMY_BUILD_DIR}
)

# Find sources (pointing to the copied files in the build directory)
set(AMY_SOURCES
    ${AMY_BUILD_DIR}/algorithms.c
    ${AMY_BUILD_DIR}/amy.c
    ${AMY_BUILD_DIR}/envelope.c
    ${AMY_BUILD_DIR}/parse.c
    ${AMY_BUILD_DIR}/filters.c
    ${AMY_BUILD_DIR}/oscillators.c
    ${AMY_BUILD_DIR}/pcm.c
    ${AMY_BUILD_DIR}/interp_partials.c
    ${AMY_BUILD_DIR}/custom.c
    ${AMY_BUILD_DIR}/delay.c
    ${AMY_BUILD_DIR}/log2_exp2.c
    ${AMY_BUILD_DIR}/patches.c
    ${AMY_BUILD_DIR}/transfer.c
    ${AMY_BUILD_DIR}/sequencer.c
    ${AMY_BUILD_DIR}/instrument.c
    ${AMY_BUILD_DIR}/amy_midi.c
    ${AMY_BUILD_DIR}/api.c
    ${AMY_BUILD_DIR}/midi_mappings.c
    ${AMY_BUILD_DIR}/cv_trigger.c
)

set(PLUGIN_SOURCES
    src/dsp/amy_plugin.cpp
)

# Combine all source files
set(ALL_SOURCES
    ${AMY_SOURCES}
    ${PLUGIN_SOURCES}
)

# Build shared library (dsp.so)
add_library(amy-move-plugin SHARED ${ALL_SOURCES})

# Target properties to output dsp.so directly
set_target_properties(amy-move-plugin PROPERTIES
    PREFIX ""
    SUFFIX ".so"
    OUTPUT_NAME "dsp"
)

# Link standard libraries
target_link_libraries(amy-move-plugin PRIVATE
    pthread
    rt
    dl
    m
)

# Link libstdc++ and libgcc statically to avoid GLIBCXX version issues on the Move
target_link_options(amy-move-plugin PRIVATE "-static-libstdc++" "-static-libgcc")
