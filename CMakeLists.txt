cmake_minimum_required(VERSION 3.0)

project(raven)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt5 REQUIRED COMPONENTS Core Widgets Network)

add_library(${PROJECT_NAME} raven_client.cpp)

target_link_libraries(${PROJECT_NAME} Qt5::Core Qt5::Network Qt5::Widgets)
