cmake_minimum_required(VERSION 3.13)
project(csk_flow C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Path to a checkout of https://github.com/LISTENAI/cskburn (Apache-2.0).
# We link its serial burn library rather than wrapping the CLI binary.
#   cmake -B build -DCSKBURN_DIR=/path/to/cskburn
if(NOT DEFINED CSKBURN_DIR)
    message(FATAL_ERROR
        "Set -DCSKBURN_DIR=/path/to/cskburn (a checkout of LISTENAI/cskburn)")
endif()
get_filename_component(CSKBURN_DIR "${CSKBURN_DIR}" ABSOLUTE)

# libcskburn_serial embeds its burner blobs via ${CMAKE_SOURCE_DIR}/bin2c.py.
# When we are the top-level project, CMAKE_SOURCE_DIR is this dir, not the
# cskburn checkout, so make the helper available here (gitignored).
file(COPY ${CSKBURN_DIR}/bin2c.py DESTINATION ${CMAKE_SOURCE_DIR})

# The serial burn path pulls in only these components (no libusb, no mbedtls:
# the flash MD5 is computed on-device and compared against a caller hash).
foreach(comp liberrors libio liblog libportable libserial libslip
             libcskburn_serial)
    add_subdirectory(${CSKBURN_DIR}/${comp} ${CMAKE_BINARY_DIR}/${comp})
endforeach()

# Shared flash/console core (CLI + daemon).
add_library(csk-ops STATIC src/ops.c src/md5.c)
# cskburn_serial links serial/io/etc. transitively; listing them again warns.
target_link_libraries(csk-ops cskburn_serial)
target_include_directories(csk-ops PUBLIC
    src
    ${CSKBURN_DIR}/libcskburn_serial/include
    ${CSKBURN_DIR}/libserial/include
    ${CSKBURN_DIR}/libio/include
    ${CSKBURN_DIR}/liblog/include)

add_executable(csk-flow src/main.c)
target_link_libraries(csk-flow csk-ops)

add_executable(csk-flowd src/daemon.c)
target_link_libraries(csk-flowd csk-ops)
