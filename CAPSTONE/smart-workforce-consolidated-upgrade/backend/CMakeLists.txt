
cmake_minimum_required(VERSION 3.21)
project(smart_workforce_server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Drogon CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(unofficial-sodium CONFIG REQUIRED)

add_executable(workforce_server main.cpp)

target_link_libraries(workforce_server PRIVATE
    Drogon::Drogon
    unofficial::sqlite3::sqlite3
    unofficial-sodium::sodium
)
