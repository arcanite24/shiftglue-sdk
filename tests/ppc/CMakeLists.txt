# PowerPC Instruction Test Suite
# Compiles PPC assembly tests and generates Catch2 test cases

include(ppc_test_pipeline)

# Output directories
set(PPC_OBJ_DIR ${CMAKE_BINARY_DIR}/tests/ppc/obj)
set(PPC_BIN_DIR ${CMAKE_BINARY_DIR}/tests/ppc/bin)
set(PPC_GEN_DIR ${CMAKE_BINARY_DIR}/tests/ppc/generated)

file(MAKE_DIRECTORY ${PPC_OBJ_DIR})
file(MAKE_DIRECTORY ${PPC_BIN_DIR})
file(MAKE_DIRECTORY ${PPC_GEN_DIR})

# Collect and build all assembly test files. REXGLUE_PPC_TEST_EXTRA_ASM_DIRS
# adds directories kept outside the repository (xenia-canary's generated
# corpus, see README.md); a file name must be unique across all of them.
set(REXGLUE_PPC_TEST_EXTRA_ASM_DIRS "" CACHE STRING
    "Extra directories of .s PPC tests to build into ppc_tests (semicolon-separated)")
file(GLOB ASM_FILES ${CMAKE_CURRENT_SOURCE_DIR}/asm/*.s)
set(PPC_ASM_DIR_ARGS --asm-dir=${CMAKE_CURRENT_SOURCE_DIR}/asm)
foreach(EXTRA_DIR ${REXGLUE_PPC_TEST_EXTRA_ASM_DIRS})
    file(GLOB EXTRA_ASM_FILES ${EXTRA_DIR}/*.s)
    list(APPEND ASM_FILES ${EXTRA_ASM_FILES})
    list(APPEND PPC_ASM_DIR_ARGS --asm-dir=${EXTRA_DIR})
endforeach()
list(LENGTH ASM_FILES NUM_TESTS)

if(NUM_TESTS EQUAL 0)
    message(WARNING "No test files found in ${CMAKE_CURRENT_SOURCE_DIR}/asm/")
    return()
endif()

message(STATUS "Found ${NUM_TESTS} PPC instruction test files")

foreach(ASM_FILE ${ASM_FILES})
    ppc_add_test_binary(${ASM_FILE} ${PPC_OBJ_DIR} ${PPC_BIN_DIR})
endforeach()

get_property(TEST_BINS GLOBAL PROPERTY PPC_TEST_BINS)
get_property(TEST_MAPS GLOBAL PROPERTY PPC_TEST_MAPS)

# Generate Catch2 test code from recompiled binaries: one pair of translation
# units per test file, so a large corpus compiles in parallel.
set(PPC_GEN_SOURCES)
foreach(ASM_FILE ${ASM_FILES})
    get_filename_component(BASE_NAME ${ASM_FILE} NAME_WE)
    list(APPEND PPC_GEN_SOURCES
        ${PPC_GEN_DIR}/${BASE_NAME}.functions.cpp
        ${PPC_GEN_DIR}/${BASE_NAME}.cases.cpp)
endforeach()

add_custom_command(
    OUTPUT ${PPC_GEN_SOURCES}
           ${PPC_GEN_DIR}/ppc_test_decls.h
           ${PPC_GEN_DIR}/ppc_config.h
    COMMAND $<TARGET_FILE:rexglue> recompile-tests
            --output=${PPC_GEN_DIR}
            --bin-dir=${PPC_BIN_DIR}
            ${PPC_ASM_DIR_ARGS}
            --skip-file=${CMAKE_CURRENT_SOURCE_DIR}/skip.txt
    DEPENDS ${TEST_BINS} ${TEST_MAPS} ${ASM_FILES} ${CMAKE_CURRENT_SOURCE_DIR}/skip.txt rexglue
    COMMENT "Generating Catch2 test code with rexglue"
    VERBATIM
)

add_executable(ppc_tests ${PPC_GEN_SOURCES})

set_source_files_properties(
    ${PPC_GEN_SOURCES}
    ${PPC_GEN_DIR}/ppc_test_decls.h
    ${PPC_GEN_DIR}/ppc_config.h
    PROPERTIES GENERATED TRUE
)

target_include_directories(ppc_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PPC_GEN_DIR}
)

target_link_libraries(ppc_tests PRIVATE
    simde
    rexruntime
    fmt::fmt
    Catch2::Catch2WithMain
)

# Delay-load FidelityFX DLLs so ppc_tests can run without them.
if(WIN32)
    if(TARGET amd_fidelityfx_vk)
        target_link_options(ppc_tests PRIVATE
            "LINKER:/DELAYLOAD:$<TARGET_FILE_NAME:amd_fidelityfx_vk>")
    endif()
    if(TARGET amd_fidelityfx_dx12)
        target_link_options(ppc_tests PRIVATE
            "LINKER:/DELAYLOAD:$<TARGET_FILE_NAME:amd_fidelityfx_dx12>")
    endif()
    if(TARGET amd_fidelityfx_vk OR TARGET amd_fidelityfx_dx12)
        target_link_libraries(ppc_tests PRIVATE delayimp)
    endif()
endif()

set_target_properties(ppc_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexruntime>
)
if(UNIX AND NOT APPLE)
    set_target_properties(ppc_tests PROPERTIES
        INSTALL_RPATH "$ORIGIN"
        BUILD_WITH_INSTALL_RPATH ON
    )
endif()

target_compile_options(ppc_tests PRIVATE
    -UTRACY_ENABLE -UREXGLUE_ENABLE_PROFILING
    -UREXGLUE_ENABLE_PERF_COUNTERS -UREXGLUE_PROFILE_GUEST_FUNCTIONS
    -msse4.1 -mssse3
)

# CTest discovery parses the case list in CMake script, which does not finish
# in reasonable time for a corpus of a hundred thousand cases: with extra
# directories, run the executable directly instead.
if(NOT REXGLUE_PPC_TEST_EXTRA_ASM_DIRS)
    include(Catch)
    catch_discover_tests(ppc_tests
        TEST_PREFIX "ppc."
        PROPERTIES
            LABELS "ppc;instructions"
            TIMEOUT 60
    )
endif()

# The region oracle's interpreter (rex/ppc/interpreter.h) on the same cases:
# it reads the assembled binaries, maps and #_ directives at run time, so it
# needs no generated code and checks the interpreter independently of the
# recompiler (interp_cases.cpp).
set(PPC_INTERP_ASM_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/asm ${REXGLUE_PPC_TEST_EXTRA_ASM_DIRS})
string(REPLACE ";" "|" PPC_INTERP_ASM_DIRS "${PPC_INTERP_ASM_DIRS}")
add_custom_target(ppc_test_binaries DEPENDS ${TEST_BINS} ${TEST_MAPS})
add_executable(ppc_interp_tests interp_cases.cpp)
add_dependencies(ppc_interp_tests ppc_test_binaries)
target_include_directories(ppc_interp_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_compile_definitions(ppc_interp_tests PRIVATE
    PPC_INTERP_BIN_DIR="${PPC_BIN_DIR}"
    PPC_INTERP_ASM_DIRS="${PPC_INTERP_ASM_DIRS}"
    PPC_INTERP_SKIP_FILE="${CMAKE_CURRENT_SOURCE_DIR}/skip.txt")
target_link_libraries(ppc_interp_tests PRIVATE simde rexruntime fmt::fmt Catch2::Catch2WithMain)
set_target_properties(ppc_interp_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:rexruntime>)
if(UNIX AND NOT APPLE)
    set_target_properties(ppc_interp_tests PROPERTIES
        INSTALL_RPATH "$ORIGIN"
        BUILD_WITH_INSTALL_RPATH ON)
endif()
target_compile_options(ppc_interp_tests PRIVATE -UTRACY_ENABLE -UREXGLUE_ENABLE_PROFILING)
if(NOT REXGLUE_PPC_TEST_EXTRA_ASM_DIRS)
    add_test(NAME ppc.interpreter COMMAND ppc_interp_tests)
endif()

message(STATUS "Configured PPC test suite with ${NUM_TESTS} test files")
