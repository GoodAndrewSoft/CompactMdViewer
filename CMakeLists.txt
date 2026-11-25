cmake_minimum_required(VERSION 3.20)
project(MdViewer CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# Static CRT (/MT in Release, /MTd in Debug): no VCRUNTIME140.dll
# needed next to the exe, it stays a single file.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(MdViewer WIN32
    src/main.cpp
    src/app.rc
    third_party/md4c/md4c-release-0.5.3/src/md4c.c
    third_party/md4c/md4c-release-0.5.3/src/md4c-html.c
    third_party/md4c/md4c-release-0.5.3/src/entity.c
)

target_include_directories(MdViewer PRIVATE
    third_party/wv2/build/native/include
    third_party/md4c/md4c-release-0.5.3/src
)

target_compile_definitions(MdViewer PRIVATE
    UNICODE
    _UNICODE
    _WIN32_WINNT=0x0A00
    WIN32_LEAN_AND_MEAN
)

# Sources are UTF-8. /W4 for C++ only:
# md4c is noisy with /W4 warnings that are not ours.
target_compile_options(MdViewer PRIVATE
    /utf-8
    $<$<COMPILE_LANGUAGE:CXX>:/W4>
)

target_link_libraries(MdViewer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/wv2/build/native/x64/WebView2LoaderStatic.lib
    shlwapi
    shell32
    ole32
    oleaut32
    user32
    gdi32
    advapi32
    comdlg32
    dwmapi
)

# After a Release build, copy the exe to the author's deploy folder
# (F:\Run\Textutils\mdviewer). A Debug build is never deployed, so it
# cannot overwrite the working exe. The script itself copies only when
# that folder exists; on other machines the step is skipped.
# Disable entirely with -DMDVIEWER_DEPLOY=OFF at configure time.
option(MDVIEWER_DEPLOY "Copy the Release exe to the deploy folder" ON)
if(MDVIEWER_DEPLOY)
    add_custom_command(TARGET MdViewer POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -D "EXE=$<TARGET_FILE:MdViewer>"
            -D "CONFIG=$<CONFIG>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/copy-release.cmake"
        COMMENT "Deploy Release exe to F:/Run/Textutils/mdviewer (Release only)"
    )
endif()
