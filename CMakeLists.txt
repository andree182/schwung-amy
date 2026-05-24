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

# Header search paths
include_directories(
    src/dsp
    src/dsp/amy/src
)

# Find sources
set(AMY_SOURCES
    src/dsp/amy/src/algorithms.c
    src/dsp/amy/src/amy.c
    src/dsp/amy/src/envelope.c
    src/dsp/amy/src/parse.c
    src/dsp/amy/src/filters.c
    src/dsp/amy/src/oscillators.c
    src/dsp/amy/src/pcm.c
    src/dsp/amy/src/interp_partials.c
    src/dsp/amy/src/custom.c
    src/dsp/amy/src/delay.c
    src/dsp/amy/src/log2_exp2.c
    src/dsp/amy/src/patches.c
    src/dsp/amy/src/transfer.c
    src/dsp/amy/src/sequencer.c
    src/dsp/amy/src/instrument.c
    src/dsp/amy/src/amy_midi.c
    src/dsp/amy/src/api.c
    src/dsp/amy/src/midi_mappings.c
    src/dsp/amy/src/cv_trigger.c
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
