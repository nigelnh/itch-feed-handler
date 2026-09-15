cmake_minimum_required(VERSION 3.20)

project(itch_feed_handler VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

function(itch_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(
            ${target}
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
                -Wconversion
                -Wsign-conversion
        )
    endif()
endfunction()

add_library(itch_core
    src/decoder.cpp
    src/framing.cpp
    src/mapped_file.cpp
    src/order_book.cpp
    src/replay.cpp
)
target_include_directories(itch_core PUBLIC include)
itch_enable_warnings(itch_core)

add_executable(toy_order_book
    src/toy_order_book.cpp
)
target_link_libraries(toy_order_book PRIVATE itch_core)
itch_enable_warnings(toy_order_book)

add_executable(itch_replay
    src/itch_replay.cpp
)
target_link_libraries(itch_replay PRIVATE itch_core)
itch_enable_warnings(itch_replay)

add_executable(itch_benchmark
    src/itch_benchmark.cpp
)
target_link_libraries(itch_benchmark PRIVATE itch_core)
target_compile_definitions(
    itch_benchmark
    PRIVATE ITCH_BUILD_TYPE="$<IF:$<CONFIG:Release>,Release,Non-Release>"
)
itch_enable_warnings(itch_benchmark)

add_executable(itch_generate_synthetic
    src/itch_generate_synthetic.cpp
)
target_link_libraries(itch_generate_synthetic PRIVATE itch_core)
itch_enable_warnings(itch_generate_synthetic)

include(CTest)
if(BUILD_TESTING)
    add_executable(order_book_tests
        tests/order_book_tests.cpp
    )
    target_link_libraries(order_book_tests PRIVATE itch_core)
    itch_enable_warnings(order_book_tests)

    add_test(NAME order_book_tests COMMAND order_book_tests)

    add_executable(decoder_replay_tests
        tests/decoder_replay_tests.cpp
    )
    target_link_libraries(decoder_replay_tests PRIVATE itch_core)
    itch_enable_warnings(decoder_replay_tests)

    add_test(NAME decoder_replay_tests COMMAND decoder_replay_tests)
endif()
