# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 The RadioFoundry Gateway authors
# cmake -D AR=<ar> -D LIB=<librdof_rust.a> -P strip_builtins.cmake
# Keeps only the crate's own object in the archive (LTO puts core in it) and drops Rust's compiler_builtins. It
# defines C's libm (sinf, sqrtf, expf, ...) and libgcc's helpers too, and the archive links ahead of newlib and
# libgcc: left in, every C caller in the image would get Rust's versions, depending on link order. Without it the
# Rust's few helpers come from libgcc, as the C's do; one it needs that libgcc lacks fails the link.
execute_process(COMMAND ${AR} t ${LIB} OUTPUT_VARIABLE members COMMAND_ERROR_IS_FATAL ANY)
string(REPLACE "\n" ";" members "${members}")
list(FILTER members EXCLUDE REGEX "^(rdof_rust-.*|__\\.SYMDEF.*)?$")
if(members)
    execute_process(COMMAND ${AR} d ${LIB} ${members} COMMAND_ERROR_IS_FATAL ANY)
endif()
