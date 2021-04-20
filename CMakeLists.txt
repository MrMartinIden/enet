cmake_minimum_required(VERSION 2.6)
project(enet CXX)

if(MSVC)
        add_definitions(-W4)
else()
        add_definitions(-Wall)
endif()

set(SRC test/library.cpp)

option(ENET_STATIC "" ON)
if (ENET_STATIC)
    add_library(enet_static STATIC ${SRC})
    target_include_directories(enet_static PUBLIC "./include")

    if (WIN32)
        target_link_libraries(enet_static winmm ws2_32)
    endif()
endif()

option(ENET_SHARED "" ON)
if (ENET_SHARED)
    add_library(enet SHARED ${SRC})
    target_compile_definitions(enet PRIVATE -DENET_DLL)
    target_include_directories(enet PUBLIC "./include")

    if (WIN32)
        target_link_libraries(enet winmm ws2_32)
    endif()
endif()

option(ENET_TEST "" ON)
if (ENET_TEST)
    add_executable(enet_test test/build.cpp)
    target_include_directories(enet_test PUBLIC "./include")
    target_link_libraries(enet_test enet_static)

    add_executable(enet_server test/server.cpp)
    target_include_directories(enet_server PUBLIC "./include")
    target_link_libraries(enet_server enet_static)

endif()
