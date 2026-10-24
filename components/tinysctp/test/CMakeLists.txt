# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
# tinysctp's host tests, on their own or from host-tests:
#   cmake -S components/tinysctp/test -B build/tinysctp && cmake --build build/tinysctp && ctest --test-dir build/tinysctp
cmake_minimum_required(VERSION 3.16)
project(tinysctp_tests C)

if(NOT TARGET tinysctp)
    add_subdirectory(.. tinysctp)
endif()

add_executable(tinysctp_tests test_sctp.c)
target_link_libraries(tinysctp_tests PRIVATE tinysctp)
set_target_properties(tinysctp_tests PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_compile_options(tinysctp_tests PRIVATE -Wall -Wno-unused-parameter)

# The UDP glue over real loopback sockets.
add_executable(tinysctp_udp_tests test_udp.c)
target_link_libraries(tinysctp_udp_tests PRIVATE tinysctp)
set_target_properties(tinysctp_udp_tests PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_compile_options(tinysctp_udp_tests PRIVATE -Wall -Wno-unused-parameter)

# Edge cases: hand-built packets, collisions, refused data, out of memory.
add_executable(tinysctp_edge_tests test_edges.c)
target_link_libraries(tinysctp_edge_tests PRIVATE tinysctp)
set_target_properties(tinysctp_edge_tests PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_compile_options(tinysctp_edge_tests PRIVATE -Wall -Wno-unused-parameter)

# A peer over a real UDP socket, for the interop test against usrsctp (interop.sh).
add_executable(tinysctp_peer peer.c)
target_link_libraries(tinysctp_peer PRIVATE tinysctp)
set_target_properties(tinysctp_peer PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_compile_options(tinysctp_peer PRIVATE -Wall)

option(TINYSCTP_SANITIZE "Build with address and undefined behavior sanitizers" ON)
if(TINYSCTP_SANITIZE AND NOT CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    foreach(t tinysctp tinysctp_tests tinysctp_udp_tests tinysctp_edge_tests tinysctp_peer)
        target_compile_options(${t} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    endforeach()
    target_link_options(tinysctp_tests PRIVATE -fsanitize=address,undefined)
    target_link_options(tinysctp_edge_tests PRIVATE -fsanitize=address,undefined)
    target_link_options(tinysctp_udp_tests PRIVATE -fsanitize=address,undefined)
    target_link_options(tinysctp_peer PRIVATE -fsanitize=address,undefined)
endif()

enable_testing()
add_test(NAME tinysctp_tests COMMAND tinysctp_tests)
add_test(NAME tinysctp_udp_tests COMMAND tinysctp_udp_tests)
add_test(NAME tinysctp_edge_tests COMMAND tinysctp_edge_tests)
