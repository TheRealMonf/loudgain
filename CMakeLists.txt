cmake_minimum_required(VERSION 3.14)

project(Loudgain LANGUAGES C CXX)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake/" ${CMAKE_MODULE_PATH})

set(VERSION_MAJOR 0)
set(VERSION_MINOR 7)
set(VERSION_PATCH 0)

set(loudgain_VERSION ${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH})

set(INSTALL_PREFIX ${CMAKE_INSTALL_PREFIX})

file(GLOB SOURCES RELATIVE ${CMAKE_SOURCE_DIR} "src/*.c" "src/*.cpp")

add_executable(Loudgain ${SOURCES})

configure_file("config.h.in" "config.h")

if (MSVC)
    add_definitions(-DTAGLIB_STATIC)

    include_directories(
      ${CMAKE_CURRENT_SOURCE_DIR}/include
      ${CMAKE_CURRENT_SOURCE_DIR}/include/argparse
      ${CMAKE_CURRENT_SOURCE_DIR}/src
      ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/ebur128/include
      ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/taglib/include
      ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/ffmpeg/include
      ${CMAKE_CURRENT_BINARY_DIR})

    target_link_libraries(Loudgain
        ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/taglib/lib/tag.lib
        ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/ebur128/lib/ebur128.lib
        ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/ffmpeg/lib/swresample.lib
        ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/ffmpeg/lib/avformat.lib
        ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/ffmpeg/lib/avcodec.lib
        ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/ffmpeg/lib/avutil.lib)

    set_target_properties(Loudgain PROPERTIES COMPILE_FLAGS "/EHsc /W4 /O2 /MD")
    set(CMAKE_C_FLAGS " /O2 /MD")
    set(CMAKE_CXX_FLAGS " /O2 /MD")

elseif (MINGW)
    add_definitions(-DTAGLIB_STATIC)

    include_directories(
      ${CMAKE_CURRENT_SOURCE_DIR}/include
      ${CMAKE_CURRENT_SOURCE_DIR}/include/argparse
      ${CMAKE_CURRENT_SOURCE_DIR}/src
      ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/ebur128/include
      ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/taglib/include
      ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/ffmpeg/include
      ${CMAKE_CURRENT_BINARY_DIR})

    target_link_libraries(Loudgain
        ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/taglib/lib/libtag.a
        z
        ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/ebur128/lib/libebur128.a
        ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/ffmpeg/lib/libswresample.a
        ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/ffmpeg/lib/libavformat.a
        ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/ffmpeg/lib/libavcodec.a
        ${CMAKE_CURRENT_SOURCE_DIR}/dependencies/ffmpeg/lib/libavutil.a
        bcrypt)

    set_target_properties(Loudgain PROPERTIES COMPILE_FLAGS "-Wall -O3 -static")
    set(CMAKE_C_FLAGS "-O3 -static")
    set(CMAKE_CXX_FLAGS "-O3 -static")

elseif (UNIX)
    find_package( PkgConfig )
    PKG_CHECK_MODULES(LAVC libavcodec REQUIRED)
    PKG_CHECK_MODULES(LAVF libavformat REQUIRED)
    PKG_CHECK_MODULES(LAVR libswresample REQUIRED)
    PKG_CHECK_MODULES(LAVU libavutil REQUIRED)
    PKG_CHECK_MODULES(LTAG taglib REQUIRED)
    PKG_CHECK_MODULES(LEBU libebur128 REQUIRED)

    include_directories(
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/include/argparse
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${LEBU_INCLUDE_DIRS}
        ${LAVC_INCLUDE_DIRS}
        ${LAVF_INCLUDE_DIRS}
        ${LAVR_INCLUDE_DIRS}
        ${LAVU_INCLUDE_DIRS}
        ${LTAG_INCLUDE_DIRS}
        ${CMAKE_CURRENT_BINARY_DIR})

    target_link_libraries(Loudgain
        ${LEBU_LIBRARIES}
        ${LAVC_LIBRARIES}
        ${LAVF_LIBRARIES}
        ${LAVR_LIBRARIES}
        ${LAVU_LIBRARIES}
        ${LTAG_LIBRARIES})

    set_target_properties(Loudgain PROPERTIES COMPILE_FLAGS "-Wall -O3")
    set(CMAKE_C_FLAGS "-O3")
    set(CMAKE_CXX_FLAGS "-O3")
endif()

find_package(OpenMP)
if (OPENMP_FOUND)
    set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${OpenMP_C_FLAGS}")
    set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${OpenMP_CXX_FLAGS}")
    set (CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${OpenMP_EXE_LINKER_FLAGS}")
endif()

install(TARGETS Loudgain DESTINATION ${CMAKE_INSTALL_PREFIX}/Loudgain)
