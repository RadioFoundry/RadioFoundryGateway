# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
# rust built by cargo as a static library, for the host tests and the ESP-IDF build.
#   rdof_rust_add(<name> [TARGET <triple>] [TOOLCHAIN <rustup toolchain>] [BUILD_STD]
#                 [NO_DEFAULT_FEATURES] [FEATURES <feature>...])
# makes <name> an imported static library (librdof_rust.a) that cargo rebuilds when a .rs file changes, without
# Rust's compiler_builtins (strip_builtins.cmake). The default feature, legacy, is the modules that replace common's
# C; t3 is the trunked site (NO_DEFAULT_FEATURES FEATURES t3 builds it alone).
set(RDOF_RUST_DIR ${CMAKE_CURRENT_LIST_DIR})
# common modules the crate replaces: common/src/<module>.c each.
set(RDOF_RUST_MODULES ambe benchcfg benchtx bits config hblink jitter json jsonp lastheard layer2 mmdvm modem netcalls oled parrot pdu rawburst repeater slotgrid status tracker txvr usbmatch)
# C built with them: the check of the structs they share (layout.c).
set(RDOF_RUST_C_SRCS ${RDOF_RUST_DIR}/layout.c)

# rdof_rust_check(<var> [TARGET <triple>] [TOOLCHAIN <rustup toolchain>] [BUILD_STD]): what the build above would
# need and this machine lacks, one per line in <var>; empty when it's all there.
function(rdof_rust_check var)
    cmake_parse_arguments(A "BUILD_STD" "TARGET;TOOLCHAIN" "" ${ARGN})
    set(miss "")
    find_program(RDOF_CARGO cargo HINTS $ENV{HOME}/.cargo/bin)
    if(NOT RDOF_CARGO)
        string(APPEND miss "  - cargo (Rust, stable): https://rustup.rs\n")
        if(A_TOOLCHAIN STREQUAL "esp")
            string(APPEND miss "  - Espressif's Rust toolchain: espup install --targets esp32s3 --std\n")
        elseif(A_TARGET)
            string(APPEND miss "  - the Rust target ${A_TARGET}: rustup target add ${A_TARGET}\n")
        endif()
        set(${var} "${miss}" PARENT_SCOPE)
        return()
    endif()
    get_filename_component(bin ${RDOF_CARGO} DIRECTORY)
    set(tc)
    if(A_TOOLCHAIN)
        set(tc +${A_TOOLCHAIN})
        execute_process(COMMAND ${RDOF_CARGO} ${tc} --version RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET)
        if(NOT rc EQUAL 0)
            if(A_TOOLCHAIN STREQUAL "esp")
                string(APPEND miss "  - Espressif's Rust toolchain ('esp'): espup install --targets esp32s3 --std\n")
            else()
                string(APPEND miss "  - the Rust toolchain '${A_TOOLCHAIN}': rustup toolchain install ${A_TOOLCHAIN}\n")
            endif()
            set(${var} "${miss}" PARENT_SCOPE)
            return()
        endif()
    endif()
    find_program(RDOF_RUSTC rustc HINTS ${bin} $ENV{HOME}/.cargo/bin)
    if(NOT RDOF_RUSTC)
        string(APPEND miss "  - rustc (Rust, stable): https://rustup.rs\n")
    elseif(A_BUILD_STD)  # core is built from the toolchain's source
        execute_process(COMMAND ${RDOF_RUSTC} ${tc} --print sysroot RESULT_VARIABLE rc OUTPUT_VARIABLE root
            OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(NOT rc EQUAL 0 OR NOT EXISTS "${root}/lib/rustlib/src/rust/library/core")
            string(APPEND miss "  - the toolchain's Rust source (rust-src): espup install --targets esp32s3 --std\n")
        endif()
    elseif(A_TARGET)
        execute_process(COMMAND ${RDOF_RUSTC} ${tc} --print target-libdir --target ${A_TARGET} RESULT_VARIABLE rc
            OUTPUT_VARIABLE libdir OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(NOT rc EQUAL 0 OR NOT EXISTS "${libdir}")
            string(APPEND miss "  - the Rust target ${A_TARGET}: rustup target add ${A_TARGET}\n")
        endif()
    endif()
    set(${var} "${miss}" PARENT_SCOPE)
endfunction()

function(rdof_rust_add name)
    cmake_parse_arguments(A "BUILD_STD;NO_DEFAULT_FEATURES" "TARGET;TOOLCHAIN" "FEATURES" ${ARGN})
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
    if(A_NO_DEFAULT_FEATURES)
        list(APPEND cmd --no-default-features)
    endif()
    if(A_FEATURES)
        list(JOIN A_FEATURES "," features)
        list(APPEND cmd --features ${features})
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

# rdof_t3_lib(<var> <common target>): what a program that runs the trunked site's controller (rdof/t3.h) links
# beside <common target>: nothing when that library's Rust has the t3 feature already (RDOF_RUST_T3), else rust's t3
# feature built alone (for RDOF_RUST_TARGET when set), else NOTFOUND (no cargo).
function(rdof_t3_lib var common)
    get_target_property(defs ${common} INTERFACE_COMPILE_DEFINITIONS)
    if(defs MATCHES "RDOF_RUST_T3=1")
        set(${var} "" PARENT_SCOPE)
        return()
    endif()
    find_program(RDOF_CARGO cargo HINTS $ENV{HOME}/.cargo/bin)
    if(NOT RDOF_CARGO)
        set(${var} NOTFOUND PARENT_SCOPE)
        return()
    endif()
    if(RDOF_RUST_TARGET)
        string(REPLACE "-" "_" name rdof_rust_t3_${RDOF_RUST_TARGET})
        if(NOT TARGET ${name})
            rdof_rust_add(${name} TARGET ${RDOF_RUST_TARGET} NO_DEFAULT_FEATURES FEATURES t3)
        endif()
    else()
        set(name rdof_rust_t3)
        if(NOT TARGET ${name})
            rdof_rust_add(${name} NO_DEFAULT_FEATURES FEATURES t3)
        endif()
    endif()
    set(${var} ${name} PARENT_SCOPE)
endfunction()
