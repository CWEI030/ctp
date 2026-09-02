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
        "${CTP_MD_DIR}/ThostFtdcUserApiDataType.h"
        "${CTP_MD_DIR}/ThostFtdcUserApiStruct.h"
        "${CTP_TRADER_DIR}/ThostFtdcTraderApi.h"
        "${CTP_TRADER_DIR}/ThostFtdcUserApiDataType.h"
        "${CTP_TRADER_DIR}/ThostFtdcUserApiStruct.h"
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

add_library(ctp_interrupt src/interrupt.cpp)
target_include_directories(ctp_interrupt PUBLIC include)
target_compile_options(ctp_interrupt PRIVATE
    -Wall
    -Wextra
    -Wpedantic)

add_library(ctp_market src/market_client.cpp)
target_include_directories(ctp_market PUBLIC include)
target_include_directories(ctp_market SYSTEM PUBLIC "${CTP_MD_DIR}")
target_link_directories(ctp_market PUBLIC "${CTP_MD_DIR}")
target_link_libraries(ctp_market
    PUBLIC ctp_config
    PRIVATE "-l:thostmduserapi_se.so")
target_compile_options(ctp_market PRIVATE
    -Wall
    -Wextra
    -Wpedantic)

add_library(ctp_engine src/engine.cpp src/trading.cpp)
target_include_directories(ctp_engine PUBLIC include)
target_include_directories(ctp_engine SYSTEM PUBLIC "${CTP_MD_DIR}")
target_link_libraries(ctp_engine PUBLIC ctp_config ctp_market)
target_compile_options(ctp_engine PRIVATE
    -Wall
    -Wextra
    -Wpedantic)

add_library(ctp_trader src/trader_client.cpp)
target_include_directories(ctp_trader PUBLIC include)
target_include_directories(ctp_trader SYSTEM PUBLIC "${CTP_TRADER_DIR}")
target_link_directories(ctp_trader PUBLIC "${CTP_TRADER_DIR}")
target_link_libraries(ctp_trader
    PUBLIC ctp_config
    PRIVATE "-l:thosttraderapi_se.so")
target_compile_options(ctp_trader PRIVATE
    -Wall
    -Wextra
    -Wpedantic)

add_executable(ctp_client src/main.cpp)

target_include_directories(ctp_client SYSTEM PRIVATE
    "${CTP_MD_DIR}")

target_link_directories(ctp_client PRIVATE
    "${CTP_MD_DIR}")

target_link_libraries(ctp_client PRIVATE
    ctp_interrupt
    ctp_engine
    ctp_market
    ctp_trader)

target_compile_options(ctp_client PRIVATE
    -Wall
    -Wextra
    -Wpedantic)

set_target_properties(ctp_client PROPERTIES
    BUILD_RPATH "${CTP_MD_DIR};${CTP_TRADER_DIR}")

include(CTest)

if(BUILD_TESTING)
    add_executable(ctp_interrupt_tests tests/interrupt_test.cpp)
    target_link_libraries(ctp_interrupt_tests PRIVATE ctp_interrupt)
    target_compile_options(ctp_interrupt_tests PRIVATE
        -Wall
        -Wextra
        -Wpedantic)
    add_test(NAME interrupt COMMAND ctp_interrupt_tests)

    add_executable(ctp_config_tests tests/config_test.cpp)
    target_link_libraries(ctp_config_tests PRIVATE ctp_config)
    target_compile_options(ctp_config_tests PRIVATE
        -Wall
        -Wextra
        -Wpedantic)
    add_test(NAME config COMMAND ctp_config_tests)

    add_executable(ctp_market_tests tests/market_client_test.cpp)
    target_link_libraries(ctp_market_tests PRIVATE ctp_market)
    target_compile_options(ctp_market_tests PRIVATE
        -Wall
        -Wextra
        -Wpedantic)
    add_test(NAME market COMMAND ctp_market_tests)

    add_executable(ctp_engine_tests tests/engine_test.cpp)
    target_include_directories(ctp_engine_tests PRIVATE tests)
    target_link_libraries(ctp_engine_tests PRIVATE ctp_engine)
    target_compile_options(ctp_engine_tests PRIVATE
        -Wall
        -Wextra
        -Wpedantic)
    add_test(NAME engine COMMAND ctp_engine_tests)

    add_executable(ctp_trading_tests tests/trading_test.cpp)
    target_include_directories(ctp_trading_tests PRIVATE tests)
    target_link_libraries(ctp_trading_tests PRIVATE ctp_engine)
    target_compile_options(ctp_trading_tests PRIVATE
        -Wall
        -Wextra
        -Wpedantic)
    add_test(NAME trading COMMAND ctp_trading_tests)

    add_executable(ctp_trader_tests tests/trader_client_test.cpp)
    target_link_libraries(ctp_trader_tests PRIVATE ctp_trader)
    target_compile_options(ctp_trader_tests PRIVATE
        -Wall
        -Wextra
        -Wpedantic)
    add_test(NAME trader COMMAND ctp_trader_tests)
endif()
