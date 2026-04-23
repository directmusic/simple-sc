cmake_minimum_required(VERSION 3.10)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE STRING "")

project(simple-sc LANGUAGES C CXX VERSION 0.1.1)
add_executable(simple-sc main.cc screencast_portal.cc shm.cc)

find_package(PkgConfig)

pkg_check_modules(PIPEWIRE libpipewire-0.3 REQUIRED)
pkg_check_modules(DBUS dbus-1 REQUIRED)
pkg_check_modules(PORTAL libportal REQUIRED)
pkg_check_modules(AVCODEC libavcodec REQUIRED)
pkg_check_modules(AVFORMAT libavformat REQUIRED)
pkg_check_modules(AVUTIL libavutil REQUIRED)
pkg_check_modules(SWSCALE libswscale REQUIRED)
pkg_check_modules(SWRESAMPLE libswresample REQUIRED)

# Link libraries
target_include_directories(simple-sc PUBLIC
    ${PIPEWIRE_INCLUDE_DIRS}
    ${DBUS_INCLUDE_DIRS}
    ${PORTAL_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${SWSCALE_INCLUDE_DIRS}
    ${SWRESAMPLE_INCLUDE_DIRS}
)

target_link_libraries(simple-sc
    ${PIPEWIRE_LIBRARIES}
    ${DBUS_LIBRARIES}
    ${PORTAL_LIBRARIES}
    ${AVCODEC_LIBRARIES}
    ${AVFORMAT_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    ${SWSCALE_LIBRARIES}
    ${SWRESAMPLE_LIBRARIES}
    m
    z
)

install(TARGETS simple-sc)
