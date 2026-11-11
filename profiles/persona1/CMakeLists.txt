# Persona 1 (PSP, ULUS10432) profile for PSPRecomp.
# Built by the root project with:  -DPSPRECOMP_PROFILE=persona1

file(GLOB PERSONA1_GENERATED CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/generated/*.cpp")
if(NOT PERSONA1_GENERATED)
    message(FATAL_ERROR "No generated sources found in ${CMAKE_CURRENT_SOURCE_DIR}/generated")
endif()

if(MSVC)
    # The generated units are large single translation units. Keep inlining modest
    # and allow big object files; this configuration compiled cleanly in testing.
    set_source_files_properties(${PERSONA1_GENERATED} PROPERTIES
        COMPILE_OPTIONS "/O2;/Ob1;/bigobj")
endif()

add_executable(Persona1Native
    host/main.cpp
    host/persona1_kernel.cpp
    host/persona1_kernel.hpp
    host/persona1_io.cpp
    host/persona1_io.hpp
    host/persona1_ge.cpp
    host/persona1_ge.hpp
    host/persona1_audio.cpp
    host/persona1_audio.hpp
    host/persona1_movie.cpp
    host/persona1_movie.hpp
    host/persona1_display.cpp
    host/persona1_display.hpp
    host/persona1_mods.cpp
    host/persona1_mods.hpp
    ${PERSONA1_GENERATED})
target_include_directories(Persona1Native PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/generated"
    "${CMAKE_CURRENT_SOURCE_DIR}/host")
target_link_libraries(Persona1Native PRIVATE psprecomp_core)
if(WIN32)
    target_link_libraries(Persona1Native PRIVATE user32 gdi32 winmm d3d11 dxgi d3dcompiler windowscodecs ole32 uuid)
endif()

# FFmpeg (ATRAC3/ATRAC3+ music). Found in vcpkg; override with -DPERSONA1_FFMPEG_ROOT=<dir>.
# Without it the game still runs, the music is just silent.
set(PERSONA1_FFMPEG_ROOT "C:/vcpkg/installed/x64-windows" CACHE PATH "FFmpeg install prefix (contains include/ lib/ bin/)")
if(EXISTS "${PERSONA1_FFMPEG_ROOT}/include/libavcodec/avcodec.h")
    message(STATUS "Persona1: FFmpeg found in ${PERSONA1_FFMPEG_ROOT}")
    target_compile_definitions(Persona1Native PRIVATE PERSONA1_HAVE_FFMPEG=1)
    target_include_directories(Persona1Native PRIVATE "${PERSONA1_FFMPEG_ROOT}/include")
    target_link_libraries(Persona1Native PRIVATE
        "${PERSONA1_FFMPEG_ROOT}/lib/avcodec.lib"
        "${PERSONA1_FFMPEG_ROOT}/lib/avutil.lib")
    # The runtime DLLs must sit next to the exe.
    file(GLOB PERSONA1_FFMPEG_DLLS "${PERSONA1_FFMPEG_ROOT}/bin/*.dll")
    add_custom_command(TARGET Persona1Native POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${PERSONA1_FFMPEG_DLLS} $<TARGET_FILE_DIR:Persona1Native>)
else()
    message(WARNING "Persona1: FFmpeg not found at ${PERSONA1_FFMPEG_ROOT}; music will be silent. Set -DPERSONA1_FFMPEG_ROOT=...")
endif()
if(MSVC)
    target_compile_options(Persona1Native PRIVATE /W3 /permissive- /EHsc /MP3)
endif()
