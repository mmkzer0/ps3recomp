# ps3recomp - tests/video_resolutions
#
# video.resolutions and the cellVideoOut answers that must agree with it (availability, device
# mode list, GetState/GetConfiguration). The value comes in through the typed option's
# environment form, set per test; the loader's rejection of an invalid id is also run here.
#
#   ctest --test-dir build/dev -R video_resolutions --output-on-failure

add_executable(test_video_resolutions
    main.c
    "${PROJECT_SOURCE_DIR}/runtime/host/host_stubs.c"
)
target_link_libraries(test_video_resolutions PRIVATE ps3recomp_runtime)
if(NOT WIN32)
    target_link_libraries(test_video_resolutions PRIVATE m)
endif()
target_include_directories(test_video_resolutions PRIVATE
    "${PROJECT_SOURCE_DIR}/libs/video"
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/runtime"
)
set_target_properties(test_video_resolutions PROPERTIES LINKER_LANGUAGE CXX)

function(video_resolutions_case name value)
    add_test(NAME video_resolutions_${name} COMMAND test_video_resolutions ${name})
    if(value STREQUAL "")
        set_tests_properties(video_resolutions_${name} PROPERTIES
            ENVIRONMENT_MODIFICATION "PS3RECOMP_VIDEO_RESOLUTIONS=unset:")
    else()
        set_tests_properties(video_resolutions_${name} PROPERTIES
            ENVIRONMENT "PS3RECOMP_VIDEO_RESOLUTIONS=${value}")
    endif()
    set_tests_properties(video_resolutions_${name} PROPERTIES LABELS "video;t0")
endfunction()

video_resolutions_case(default "")
video_resolutions_case(only720 "2")
video_resolutions_case(only1080 "1")
video_resolutions_case(pair "4,1")
video_resolutions_case(extended "13,10")
video_resolutions_case(configure "1,2")
video_resolutions_case(checks "")

# cellVideoOutConfigure on an output other than PRIMARY: no firmware or oracle answer, so a named stop.
add_test(NAME video_resolutions_configure_secondary COMMAND test_video_resolutions configure_secondary)
set_tests_properties(video_resolutions_configure_secondary PROPERTIES
    ENVIRONMENT_MODIFICATION "PS3RECOMP_VIDEO_RESOLUTIONS=unset:"
    PASS_REGULAR_EXPRESSION "PS3STOP v=1 kind=gcm-unmodelled[^\n]*func=cellVideoOutConfigure[^\n]*feature=video-out"
    LABELS "video;t0")

# An id outside the known set stops the load with config-invalid, naming the key.
add_test(NAME video_resolutions_invalid COMMAND test_video_resolutions default)
set_tests_properties(video_resolutions_invalid PROPERTIES
    ENVIRONMENT "PS3RECOMP_VIDEO_RESOLUTIONS=3"
    PASS_REGULAR_EXPRESSION "PS3STOP v=1 kind=config-invalid[^\n]*key=video.resolutions"
    LABELS "video;t0")
