# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
# rust built by cargo as a static library, for the host tests and the ESP-IDF build.
#   rdof_rust_add(<name> [TARGET <triple>] [TOOLCHAIN <rustup toolchain>] [BUILD_STD])
# makes <name> an imported static library (librdof_rust.a) that cargo rebuilds when a .rs file changes, without
# Rust's compiler_builtins (strip_builtins.cmake).
set(RDOF_RUST_DIR ${CMAKE_CURRENT_LIST_DIR})
# common modules the crate replaces: common/src/<module>.c each.
set(RDOF_RUST_MODULES ambe benchcfg benchtx bits config hblink jitter json jsonp layer2 mmdvm modem netcalls oled pdu rawburst repeater slotgrid status tracker txvr usbmatch)
# C built with them: the check of the structs they share (layout.c).
set(RDOF_RUST_C_SRCS ${RDOF_RUST_DIR}/layout.c)

function(rdof_rust_add name)
    cmake_parse_arguments(A "BUILD_STD" "TARGET;TOOLCHAIN" "" ${ARGN})
    find_program(RDOF_CARGO cargo HINTS $ENV{HOME}/.cargo/bin REQUIRED)
    set(dir ${CMAKE_CURRENT_BINARY_DIR}/${name}-cargo)
    set(cmd ${RDOF_CARGO})
    if(A_TOOLCHAIN)
        list(APPEND cmd +${A_TOOLCHAIN})
    endif()
    list(APPEND cmd build --release --manifest-path ${RDOF_RUST_DIR}/Cargo.toml --target-dir ${dir})
    if(A_TARGET)
        list(APPEND cmd --target ${A_TARGET})
        set(lib ${dir}/${A_TARGET}/release/librdof_rust.a)
    else()
        set(lib ${dir}/release/librdof_rust.a)
    endif()
    if(A_BUILD_STD)  # a target without a prebuilt core (Xtensa): build it from rust-src
        list(APPEND cmd -Zbuild-std=core)
    endif()
    file(GLOB_RECURSE srcs CONFIGURE_DEPENDS ${RDOF_RUST_DIR}/src/*.rs)
    set(jobs)
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.28)  # cargo joins make's jobserver (else it warns it can't)
        set(jobs JOB_SERVER_AWARE TRUE)
    endif()
    add_custom_command(OUTPUT ${lib}
        COMMAND ${cmd}
        COMMAND ${CMAKE_COMMAND} -D AR=${CMAKE_AR} -D LIB=${lib} -P ${RDOF_RUST_DIR}/strip_builtins.cmake
        DEPENDS ${srcs} ${RDOF_RUST_DIR}/Cargo.toml ${RDOF_RUST_DIR}/Cargo.lock ${RDOF_RUST_DIR}/strip_builtins.cmake
        COMMENT "cargo: rust for ${name}"
        USES_TERMINAL VERBATIM ${jobs})
    add_custom_target(${name}_cargo DEPENDS ${lib})
    add_library(${name} STATIC IMPORTED GLOBAL)
    set_target_properties(${name} PROPERTIES IMPORTED_LOCATION ${lib})
    add_dependencies(${name} ${name}_cargo)
endfunction()
