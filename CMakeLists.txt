cmake_minimum_required(VERSION 3.10)
project(optimfrogcpp CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -ffast-math")

include_directories(include)

add_library(optimfrogcpp_lib
    src/optimfrog.cpp
    src/optimfrog_decoder.cpp
    src/optimfrog_decoder_entropy.cpp
    src/optimfrog_decoder_predictor.cpp
    src/optimfrog_decoder_pred3.cpp
    src/optimfrog_decoder_ent3.cpp
)

# pred3 cascade uses float32 math that must NOT be reordered/contracted by -ffast-math
set_source_files_properties(src/optimfrog_decoder_pred3.cpp PROPERTIES
    COMPILE_OPTIONS "-fno-fast-math;-ffp-contract=off")

add_executable(optimfrogcpp src/main.cpp)
target_link_libraries(optimfrogcpp optimfrogcpp_lib)
