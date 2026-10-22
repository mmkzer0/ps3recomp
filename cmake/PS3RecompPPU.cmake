# PPU lift-and-build support for a recompiled title.
#
# A title includes this file and calls three functions instead of repeating
# the loader/lifter invocation, the HLE NID table generation and the boot
# scaffold source list in every port:
#
#   include("${PS3RECOMP_DIR}/cmake/PS3RecompPPU.cmake")
#
#   ps3recomp_ppu_lift(LIFTED INPUT "${EBOOT}" OUTPUT_DIR "${CMAKE_BINARY_DIR}/recomp")
#   ps3recomp_ppu_hle_nids(HLE_NID_TABLE)
#   ps3recomp_ppu_scaffold(SCAFFOLD)
#
#   add_executable(game main.cpp ${LIFTED} ${HLE_NID_TABLE} ${SCAFFOLD})
#   target_include_directories(game PRIVATE "${LIFTED_INCLUDE_DIR}" ...)
#
# The lift is a build product, not a configured source list: it runs at build
# time through add_custom_command with a fixed set of output names (--chunks N
# cuts by guest bytes, so a relift that changes one function rewrites one
# file). The ELF and the loader/lifter scripts are DEPENDS, so editing either
# relifts.
#
# The generated translation units are large. Give them -w and
# -ffp-contract=off, and an -O1 (or -O0) rather than the Release -O3: see
# templates/project/CMakeLists.txt for the flags a title applies.

# Exclusive end of executable code, read out of the ELF itself. Section
# headers are authoritative (the highest SHF_EXECINSTR section end); a
# section-stripped image falls back to the executable PT_LOAD end. The loader
# owns that definition, so this asks it rather than parsing ELF in CMake.
function(ps3recomp_ppu_code_end elf out_var)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${PS3RECOMP_DIR}/tools/ppu_loader.py"
                --code-end "${elf}"
        OUTPUT_VARIABLE _value OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_VARIABLE _error RESULT_VARIABLE _result)
    if(NOT _result EQUAL 0 OR NOT _value MATCHES "^0x[0-9A-Fa-f]+$")
        message(FATAL_ERROR "Could not read the code end of ${elf}:\n${_value}\n${_error}")
    endif()
    set(${out_var} "${_value}" PARENT_SCOPE)
endfunction()

# Lift an ELF at build time.
#
#   ps3recomp_ppu_lift(<out_var>
#       INPUT <elf> OUTPUT_DIR <dir>
#       [CHUNKS <n>]            source chunks (default 64; deterministic names)
#       [CODE_END <addr>|auto]  default: the ELF's own executable end
#       [JOBS <n>]              lifter worker processes
#       [SOURCE_NAME <name>]    default ppu_recomp; emits <name>_NNN.cpp
#       [HEADER_NAME <name>]    default ppu_recomp.h
#       [LIFTER_ARGS <arg>...]) extra arguments, for lifter options a port needs
#
# Sets in the parent scope:
#   <out_var>              the generated .cpp sources (chunks + function table)
#   <out_var>_HEADER       the generated header
#   <out_var>_INCLUDE_DIR  the directory to add to target_include_directories
#   <out_var>_LOADER_DIR   loader JSON (functions/imports/image) for reporting
function(ps3recomp_ppu_lift out_var)
    cmake_parse_arguments(ARG "" "INPUT;OUTPUT_DIR;CHUNKS;CODE_END;JOBS;SOURCE_NAME;HEADER_NAME"
                          "LIFTER_ARGS" ${ARGN})
    if(NOT ARG_INPUT OR NOT ARG_OUTPUT_DIR)
        message(FATAL_ERROR "ps3recomp_ppu_lift: INPUT <elf> and OUTPUT_DIR <dir> are required")
    endif()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)

    get_filename_component(_elf "${ARG_INPUT}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    get_filename_component(_out "${ARG_OUTPUT_DIR}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_BINARY_DIR}")
    if(NOT EXISTS "${_elf}")
        message(FATAL_ERROR "ps3recomp_ppu_lift: no ELF at ${_elf}")
    endif()

    set(_chunks "${ARG_CHUNKS}")
    if(NOT _chunks)
        set(_chunks 64)
    endif()
    set(_base "${ARG_SOURCE_NAME}")
    if(NOT _base)
        set(_base "ppu_recomp")
    endif()
    set(_header "${ARG_HEADER_NAME}")
    if(NOT _header)
        set(_header "ppu_recomp.h")
    endif()

    if(ARG_CODE_END AND NOT ARG_CODE_END STREQUAL "auto")
        set(_code_end "${ARG_CODE_END}")
    else()
        ps3recomp_ppu_code_end("${_elf}" _code_end)
    endif()

    get_filename_component(_stem "${_elf}" NAME_WE)
    set(_loader_dir "${_out}/loader")

    set(_loader_outputs
        "${_loader_dir}/${_stem}.functions.json"
        "${_loader_dir}/${_stem}.image.json"
        "${_loader_dir}/${_stem}.loader.json"
        "${_loader_dir}/${_stem}.imports.json")

    set(_lift_outputs "${_out}/${_header}")
    set(_sources "")
    math(EXPR _last "${_chunks} - 1")
    foreach(_i RANGE 0 ${_last})
        if(_i LESS 10)
            set(_pad "00${_i}")
        elseif(_i LESS 100)
            set(_pad "0${_i}")
        else()
            set(_pad "${_i}")
        endif()
        list(APPEND _sources "${_out}/${_base}_${_pad}.cpp")
    endforeach()
    list(APPEND _sources "${_out}/${_base}_table.cpp")
    list(APPEND _lift_outputs ${_sources})

    set(_lifter_extra "")
    if(ARG_JOBS)
        list(APPEND _lifter_extra --jobs "${ARG_JOBS}")
    endif()
    list(APPEND _lifter_extra ${ARG_LIFTER_ARGS})

    add_custom_command(
        OUTPUT ${_loader_outputs} ${_lift_outputs}
        COMMAND "${Python3_EXECUTABLE}" "${PS3RECOMP_DIR}/tools/ppu_loader.py"
                "${_elf}" --output "${_loader_dir}"
        COMMAND "${Python3_EXECUTABLE}" "${PS3RECOMP_DIR}/tools/ppu_lifter.py"
                "${_elf}"
                --functions "${_loader_dir}/${_stem}.functions.json"
                --hle-stubs "${_loader_dir}/${_stem}.imports.json"
                --chunks "${_chunks}"
                --code-end "${_code_end}"
                --output "${_out}"
                ${_lifter_extra}
        DEPENDS "${_elf}"
                "${PS3RECOMP_DIR}/tools/ppu_loader.py"
                "${PS3RECOMP_DIR}/tools/ppu_lifter.py"
                "${PS3RECOMP_DIR}/tools/ppu_disasm.py"
                "${PS3RECOMP_DIR}/tools/elf_parser.py"
        COMMENT "Lifting PPU code from ${_stem} (${_chunks} chunks, code end ${_code_end})"
        VERBATIM)

    set(${out_var} "${_sources}" PARENT_SCOPE)
    set(${out_var}_HEADER "${_out}/${_header}" PARENT_SCOPE)
    set(${out_var}_INCLUDE_DIR "${_out}" PARENT_SCOPE)
    set(${out_var}_LOADER_DIR "${_loader_dir}" PARENT_SCOPE)
