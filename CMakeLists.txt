cmake_minimum_required(VERSION 2.8.8)

project(entwine)

add_definitions(${CMAKE_CXX_FLAGS} "-std=c++11")
add_definitions(${CMAKE_CXX_FLAGS} "-g")
add_definitions(${CMAKE_CXX_FLAGS} "-O3")
add_definitions(${CMAKE_CXX_FLAGS} "-Wno-deprecated-declarations")
add_definitions(${CMAKE_CXX_FLAGS} "-Wall")
add_definitions(${CMAKE_CXX_FLAGS} "-Werror")
add_definitions(${CMAKE_CXX_FLAGS} "-pedantic")
add_definitions(${CMAKE_CXX_FLAGS} "-fexceptions")
add_definitions(${CMAKE_CXX_FLAGS} "-fPIC")

include_directories("${CMAKE_CURRENT_SOURCE_DIR}")

add_subdirectory(entwine)
add_subdirectory(kernel)

set(OBJS
    $<TARGET_OBJECTS:compression>
    $<TARGET_OBJECTS:http>
    $<TARGET_OBJECTS:third>
    $<TARGET_OBJECTS:tree>
    $<TARGET_OBJECTS:types>
)

add_library(entwine SHARED ${OBJS})
target_link_libraries(entwine pdalcpp curl crypto)
install(TARGETS entwine DESTINATION lib)
