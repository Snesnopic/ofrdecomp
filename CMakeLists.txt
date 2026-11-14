cmake_minimum_required(VERSION 3.10)
project(ofrdecomp CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

# the output has to match the reference bit for bit, so float math must stay exactly as written
# everywhere: no reassociation, no fused multiply-add, no x87 excess precision on 32-bit x86.
if(MSVC)
    add_compile_options(/fp:precise)
else()
    add_compile_options(-fno-fast-math -ffp-contract=off -fno-math-errno)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "i[3-6]86|x86|AMD64|x86_64" AND CMAKE_SIZEOF_VOID_P EQUAL 4
       OR CMAKE_CXX_FLAGS MATCHES "-m32")
        add_compile_options(-msse2 -mfpmath=sse)
    endif()
endif()

find_package(Threads REQUIRED)

include_directories(include)

add_library(ofrdecomp_lib
    src/core/optimfrog.cpp
    src/core/optimfrog_decoder.cpp
    src/core/optimfrog_decoder_entropy.cpp
    src/core/optimfrog_decoder_predictor.cpp
    src/core/optimfrog_decoder_pred3.cpp
    src/core/optimfrog_decoder_ent3.cpp
    src/core/optimfrog_decoder_post2.cpp
    src/core/optimfrog_decoder_float.cpp
    src/core/optimfrog_decoder_dualstream.cpp
    src/core/optimfrog_tags.cpp
    src/core/optimfrog_encoder.cpp
    src/core/optimfrog_encoder_float.cpp
    src/core/ofrdecomp.cpp
)

target_link_libraries(ofrdecomp_lib PUBLIC Threads::Threads)

add_executable(ofrdecomp src/cli/main.cpp)
target_link_libraries(ofrdecomp ofrdecomp_lib)

# encoder CLI (positional args: in.raw out.ofr rate [channels] [bps])
add_executable(ofrenc src/cli/ofrenc_main.cpp)
target_link_libraries(ofrenc ofrdecomp_lib)

# unified CLI aiming for usage/flag parity with the original `ofr` tool
add_executable(ofr src/cli/ofr_main.cpp)
target_link_libraries(ofr ofrdecomp_lib)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(OFRDECOMP_TOP_LEVEL ON)
else()
    set(OFRDECOMP_TOP_LEVEL OFF)
endif()
option(OFRDECOMP_TESTS "build the unit tests" ${OFRDECOMP_TOP_LEVEL})
if(OFRDECOMP_TESTS)
    enable_testing()
    add_executable(ofr_tests tests/unit/ofr_tests.cpp)
    target_link_libraries(ofr_tests ofrdecomp_lib)
    add_test(NAME unit COMMAND ofr_tests)
    if(NOT WIN32)
        add_test(NAME cli COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_roundtrip.sh $<TARGET_FILE:ofr>)
    endif()
endif()

# clang only: cmake -DOFRDECOMP_FUZZ=ON -DCMAKE_CXX_COMPILER=clang++
option(OFRDECOMP_FUZZ "build the libFuzzer harness" OFF)
if(OFRDECOMP_FUZZ)
    # the library has to be instrumented too, or the fuzzer gets no coverage feedback from it
    target_compile_options(ofrdecomp_lib PRIVATE -fsanitize=fuzzer-no-link)
    add_executable(fuzz_decoder tests/fuzz/fuzz_decoder.cpp)
    target_compile_options(fuzz_decoder PRIVATE -fsanitize=fuzzer)
    target_link_options(fuzz_decoder PRIVATE -fsanitize=fuzzer)
    target_link_libraries(fuzz_decoder ofrdecomp_lib)
    add_executable(make_corpus tests/fuzz/make_corpus.cpp)
    target_link_libraries(make_corpus ofrdecomp_lib)
endif()
