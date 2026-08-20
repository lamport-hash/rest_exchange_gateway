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

if(GATEWAY_BUILD_TESTS)
    enable_testing()

    add_executable(smoke_test tests/smoke_test.cpp)
    target_include_directories(smoke_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_link_libraries(smoke_test PRIVATE gateway_warnings gateway_third_party)
    add_test(NAME smoke_test COMMAND smoke_test)
endif()
