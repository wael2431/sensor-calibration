idf_component_register(SRCS "calib_stub.c"
                       INCLUDE_DIRS "include")

# ESP32-C6 = 32-bit RISC-V IMAC, no FPU -> ilp32 ABI.
# Prerequisite: rustup target add riscv32imac-unknown-none-elf
set(RUST_TARGET riscv32imac-unknown-none-elf)
set(CARGO_TARGET_DIR ${CMAKE_CURRENT_BINARY_DIR}/cargo)
set(CALIB_RS_LIB ${CARGO_TARGET_DIR}/${RUST_TARGET}/release/libcalib_rs.a)

# cargo is incremental: run it on every build, it only recompiles when needed.
add_custom_target(calib_rs_cargo ALL
    COMMAND cargo build --release --target ${RUST_TARGET}
            --target-dir ${CARGO_TARGET_DIR}
    BYPRODUCTS ${CALIB_RS_LIB}
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    COMMENT "Building calib_rs (Rust, ${RUST_TARGET})"
    USES_TERMINAL)

add_library(calib_rs_static STATIC IMPORTED GLOBAL)
set_target_properties(calib_rs_static PROPERTIES IMPORTED_LOCATION ${CALIB_RS_LIB})

add_dependencies(${COMPONENT_LIB} calib_rs_cargo)
target_link_libraries(${COMPONENT_LIB} INTERFACE calib_rs_static)

set_property(DIRECTORY APPEND PROPERTY ADDITIONAL_CLEAN_FILES ${CARGO_TARGET_DIR})
