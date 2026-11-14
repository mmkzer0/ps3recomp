# Generate all discovered SPU ELF programs from explicitly supplied input files.
# The helper runs at configure time so the source list belongs to this exact
# generation. Input/tool changes trigger reconfiguration before compilation.
# PS3RECOMP_SPU_MERGE_CHUNKS=ON lifts with --merge-chunks: the chunks of one SPU
# function become one C function, and branches between them gotos rather than
# trampoline round trips through the drain loop.
function(ps3recomp_generate_spu output_dir)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(command "${Python3_EXECUTABLE}" "${PS3RECOMP_DIR}/tools/build_spu_workloads.py"
        --lifted "${output_dir}" --out "${output_dir}/spu_workloads.c" --constructor)
    if(PS3RECOMP_SPU_MERGE_CHUNKS)
        list(APPEND command --merge-chunks)
    endif()
    foreach(input IN LISTS ARGN)
        get_filename_component(input "${input}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        list(APPEND command --input "${input}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${input}")
    endforeach()
    foreach(tool IN ITEMS build_spu_workloads.py spu_program_pipeline.py spu_elf.py
                          extract_spu_images.py spu_lifter.py find_spu_functions.py
                          spu_disasm.py check_spu_lifter_abi.py)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PS3RECOMP_DIR}/tools/${tool}")
    endforeach()
    execute_process(COMMAND ${command} RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "SPU generation incomplete. Inspect ${output_dir}/inventory.json.\n${output}\n${error}")
    endif()
    include("${output_dir}/sources.cmake")
    set(SPU_GENERATED_SOURCES "${SPU_GENERATED_SOURCES}" PARENT_SCOPE)
endfunction()

# Prebuilt SPU_DIR workflow: the template compiles checked-in lifts instead of
# regenerating them, so validate the ABI stamp/check before they reach the
# target. Without this an image lifted before the gate links into the registry
# and its registration never validates against the runtime.
function(ps3recomp_spu_prebuilt_sources out_var count_var spu_dir)
    file(GLOB lifted CONFIGURE_DEPENDS "${spu_dir}/*/spu_recomp.c")
    list(LENGTH lifted image_count)
    set(sources "")
    if(image_count GREATER 0)
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${PS3RECOMP_DIR}/tools/check_spu_lifter_abi.py")
        execute_process(
            COMMAND "${Python3_EXECUTABLE}" "${PS3RECOMP_DIR}/tools/check_spu_lifter_abi.py" "${spu_dir}"
            RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
        if(NOT result EQUAL 0)
            message(FATAL_ERROR
                "SPU lifted code in ${spu_dir} cannot be trusted: it predates the "
                "lifter ABI stamp/check and would never validate against this runtime.\n"
                "${output}${error}\n"
                "Re-lift every image with this toolkit's tools/spu_lifter.py.")
        endif()
        list(APPEND sources ${lifted})
        foreach(gen IN ITEMS spu_workloads.c jobmods_register.c taskset_policy_bytes.c)
            if(EXISTS "${spu_dir}/${gen}")
                list(APPEND sources "${spu_dir}/${gen}")
            else()
                message(STATUS "  (no ${gen} -- not registered)")
            endif()
        endforeach()
    endif()
    set(${out_var} "${sources}" PARENT_SCOPE)
    set(${count_var} ${image_count} PARENT_SCOPE)
endfunction()
