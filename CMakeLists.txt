cmake_minimum_required(VERSION 3.28)

project(rest_exchange_gateway VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug)
endif()

option(GATEWAY_SANITIZE "Enable AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(GATEWAY_BUILD_TESTS "Build the test suite" ON)

find_package(Boost REQUIRED COMPONENTS system)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)

add_library(gateway_warnings INTERFACE)
target_compile_options(gateway_warnings INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Werror
)

add_library(gateway_third_party INTERFACE)
target_include_directories(gateway_third_party SYSTEM INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_compile_definitions(gateway_third_party INTERFACE
    CROW_USE_BOOST
    CROW_ENABLE_SSL
    CPPHTTPLIB_OPENSSL_SUPPORT
)
target_link_libraries(gateway_third_party INTERFACE
    Boost::system
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
    ZLIB::ZLIB
)

if(GATEWAY_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

add_executable(smoke apps/smoke.cpp)
target_link_libraries(smoke PRIVATE gateway_warnings gateway_third_party)

add_library(gateway_core STATIC
    src/core/clock.cpp
    src/core/config.cpp
)
target_include_directories(gateway_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(gateway_core PUBLIC gateway_warnings gateway_third_party)

add_library(gateway_okx STATIC
    src/exchange/okx/okx_signer.cpp
    src/exchange/okx/okx_wire.cpp
    src/exchange/okx/okx_rest_client.cpp
    src/exchange/okx/okx_connector.cpp
)
target_link_libraries(gateway_okx PUBLIC gateway_core)

if(GATEWAY_BUILD_TESTS)
    enable_testing()

    add_library(gateway_test_support STATIC
        tests/mocks/okx_mock_server.cpp
    )
    target_include_directories(gateway_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_link_libraries(gateway_test_support PUBLIC gateway_okx)

    add_executable(smoke_test tests/smoke_test.cpp)
    target_include_directories(smoke_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_link_libraries(smoke_test PRIVATE gateway_warnings gateway_third_party)
    add_test(NAME smoke_test COMMAND smoke_test)

    add_executable(result_test tests/result_test.cpp)
    target_link_libraries(result_test PRIVATE gateway_core)
    add_test(NAME result_test COMMAND result_test)

    add_executable(clock_test tests/clock_test.cpp)
    target_link_libraries(clock_test PRIVATE gateway_core)
    add_test(NAME clock_test COMMAND clock_test)

    add_executable(config_test tests/config_test.cpp)
    target_link_libraries(config_test PRIVATE gateway_okx)
    add_test(NAME config_test COMMAND config_test)

    add_executable(okx_signer_test tests/okx_signer_test.cpp)
    target_link_libraries(okx_signer_test PRIVATE gateway_okx)
    add_test(NAME okx_signer_test COMMAND okx_signer_test)

    add_executable(okx_wire_test tests/okx_wire_test.cpp)
    target_link_libraries(okx_wire_test PRIVATE gateway_okx)
    add_test(NAME okx_wire_test COMMAND okx_wire_test)

    add_executable(okx_rest_client_test tests/okx_rest_client_test.cpp)
    target_link_libraries(okx_rest_client_test PRIVATE gateway_test_support)
    add_test(NAME okx_rest_client_test COMMAND okx_rest_client_test)

    add_executable(okx_connector_test tests/okx_connector_test.cpp)
    target_link_libraries(okx_connector_test PRIVATE gateway_test_support)
    add_test(NAME okx_connector_test COMMAND okx_connector_test)
endif()
