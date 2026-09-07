# Build the SonoBus-compatible AOO fork directly from the vendored submodule.
# The essej/aoo fork does not provide a top-level CMake target, so METRO mirrors
# the source list used by SonoBus itself.

set(METRO_AOO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/aoo")

if(NOT EXISTS "${METRO_AOO_ROOT}/lib/src/client.cpp")
    set(DYSEKT_HAS_AOO FALSE)
    message(STATUS "AOO submodule not populated — network audio disabled")
    return()
endif()

include(FetchContent)

set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_ENABLE_FLOAT_API ON CACHE BOOL "" FORCE)

FetchContent_Declare(
    metro_opus
    GIT_REPOSITORY https://github.com/xiph/opus.git
    GIT_TAG v1.5.2
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(metro_opus)

set(METRO_AOO_SOURCES
    ${METRO_AOO_ROOT}/lib/src/SLIP.hpp
    ${METRO_AOO_ROOT}/lib/src/client.cpp
    ${METRO_AOO_ROOT}/lib/src/client.hpp
    ${METRO_AOO_ROOT}/lib/src/codec_opus.cpp
    ${METRO_AOO_ROOT}/lib/src/codec_pcm.cpp
    ${METRO_AOO_ROOT}/lib/src/common.cpp
    ${METRO_AOO_ROOT}/lib/src/common.hpp
    ${METRO_AOO_ROOT}/lib/src/lockfree.hpp
    ${METRO_AOO_ROOT}/lib/src/net_utils.cpp
    ${METRO_AOO_ROOT}/lib/src/net_utils.hpp
    ${METRO_AOO_ROOT}/lib/src/server.cpp
    ${METRO_AOO_ROOT}/lib/src/server.hpp
    ${METRO_AOO_ROOT}/lib/src/sink.cpp
    ${METRO_AOO_ROOT}/lib/src/sink.hpp
    ${METRO_AOO_ROOT}/lib/src/source.cpp
    ${METRO_AOO_ROOT}/lib/src/source.hpp
    ${METRO_AOO_ROOT}/lib/src/sync.cpp
    ${METRO_AOO_ROOT}/lib/src/sync.hpp
    ${METRO_AOO_ROOT}/lib/src/time.cpp
    ${METRO_AOO_ROOT}/lib/src/time.hpp
    ${METRO_AOO_ROOT}/lib/src/time_dll.hpp
    ${METRO_AOO_ROOT}/lib/aoo/aoo.h
    ${METRO_AOO_ROOT}/lib/aoo/aoo.hpp
    ${METRO_AOO_ROOT}/lib/aoo/aoo_net.h
    ${METRO_AOO_ROOT}/lib/aoo/aoo_net.hpp
    ${METRO_AOO_ROOT}/lib/aoo/aoo_opus.h
    ${METRO_AOO_ROOT}/lib/aoo/aoo_pcm.h
    ${METRO_AOO_ROOT}/lib/aoo/aoo_types.h
    ${METRO_AOO_ROOT}/lib/aoo/aoo_utils.hpp
    ${METRO_AOO_ROOT}/deps/md5/md5.c
    ${METRO_AOO_ROOT}/deps/md5/md5.h
    ${METRO_AOO_ROOT}/deps/oscpack/osc/OscOutboundPacketStream.cpp
    ${METRO_AOO_ROOT}/deps/oscpack/osc/OscPrintReceivedElements.cpp
    ${METRO_AOO_ROOT}/deps/oscpack/osc/OscReceivedElements.cpp
    ${METRO_AOO_ROOT}/deps/oscpack/osc/OscTypes.cpp
    ${METRO_AOO_ROOT}/deps/oscpack/osc/MessageMappingOscPacketListener.h
    ${METRO_AOO_ROOT}/deps/oscpack/osc/OscException.h
    ${METRO_AOO_ROOT}/deps/oscpack/osc/OscHostEndianness.h
    ${METRO_AOO_ROOT}/deps/oscpack/osc/OscOutboundPacketStream.h
    ${METRO_AOO_ROOT}/deps/oscpack/osc/OscPacketListener.h
    ${METRO_AOO_ROOT}/deps/oscpack/osc/OscPrintReceivedElements.h
    ${METRO_AOO_ROOT}/deps/oscpack/osc/OscReceivedElements.h
    ${METRO_AOO_ROOT}/deps/oscpack/osc/OscTypes.h
)

# The Opus CMake project installs its public headers as <include>/opus_*.h,
# while the SonoBus AOO fork includes them as <opus/opus_*.h>. Generate a tiny
# compatibility include tree instead of modifying the vendored AOO submodule.
set(METRO_OPUS_COMPAT_INCLUDE "${CMAKE_CURRENT_BINARY_DIR}/metro_opus_compat")
file(MAKE_DIRECTORY "${METRO_OPUS_COMPAT_INCLUDE}/opus")
file(GLOB METRO_OPUS_PUBLIC_HEADERS "${metro_opus_SOURCE_DIR}/include/*.h")
foreach(METRO_OPUS_HEADER IN LISTS METRO_OPUS_PUBLIC_HEADERS)
    get_filename_component(METRO_OPUS_HEADER_NAME "${METRO_OPUS_HEADER}" NAME)
    file(WRITE
        "${METRO_OPUS_COMPAT_INCLUDE}/opus/${METRO_OPUS_HEADER_NAME}"
        "#pragma once\n#include <${METRO_OPUS_HEADER_NAME}>\n")
endforeach()

add_library(MetroAoo STATIC ${METRO_AOO_SOURCES})
target_include_directories(MetroAoo PUBLIC
    ${METRO_AOO_ROOT}/lib
    ${METRO_AOO_ROOT}/deps
    ${METRO_OPUS_COMPAT_INCLUDE}
    ${metro_opus_SOURCE_DIR}/include
)
target_compile_definitions(MetroAoo PUBLIC
    USE_CODEC_OPUS=1
    AOO_TIMEFILTER_CHECK=0
    AOO_STATIC
)
target_compile_features(MetroAoo PRIVATE cxx_std_17)
target_link_libraries(MetroAoo PUBLIC
    Opus::opus
    $<$<PLATFORM_ID:Windows>:ws2_32>
)

set(DYSEKT_HAS_AOO TRUE)
message(STATUS "AOO/SonoBus network audio enabled (vendored AOO + Opus)")
