cmake_minimum_required(VERSION 3.22.1)
project(webpkit)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

set(WEBP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/libwebp)
set(WEBP_SRC ${WEBP_ROOT}/src)

# libwebp ships full encode + decode source here. Each .c file self-guards its
# SIMD body with WEBP_USE_* macros, so globbing every variant is safe for any ABI
# (non-applicable variants compile to empty translation units).
file(GLOB WEBP_DEC_SOURCES     ${WEBP_SRC}/dec/*.c)
file(GLOB WEBP_DSP_SOURCES     ${WEBP_SRC}/dsp/*.c)
file(GLOB WEBP_ENC_SOURCES     ${WEBP_SRC}/enc/*.c)
file(GLOB WEBP_UTILS_SOURCES   ${WEBP_SRC}/utils/*.c)
file(GLOB WEBP_DEMUX_SOURCES   ${WEBP_SRC}/demux/*.c)
file(GLOB WEBP_MUX_SOURCES     ${WEBP_SRC}/mux/*.c)
file(GLOB SHARPYUV_SOURCES     ${WEBP_ROOT}/sharpyuv/*.c)

add_library(webpkit SHARED
        ${WEBP_DEC_SOURCES}
        ${WEBP_DSP_SOURCES}
        ${WEBP_ENC_SOURCES}
        ${WEBP_UTILS_SOURCES}
        ${WEBP_DEMUX_SOURCES}
        ${WEBP_MUX_SOURCES}
        ${SHARPYUV_SOURCES}
        WebPYUVDecoder.cpp
)

target_include_directories(webpkit PRIVATE
        ${WEBP_ROOT}
        ${WEBP_SRC}
)

target_compile_definitions(webpkit PRIVATE
        HAVE_PTHREAD
        WEBP_USE_THREAD
)

target_compile_options(webpkit PRIVATE
        -ffunction-sections
        -fdata-sections
        -fomit-frame-pointer
)

target_link_options(webpkit PRIVATE
        -Wl,--gc-sections
)

find_library(log-lib log)
target_link_libraries(webpkit ${log-lib} android)