endfunction()

# The HLE NID -> handler table: a hand-curated one beside the lift when the
# title has one, otherwise the canonical every-module table generated from the
# toolkit's handler definitions. Reconfigures when a module is added and
# rebuilds when an existing module changes.
function(ps3recomp_ppu_hle_nids out_var)
    cmake_parse_arguments(ARG "" "RECOMP_DIR" "" ${ARGN})
    if(ARG_RECOMP_DIR AND EXISTS "${ARG_RECOMP_DIR}/ppu_hle_nids.cpp")
        set(${out_var} "${ARG_RECOMP_DIR}/ppu_hle_nids.cpp" PARENT_SCOPE)
        message(STATUS "HLE NID table:   ${ARG_RECOMP_DIR}/ppu_hle_nids.cpp")
        return()
    endif()

    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(_table "${CMAKE_CURRENT_BINARY_DIR}/ppu_hle_nids.cpp")
    # The generator discovers handlers from library definitions. Rebuild when
    # an existing module changes, and reconfigure when a new module is added.
    file(GLOB_RECURSE _hle_sources CONFIGURE_DEPENDS "${PS3RECOMP_DIR}/libs/*.c")
    # A removed input cannot make the output older than a remaining
    # dependency. Track the list itself so deletions also remove stale
    # handler references.
    set(_source_list "${CMAKE_CURRENT_BINARY_DIR}/hle_nid_sources.txt")
    file(GENERATE OUTPUT "${_source_list}" CONTENT "${_hle_sources}\n")
    add_custom_command(
        OUTPUT "${_table}"
        COMMAND "${Python3_EXECUTABLE}" "${PS3RECOMP_DIR}/tools/gen_hle_nids.py"
                --out "${_table}" --all
        DEPENDS "${PS3RECOMP_DIR}/tools/gen_hle_nids.py"
                "${PS3RECOMP_DIR}/tools/nid_database.py"
                ${_hle_sources} "${_source_list}"
        COMMENT "Generating the HLE NID -> handler table")
    set(${out_var} "${_table}" PARENT_SCOPE)
    message(STATUS "HLE NID table:   generated (every module)")
endfunction()

# The PPU boot scaffold. It is compiled into the title target rather than
# taken from ps3recomp_runtime: every file #includes the lifter's per-game
# ppu_recomp.h, so it can only exist against one particular lift.
function(ps3recomp_ppu_scaffold out_var)
    set(_scaffold
        "${PS3RECOMP_DIR}/runtime/ppu/ppu_loader.cpp"
        "${PS3RECOMP_DIR}/runtime/ppu/ppu_hle.cpp"
        "${PS3RECOMP_DIR}/runtime/ppu/ppu_fs.cpp"
        "${PS3RECOMP_DIR}/runtime/ppu/ppu_sysprx.cpp")
    set(${out_var} "${_scaffold}" PARENT_SCOPE)
endfunction()
