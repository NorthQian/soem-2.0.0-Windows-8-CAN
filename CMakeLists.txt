cmake_minimum_required(VERSION 3.20)
project(soem_demo VERSION 1.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# This distribution ships SOEM as a prebuilt static library plus headers,
# not as a source tree, so link it as an imported target.
add_library(soem STATIC IMPORTED GLOBAL)
set_target_properties(soem PROPERTIES
    IMPORTED_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/lib/libsoem.a"
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# libsoem.a calls into Npcap. The SDK's .lib files are MSVC-format and
# unusable by MinGW, so import libraries were generated from the installed
# DLLs with gendef + dlltool (see import-libs/). Only the SDK headers are
# used, for nicdrv.h's "#include <pcap.h>".
target_include_directories(soem INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/npcap-sdk/Include"
)
add_library(wpcap STATIC IMPORTED GLOBAL)
set_target_properties(wpcap PROPERTIES
    IMPORTED_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/import-libs/libwpcap.a"
)

add_library(Packet STATIC IMPORTED GLOBAL)
set_target_properties(Packet PROPERTIES
    IMPORTED_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/import-libs/libPacket.a"
)

add_executable(soem_demo main.cpp)

# libsoem.a was compiled against the UCRT, so this must match or the
# C runtime ABI will not line up.
target_compile_definitions(soem_demo PRIVATE _UCRT)

target_link_libraries(soem_demo PRIVATE
    soem
    wpcap
    Packet
    ucrt
    ws2_32
    winmm
)
