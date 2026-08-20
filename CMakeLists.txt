cmake_minimum_required(VERSION 3.16)

project(ctp_client VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT DEFINED CTP_SDK_ROOT)
    message(FATAL_ERROR
        "CTP_SDK_ROOT is required. Example: "
        "cmake -S . -B build -DCTP_SDK_ROOT=/path/to/ctp/sdk/linux64")
endif()

set(CTP_MD_DIR "${CTP_SDK_ROOT}/md")
set(CTP_TRADER_DIR "${CTP_SDK_ROOT}/trader")
set(CTP_MD_LIBRARY "${CTP_MD_DIR}/thostmduserapi_se.so")
set(CTP_TRADER_LIBRARY "${CTP_TRADER_DIR}/thosttraderapi_se.so")

foreach(required_file
        "${CTP_MD_DIR}/ThostFtdcMdApi.h"
        "${CTP_TRADER_DIR}/ThostFtdcTraderApi.h"
        "${CTP_MD_LIBRARY}"
        "${CTP_TRADER_LIBRARY}")
    if(NOT EXISTS "${required_file}")
        message(FATAL_ERROR "Required CTP SDK file not found: ${required_file}")
    endif()
endforeach()

add_library(ctp_config src/config.cpp)
target_include_directories(ctp_config PUBLIC include)
target_include_directories(ctp_config SYSTEM PRIVATE "${CTP_TRADER_DIR}")
target_compile_options(ctp_config PRIVATE
    -Wall
    -Wextra
    -Wpedantic)

add_executable(ctp_client src/main.cpp)

target_include_directories(ctp_client SYSTEM PRIVATE
    "${CTP_MD_DIR}"
    "${CTP_TRADER_DIR}")

target_link_directories(ctp_client PRIVATE
    "${CTP_MD_DIR}"
    "${CTP_TRADER_DIR}")

target_link_libraries(ctp_client PRIVATE
    ctp_config
    "-l:thostmduserapi_se.so"
    "-l:thosttraderapi_se.so")

target_compile_options(ctp_client PRIVATE
    -Wall
    -Wextra
    -Wpedantic)

set_target_properties(ctp_client PROPERTIES
    BUILD_RPATH "${CTP_MD_DIR};${CTP_TRADER_DIR}")

include(CTest)

if(BUILD_TESTING)
    add_executable(ctp_config_tests tests/config_test.cpp)
    target_link_libraries(ctp_config_tests PRIVATE ctp_config)
    target_compile_options(ctp_config_tests PRIVATE
        -Wall
        -Wextra
        -Wpedantic)
    add_test(NAME config COMMAND ctp_config_tests)
endif()
