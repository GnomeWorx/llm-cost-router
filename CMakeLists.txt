cmake_minimum_required(VERSION 3.20)
project(llm-cost-router VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Dependencies ──
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)

# ── Sources ──
set(SOURCES
    src/main.cpp
    src/server.cpp
    src/router.cpp
    src/ollama.cpp
    src/cloud.cpp
    src/cost.cpp
    src/config.cpp
)

# ── Executable ──
add_executable(llm-cost-router ${SOURCES})

target_compile_definitions(llm-cost-router PRIVATE
    CPPHTTPLIB_OPENSSL_SUPPORT
)

target_include_directories(llm-cost-router PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party
)

target_link_libraries(llm-cost-router PRIVATE
    CURL::libcurl
    OpenSSL::SSL
    OpenSSL::Crypto
    pthread
)

# ── Install ──
install(TARGETS llm-cost-router DESTINATION bin)
install(FILES config.json DESTINATION share/llm-cost-router/)
