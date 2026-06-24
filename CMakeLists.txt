cmake_minimum_required(VERSION 3.10)
project(pgnparser VERSION 1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_testing()

option(PGNPARSER_BUILD_EXAMPLES "Build examples" ON)
option(PGNPARSER_BUILD_EXPORT "Build pgnexport (requires pulling chesslib)" OFF)
option(PGNPARSER_BUILD_TESTS "Build tests" ON)

add_library(pgnparser STATIC pgnparser.cpp pgnparser.hpp)

if (PGNPARSER_BUILD_EXPORT)
    include(FetchContent)

    FetchContent_Declare(
        chesslib
        GIT_REPOSITORY https://github.com/winapiadmin/chesslib.git
        GIT_TAG main
    )
    if(DEFINED BUILD_TESTING)
        set(_OLD_BUILD_TESTING "${BUILD_TESTING}")
        set(_HAD_BUILD_TESTING TRUE)
    endif()
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(chesslib)
    if(_HAD_BUILD_TESTING)
        set(BUILD_TESTING "${_OLD_BUILD_TESTING}" CACHE BOOL "" FORCE)
    else()
        unset(BUILD_TESTING CACHE)
    endif()

    add_library(pgnexport STATIC pgnexport.cpp pgnexport.hpp)
    target_link_libraries(pgnexport PUBLIC pgnparser chesslib)
    target_include_directories(pgnexport PUBLIC ${chesslib_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR})

    if (PGNPARSER_BUILD_EXAMPLES)
        add_executable(pgnexport_example1 export_example1.cpp)
        target_link_libraries(pgnexport_example1 PRIVATE pgnexport)
    endif()
endif()

if (PGNPARSER_BUILD_EXAMPLES)
    add_executable(pgnparser_example1 import_example1.cpp)
    target_link_libraries(pgnparser_example1 PRIVATE pgnparser)
    add_executable(pgnparser_count_games import_example2.cpp)
    target_link_libraries(pgnparser_count_games PRIVATE pgnparser)
endif()

if (PGNPARSER_BUILD_TESTS AND PGNPARSER_BUILD_EXPORT)
    add_executable(pgn_export_test tests/test_pgn_export.cpp)
    target_link_libraries(pgn_export_test PRIVATE pgnexport)
    target_include_directories(pgn_export_test PRIVATE ${chesslib_SOURCE_DIR})
    target_compile_definitions(pgn_export_test PRIVATE TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/")
    set_target_properties(pgn_export_test PROPERTIES FOLDER tests)
    add_test(NAME pgn_export_test COMMAND pgn_export_test
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
endif()

if (PGNPARSER_BUILD_TESTS)
    add_executable(pgn_count_test tests/test_count_games.cpp)
    target_link_libraries(pgn_count_test PRIVATE pgnparser)
    target_include_directories(pgn_count_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(pgn_count_test PRIVATE TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/")
    set_target_properties(pgn_count_test PROPERTIES FOLDER tests)
    add_test(NAME pgn_count_test COMMAND pgn_count_test
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
endif()
