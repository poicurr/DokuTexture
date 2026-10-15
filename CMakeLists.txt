cmake_minimum_required(VERSION 3.18)
project(DokuTexture VERSION 1.0.0 LANGUAGES C CXX)

if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(TP_ENABLE_KTX2 "Enable KTX2 native read/write" ON)
option(TP_ENABLE_BASIS_TRANSCODER "Enable Basis Universal transcoding" ON)
set(TP_STANDALONE OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(TP_STANDALONE ON)
endif()
option(TP_BUILD_TESTS "Build tests" ${TP_STANDALONE})
option(TP_BUILD_BENCHMARKS "Build benchmarks" OFF)
option(TP_BUILD_EXAMPLES "Build minimal consumer example" ${TP_STANDALONE})

include(FetchContent)

# -----------------------------------------------------------------------------
# Dependency Management: FetchContent with local fallback (Pinned Commit SHAs)
# -----------------------------------------------------------------------------
set(LOCAL_BC7ENC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/bc7enc")
set(LOCAL_BASIS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/basis_universal")

if(EXISTS "${LOCAL_BC7ENC_DIR}/bc7enc.c")
    set(BC7ENC_DIR "${LOCAL_BC7ENC_DIR}")
else()
    message(STATUS "DokuTexture: Fetching bc7enc (commit f66c2e4) via FetchContent...")
    FetchContent_Declare(
        bc7enc_dep
        GIT_REPOSITORY https://github.com/richgel999/bc7enc.git
        GIT_TAG        f66c2e489b07138f2673a2fb3d27c1aa1d565c48
        GIT_SHALLOW    FALSE
        SOURCE_SUBDIR  _dokutexture_sources_only
    )
    FetchContent_MakeAvailable(bc7enc_dep)
    set(BC7ENC_DIR "${bc7enc_dep_SOURCE_DIR}")
endif()

if(TP_ENABLE_BASIS_TRANSCODER)
if(EXISTS "${LOCAL_BASIS_DIR}/transcoder/basisu_transcoder.cpp")
    set(BASIS_DIR "${LOCAL_BASIS_DIR}")
else()
    message(STATUS "DokuTexture: Fetching basis_universal (commit 99f52d6) via FetchContent...")
    FetchContent_Declare(
        basis_dep
        GIT_REPOSITORY https://github.com/BinomialLLC/basis_universal.git
        GIT_TAG        99f52d63aa6799cbdaecfe977111dc5ec3b31d47
        GIT_SHALLOW    FALSE
        SOURCE_SUBDIR  _dokutexture_sources_only
    )
    FetchContent_MakeAvailable(basis_dep)
    set(BASIS_DIR "${basis_dep_SOURCE_DIR}")
endif()
endif()

message(STATUS "DokuTexture: Using BC7ENC_DIR = ${BC7ENC_DIR}")
message(STATUS "DokuTexture: Using BASIS_DIR = ${BASIS_DIR}")

set(BC7ENC_SOURCES
    ${BC7ENC_DIR}/bc7enc.c
)

if(TP_ENABLE_BASIS_TRANSCODER)
set(BASIS_SOURCES
    ${BASIS_DIR}/transcoder/basisu_transcoder.cpp
    ${BASIS_DIR}/zstd/zstd.c
)
endif()

set(DOKUTEXTURE_SOURCES
    src/bc7_encoder.cpp
    src/disabled.cpp
)

if(TP_ENABLE_KTX2 OR TP_ENABLE_BASIS_TRANSCODER)
    list(APPEND DOKUTEXTURE_SOURCES src/ktx2_io.cpp)
endif()

if(TP_ENABLE_BASIS_TRANSCODER)
    list(APPEND DOKUTEXTURE_SOURCES src/basis_transcoder.cpp)
endif()

add_library(DokuTexture_static STATIC
    ${DOKUTEXTURE_SOURCES}
    ${BC7ENC_SOURCES}
    ${BASIS_SOURCES}
)

add_library(DokuTexture::DokuTexture ALIAS DokuTexture_static)
target_compile_features(DokuTexture_static PUBLIC cxx_std_17)

target_include_directories(DokuTexture_static PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_include_directories(DokuTexture_static PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${BC7ENC_DIR}
)
if(TP_ENABLE_BASIS_TRANSCODER)
    target_include_directories(DokuTexture_static PRIVATE ${BASIS_DIR}/transcoder ${BASIS_DIR}/zstd)
    # Required by the pinned Basis transcoder's aliasing contract.
    target_compile_options(DokuTexture_static PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-fno-strict-aliasing>)
endif()

target_compile_definitions(DokuTexture_static PRIVATE
    TP_HAS_KTX2=$<OR:$<BOOL:${TP_ENABLE_KTX2}>,$<BOOL:${TP_ENABLE_BASIS_TRANSCODER}>>
    TP_HAS_BASIS=$<BOOL:${TP_ENABLE_BASIS_TRANSCODER}>
    BASISD_SUPPORT_KTX2=1
    BASISD_SUPPORT_KTX2_ZSTD=1
)

find_package(Threads REQUIRED)
target_link_libraries(DokuTexture_static PRIVATE Threads::Threads)

if(TP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(TP_BUILD_TESTS OR TP_BUILD_BENCHMARKS)
    add_library(DokuTexture_test_decoder STATIC ${BC7ENC_DIR}/bc7decomp.cpp)
    target_include_directories(DokuTexture_test_decoder PUBLIC ${BC7ENC_DIR})
endif()

if(TP_BUILD_BENCHMARKS)
    add_subdirectory(bench)
endif()

if(TP_BUILD_EXAMPLES AND (TP_ENABLE_KTX2 OR TP_ENABLE_BASIS_TRANSCODER))
    add_subdirectory(examples)
endif()
