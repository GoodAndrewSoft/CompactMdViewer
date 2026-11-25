# Copy the Release exe to the author's deploy folder — but only when that
# folder exists (personal setup). On any other machine this step is skipped
# instead of creating stray directories. Called from CMakeLists.txt via
# POST_BUILD; CONFIG comes from $<CONFIG>. Disable entirely with
# -DMDVIEWER_DEPLOY=OFF at configure time.
if(NOT CONFIG STREQUAL "Release")
    return()
endif()
set(_deploy_dir "F:/Run/Textutils/mdviewer")
if(NOT IS_DIRECTORY "${_deploy_dir}")
    message(STATUS "MdViewer: deploy dir ${_deploy_dir} not found, skipping deploy")
    return()
endif()
file(MAKE_DIRECTORY "${_deploy_dir}")
execute_process(
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${EXE}" "${_deploy_dir}/MdViewer.exe"
    RESULT_VARIABLE _copy_result
)
if(NOT _copy_result EQUAL 0)
    message(WARNING "MdViewer: failed to copy exe to ${_deploy_dir}")
else()
    message(STATUS "MdViewer: copied ${EXE} to ${_deploy_dir}/MdViewer.exe")
endif()
