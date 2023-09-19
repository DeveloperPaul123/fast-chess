cmake_minimum_required(VERSION 3.15 FATAL_ERROR)

project(fast-chess)

option(FAST_CHESS_BUILD_TESTS "Build tests" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
file(GLOB_RECURSE FAST_CHESS_SOURCES CONFIG_DEPENDS "src/*.cpp")
file(GLOB_RECURSE FAST_CHESS_HEADERS CONFIG_DEPENDS "src/*.hpp")

add_executable(${PROJECT_NAME} ${FAST_CHESS_SOURCES} ${FAST_CHESS_HEADERS})
target_include_directories(${PROJECT_NAME} PRIVATE src)
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_17)

target_compile_options(${PROJECT_NAME} PRIVATE 
    $<$<CXX_COMPILER_ID:Clang>:-march=native>
    $<$<CXX_COMPILER_ID:MSVC>:/clr>
)

if(FAST_CHESS_BUILD_TESTS)
    enable_testing()
    file(GLOB_RECURSE FAST_CHESS_TEST_SOURCES CONFIG_DEPENDS "test/*.cpp")
endif()
