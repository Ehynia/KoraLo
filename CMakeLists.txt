cmake_minimum_required(VERSION 3.20)
project(KoraLoNative VERSION 0.7.0 LANGUAGES CXX)

set(KORALO_REVISION_FILE "${CMAKE_SOURCE_DIR}/BUILD_REVISION.txt")
if(NOT EXISTS "${KORALO_REVISION_FILE}")
    message(FATAL_ERROR "BUILD_REVISION.txt is missing from the KoraLo source package")
endif()
file(STRINGS "${KORALO_REVISION_FILE}" KORALO_SOURCE_REVISION LIMIT_COUNT 1 ENCODING UTF-8)
string(STRIP "${KORALO_SOURCE_REVISION}" KORALO_SOURCE_REVISION)
if(KORALO_SOURCE_REVISION STREQUAL "")
    message(FATAL_ERROR "BUILD_REVISION.txt is empty")
endif()
message(STATUS "KoraLo source revision: ${KORALO_SOURCE_REVISION}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(KORALO_BUILD_GUI "Build desktop GUI" ON)
option(KORALO_BUILD_CLI "Build offline CLI" ON)
# CUDA is optional: the editor must still build and render via CPU without
# nvcc, an NVIDIA device, or the CUDA Toolkit on the target machine.
option(KORALO_ENABLE_CUDA "Build CUDA FSR kernels when NVIDIA CUDA Toolkit is installed" ON)
set(KORALO_CUDA_AVAILABLE OFF)
if(KORALO_ENABLE_CUDA)
    include(CheckLanguage)
    check_language(CUDA)
    if(CMAKE_CUDA_COMPILER)
        enable_language(CUDA)
        find_package(CUDAToolkit QUIET)
        if(CUDAToolkit_FOUND)
            set(KORALO_CUDA_AVAILABLE ON)
            if(NOT CMAKE_CUDA_ARCHITECTURES)
                set(CMAKE_CUDA_ARCHITECTURES 75 86)
            endif()
            message(STATUS "KoraLo CUDA FSR enabled (${CMAKE_CUDA_COMPILER})")
        endif()
    endif()
endif()
if(NOT KORALO_CUDA_AVAILABLE)
    message(STATUS "KoraLo CUDA FSR unavailable - retaining portable CPU FSR and FFmpeg NVENC")
endif()

option(KORALO_ENABLE_ASAN_UBSAN "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(KORALO_ENABLE_TSAN "Enable ThreadSanitizer" OFF)

if(KORALO_ENABLE_ASAN_UBSAN AND KORALO_ENABLE_TSAN)
    message(FATAL_ERROR "ASan/UBSan and TSan require separate build directories")
endif()
if(KORALO_ENABLE_ASAN_UBSAN OR KORALO_ENABLE_TSAN)
    if(MSVC)
        message(FATAL_ERROR "KoraLo sanitizer CMake options currently target GCC/Clang builds")
    endif()
    add_compile_options(-fno-omit-frame-pointer)
    if(KORALO_ENABLE_ASAN_UBSAN)
        add_compile_options(-fsanitize=address,undefined)
        add_link_options(-fsanitize=address,undefined)
    else()
        add_compile_options(-fsanitize=thread)
        add_link_options(-fsanitize=thread)
    endif()
endif()

find_package(Threads REQUIRED)
if(WIN32 AND KORALO_BUILD_GUI)
    enable_language(RC)
endif()

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
endif()

function(koralo_enable_strict_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
    endif()
endfunction()

# Keep third-party OFX loading physically out of the editor core. The protocol
# library is safe to link into KoraLo; only the scanner links the inspector that
# calls LoadLibrary/dlopen on plug-ins.
add_library(koralo_ofx_protocol STATIC
    src/plugins/ofx_ipc.cpp
)
target_include_directories(koralo_ofx_protocol PUBLIC src)
koralo_enable_strict_warnings(koralo_ofx_protocol)

add_library(koralo_ofx_inspector STATIC
    src/plugins/ofx_inspector.cpp
)
target_include_directories(koralo_ofx_inspector PUBLIC src)
koralo_enable_strict_warnings(koralo_ofx_inspector)
if(UNIX AND NOT APPLE)
    target_link_libraries(koralo_ofx_inspector PUBLIC dl)
endif()

add_library(koralo_core STATIC
    src/core/project.cpp
    src/core/vector_animation.cpp
    src/media/vector_renderer.cpp
    src/media/paint_mask.cpp
    src/core/effects.cpp
    src/core/selection_model.cpp
    src/core/edit_controller.cpp
    src/core/vegas_importer.cpp
    src/media/fsr_upscaler.cpp
    src/media/native_cuda_context.cpp
    src/media/frame_cache.cpp
    src/media/ffmpeg_runtime.cpp
    src/media/media_probe.cpp
    src/media/render_planner.cpp
    src/media/render_fast_path.cpp
    src/media/render_control.cpp
    src/media/render_graph.cpp
    src/media/playback_controller.cpp
    src/media/decoder_pool.cpp
    src/media/audio_mix_graph.cpp
    src/media/media_engine.cpp
    src/media/native_effects.cpp
    src/plugins/ofx_host.cpp
    src/platform/process.cpp
    src/platform/app_paths.cpp
    src/ui/editor_app.cpp
    src/ui/editor_commands.cpp
    src/ui/opengl_canvas.cpp
    src/ui/animation_workspace.cpp
    src/ui/localization.cpp
)
if(KORALO_CUDA_AVAILABLE)
    target_sources(koralo_core PRIVATE src/media/fsr_cuda.cu src/media/native_cuda.cu)
    target_link_libraries(koralo_core PRIVATE CUDA::cudart_static)
    set_target_properties(koralo_core PROPERTIES CUDA_RUNTIME_LIBRARY Static CUDA_STANDARD 17 CUDA_STANDARD_REQUIRED ON)
else()
    target_sources(koralo_core PRIVATE src/media/fsr_cuda_stub.cpp src/media/native_cuda_stub.cpp)
endif()
target_include_directories(koralo_core PUBLIC src)
target_link_libraries(koralo_core PUBLIC Threads::Threads PRIVATE koralo_ofx_protocol)
if(MINGW)
    target_link_options(koralo_core INTERFACE -static -static-libgcc -static-libstdc++)
endif()
if(MSVC)
    set_property(TARGET koralo_core PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
if(WIN32)
    target_compile_definitions(koralo_core PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
koralo_enable_strict_warnings(koralo_core)

if(KORALO_BUILD_GUI)
if(WIN32)
    add_executable(KoraLo WIN32 src/platform/win32_main.cpp src/platform/koralo.rc)
    target_link_libraries(KoraLo PRIVATE koralo_core comdlg32 shell32 gdi32 user32 msimg32)
else()
    find_package(X11 REQUIRED)
    find_library(KORALO_XRENDER_LIBRARY Xrender)
    add_executable(KoraLo src/platform/x11_main.cpp)
    target_include_directories(KoraLo PRIVATE ${X11_INCLUDE_DIR})
    target_link_libraries(KoraLo PRIVATE koralo_core ${X11_LIBRARIES} pthread)
    if(KORALO_XRENDER_LIBRARY)
        target_compile_definitions(KoraLo PRIVATE KORALO_HAVE_XRENDER=1)
        target_link_libraries(KoraLo PRIVATE ${KORALO_XRENDER_LIBRARY})
    endif()
endif()

# Runtime language packs stay external so translators can add/update them without recompiling.
add_custom_command(TARGET KoraLo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/locales
            $<TARGET_FILE_DIR:KoraLo>/locales)

if(MINGW)
    target_link_options(KoraLo PRIVATE -municode)
endif()
install(TARGETS KoraLo RUNTIME DESTINATION .)
endif()
if(KORALO_BUILD_CLI)
    add_executable(koralo-cli src/platform/cli_main.cpp)
    target_link_libraries(koralo-cli PRIVATE koralo_core)
    target_compile_definitions(koralo-cli PRIVATE KORALO_SOURCE_REVISION="${KORALO_SOURCE_REVISION}")
    if(MINGW)
        target_link_options(koralo-cli PRIVATE -municode)
    endif()
    install(TARGETS koralo-cli RUNTIME DESTINATION .)
endif()

# OFX discovery runs out-of-process by design. The scanner links only the
# inspector/protocol boundary, not the full editor core.
add_executable(koralo-ofx-scanner src/plugins/ofx_scanner_main.cpp)
target_link_libraries(koralo-ofx-scanner PRIVATE koralo_ofx_inspector koralo_ofx_protocol)
koralo_enable_strict_warnings(koralo-ofx-scanner)
# Scanner only accepts the ASCII switch --jsonl; the request body contains
# UTF-8 JSON. Unlike CLI it deliberately uses main(), so do NOT pass -municode
# here: MinGW -municode expects wmain() and otherwise fails to link.
install(TARGETS koralo-ofx-scanner RUNTIME DESTINATION .)

option(KORALO_BUILD_TESTS "Build native engine tests" ON)
if(KORALO_BUILD_TESTS)
    enable_testing()
    add_executable(koralo_render_ui_tests tests_native/test_render_ui.cpp)
    target_link_libraries(koralo_render_ui_tests PRIVATE koralo_core)
    add_test(NAME koralo_render_ui_tests COMMAND koralo_render_ui_tests)
    set_tests_properties(koralo_render_ui_tests PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
    add_executable(koralo_direct_export_integration_tests tests_native/test_direct_export_integration.cpp)
    target_link_libraries(koralo_direct_export_integration_tests PRIVATE koralo_core)
    koralo_enable_strict_warnings(koralo_direct_export_integration_tests)
    add_test(NAME koralo_direct_export_integration_tests COMMAND koralo_direct_export_integration_tests)
    set_tests_properties(koralo_direct_export_integration_tests PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)
    add_executable(koralo_cut_export_integration_tests tests_native/test_cut_export_integration.cpp)
    target_link_libraries(koralo_cut_export_integration_tests PRIVATE koralo_core)
    koralo_enable_strict_warnings(koralo_cut_export_integration_tests)
    add_test(NAME koralo_cut_export_integration_tests COMMAND koralo_cut_export_integration_tests)
    set_tests_properties(koralo_cut_export_integration_tests PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)
    add_executable(koralo_gpu_fsr_tests tests_native/test_gpu_fsr.cpp)
    target_link_libraries(koralo_gpu_fsr_tests PRIVATE koralo_core)
    koralo_enable_strict_warnings(koralo_gpu_fsr_tests)
    add_test(NAME koralo_gpu_fsr_tests COMMAND koralo_gpu_fsr_tests)
    set_tests_properties(koralo_gpu_fsr_tests PROPERTIES TIMEOUT 180)
    add_executable(koralo_export_improvements_tests tests_native/test_export_improvements.cpp)
    target_link_libraries(koralo_export_improvements_tests PRIVATE koralo_core)
    add_test(NAME koralo_export_improvements_tests COMMAND koralo_export_improvements_tests)
    set_tests_properties(koralo_export_improvements_tests PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77)
    add_executable(koralo_multitrack_tests tests_native/test_multitrack.cpp)
    target_link_libraries(koralo_multitrack_tests PRIVATE koralo_core)
    add_test(NAME koralo_multitrack_tests COMMAND koralo_multitrack_tests)
    set_tests_properties(koralo_multitrack_tests PROPERTIES TIMEOUT 180)
    add_library(koralo_fake_ofx MODULE tests_native/fake_ofx.cpp)
    set_target_properties(koralo_fake_ofx PROPERTIES PREFIX "" SUFFIX ".ofx")
    add_library(koralo_bad_ofx MODULE tests_native/bad_ofx.cpp)
    set_target_properties(koralo_bad_ofx PROPERTIES PREFIX "" SUFFIX ".ofx")
    if(WIN32)
        # Keep the synthetic OFX binary directly under Contents/Win64 even for
        # multi-config generators such as Visual Studio.  Otherwise CMake adds
        # an extra Release/Debug directory and the bundle no longer matches the
        # OpenFX packaging layout used by the scanner.
        set(KORALO_TEST_OFX_ROOT "${CMAKE_BINARY_DIR}/ofx_test")
        set(KORALO_TEST_OFX_BIN_DIR "${KORALO_TEST_OFX_ROOT}/FakeBoris.ofx.bundle/Contents/Win64")
        set(KORALO_TEST_BAD_OFX_ROOT "${CMAKE_BINARY_DIR}/ofx_bad_test")
        set(KORALO_TEST_BAD_OFX_BIN_DIR "${KORALO_TEST_BAD_OFX_ROOT}/Crash.ofx.bundle/Contents/Win64")
        set_target_properties(koralo_fake_ofx PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${KORALO_TEST_OFX_BIN_DIR}"
            LIBRARY_OUTPUT_DIRECTORY "${KORALO_TEST_OFX_BIN_DIR}")
        set_target_properties(koralo_bad_ofx PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${KORALO_TEST_BAD_OFX_BIN_DIR}"
            LIBRARY_OUTPUT_DIRECTORY "${KORALO_TEST_BAD_OFX_BIN_DIR}")
        foreach(cfg Debug Release RelWithDebInfo MinSizeRel)
            string(TOUPPER "${cfg}" cfg_upper)
            set_target_properties(koralo_fake_ofx PROPERTIES
                RUNTIME_OUTPUT_DIRECTORY_${cfg_upper} "${KORALO_TEST_OFX_BIN_DIR}"
                LIBRARY_OUTPUT_DIRECTORY_${cfg_upper} "${KORALO_TEST_OFX_BIN_DIR}")
            set_target_properties(koralo_bad_ofx PROPERTIES
                RUNTIME_OUTPUT_DIRECTORY_${cfg_upper} "${KORALO_TEST_BAD_OFX_BIN_DIR}"
                LIBRARY_OUTPUT_DIRECTORY_${cfg_upper} "${KORALO_TEST_BAD_OFX_BIN_DIR}")
        endforeach()
    else()
        set(KORALO_TEST_OFX_ROOT "${CMAKE_BINARY_DIR}/ofx_test")
        set(KORALO_TEST_BAD_OFX_ROOT "${CMAKE_BINARY_DIR}/ofx_bad_test")
        set_target_properties(koralo_fake_ofx PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${KORALO_TEST_OFX_ROOT}/FakeBoris.ofx.bundle/Contents/Linux-x86-64")
        set_target_properties(koralo_bad_ofx PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${KORALO_TEST_BAD_OFX_ROOT}/Crash.ofx.bundle/Contents/Linux-x86-64")
    endif()
    add_executable(koralo_native_tests tests_native/test_core.cpp)
    add_dependencies(koralo_native_tests koralo_fake_ofx koralo_bad_ofx koralo-ofx-scanner)
    target_compile_definitions(koralo_native_tests PRIVATE KORALO_TEST_OFX_ROOT="ofx_test" KORALO_TEST_BAD_OFX_ROOT="ofx_bad_test")
    target_link_libraries(koralo_native_tests PRIVATE koralo_core)
    add_test(NAME koralo_native_tests COMMAND koralo_native_tests)
    set_tests_properties(koralo_native_tests PROPERTIES WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
    add_executable(koralo_render_control_tests tests_native/test_render_control.cpp)
    target_link_libraries(koralo_render_control_tests PRIVATE koralo_core)
    koralo_enable_strict_warnings(koralo_render_control_tests)
    add_test(NAME koralo_render_control_tests COMMAND koralo_render_control_tests)
    set_tests_properties(koralo_render_control_tests PROPERTIES TIMEOUT 120)
    add_executable(koralo_frame_worker_tests tests_native/test_export_frame_workers.cpp)
    target_include_directories(koralo_frame_worker_tests PRIVATE src)
    koralo_enable_strict_warnings(koralo_frame_worker_tests)
    add_test(NAME koralo_frame_worker_tests COMMAND koralo_frame_worker_tests)
    set_tests_properties(koralo_frame_worker_tests PROPERTIES TIMEOUT 20)
    add_executable(koralo_export_parallel_tests tests_native/test_export_parallel.cpp)
    target_link_libraries(koralo_export_parallel_tests PRIVATE koralo_core)
    add_test(NAME koralo_export_parallel_tests COMMAND koralo_export_parallel_tests)
    set_tests_properties(koralo_export_parallel_tests PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)
    add_executable(koralo_effect_tests tests_native/test_native_effects.cpp)
    target_link_libraries(koralo_effect_tests PRIVATE koralo_core)
    add_test(NAME koralo_effect_tests COMMAND koralo_effect_tests)
    set_tests_properties(koralo_effect_tests PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
    add_executable(koralo_media_tests tests_native/test_media.cpp)
    target_link_libraries(koralo_media_tests PRIVATE koralo_core)
    add_test(NAME koralo_media_tests COMMAND koralo_media_tests)
    add_executable(koralo_animation_tests tests_native/test_animation.cpp)
    target_link_libraries(koralo_animation_tests PRIVATE koralo_core)
    add_test(NAME koralo_animation_tests COMMAND koralo_animation_tests)
    set_tests_properties(koralo_animation_tests PROPERTIES TIMEOUT 180)
    # Exercise Windows CreateProcessW inheritance, Unicode executable paths,
    # stdin/stdout pipes, timeouts and exit codes independently of FFmpeg.
    add_executable(koralo_platform_process_tests
        tests_native/test_platform_process.cpp src/platform/process.cpp)
    target_include_directories(koralo_platform_process_tests PRIVATE src)
    target_link_libraries(koralo_platform_process_tests PRIVATE Threads::Threads)
    koralo_enable_strict_warnings(koralo_platform_process_tests)
    if(MINGW)
        target_link_options(koralo_platform_process_tests PRIVATE -municode)
    endif()
    add_test(NAME koralo_platform_process_tests COMMAND koralo_platform_process_tests)
    set_tests_properties(koralo_platform_process_tests PROPERTIES TIMEOUT 45)
    add_executable(koralo_stream_pipe_tests tests_native/test_stream_pipe.cpp src/platform/process.cpp)
    target_include_directories(koralo_stream_pipe_tests PRIVATE src)
    target_link_libraries(koralo_stream_pipe_tests PRIVATE Threads::Threads)
    add_test(NAME koralo_stream_pipe_tests COMMAND koralo_stream_pipe_tests)
    set_tests_properties(koralo_stream_pipe_tests PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
    add_executable(koralo_audio_mix_tests tests_native/test_audio_mix_graph.cpp)
    # Pure audio graph regression is deliberately independent of heavy UI and
    # MediaEngine: it can run in sanitizer/CI jobs without recompiling those
    # translation units.
    target_sources(koralo_audio_mix_tests PRIVATE
        src/core/project.cpp src/core/vector_animation.cpp src/core/effects.cpp
        src/media/audio_mix_graph.cpp src/platform/process.cpp
        src/platform/app_paths.cpp)
    target_include_directories(koralo_audio_mix_tests PRIVATE src)
    target_link_libraries(koralo_audio_mix_tests PRIVATE Threads::Threads)
    koralo_enable_strict_warnings(koralo_audio_mix_tests)
    add_test(NAME koralo_audio_mix_tests COMMAND koralo_audio_mix_tests)
    set_tests_properties(koralo_audio_mix_tests PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 90)
    add_executable(koralo_audio_preview_timing_tests tests_native/test_audio_preview_timing.cpp)
    target_include_directories(koralo_audio_preview_timing_tests PRIVATE src)
    add_test(NAME koralo_audio_preview_timing_tests COMMAND koralo_audio_preview_timing_tests)
    add_executable(koralo_media_concurrency_tests tests_native/test_media_concurrency.cpp)
    target_link_libraries(koralo_media_concurrency_tests PRIVATE koralo_core)
    add_test(NAME koralo_media_concurrency_tests COMMAND koralo_media_concurrency_tests)
    set_tests_properties(koralo_media_concurrency_tests PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
    add_executable(koralo_render_fast_path_tests tests_native/test_render_fast_path.cpp src/media/render_fast_path.cpp)
    target_include_directories(koralo_render_fast_path_tests PRIVATE src)
    koralo_enable_strict_warnings(koralo_render_fast_path_tests)
    add_test(NAME koralo_render_fast_path_tests COMMAND koralo_render_fast_path_tests)
    set_tests_properties(koralo_render_fast_path_tests PROPERTIES TIMEOUT 20)
    add_executable(koralo_render_planner_tests tests_native/test_render_planner.cpp)
    target_link_libraries(koralo_render_planner_tests PRIVATE koralo_core)
    add_test(NAME koralo_render_planner_tests COMMAND koralo_render_planner_tests)
    set_tests_properties(koralo_render_planner_tests PROPERTIES TIMEOUT 30)
    add_executable(koralo_render_graph_tests tests_native/test_render_graph.cpp)
    target_link_libraries(koralo_render_graph_tests PRIVATE koralo_core)
    add_test(NAME koralo_render_graph_tests COMMAND koralo_render_graph_tests)
    set_tests_properties(koralo_render_graph_tests PROPERTIES TIMEOUT 30)
    add_executable(koralo_playback_controller_tests tests_native/test_playback_controller.cpp)
    target_link_libraries(koralo_playback_controller_tests PRIVATE koralo_core)
    add_test(NAME koralo_playback_controller_tests COMMAND koralo_playback_controller_tests)
    set_tests_properties(koralo_playback_controller_tests PROPERTIES TIMEOUT 30)
    add_executable(koralo_runtime_tests tests_native/test_runtime.cpp)
    target_link_libraries(koralo_runtime_tests PRIVATE koralo_core)
    if(MINGW)
        target_link_options(koralo_runtime_tests PRIVATE -municode)
    endif()
    add_test(NAME koralo_runtime_tests COMMAND koralo_runtime_tests)
    set_tests_properties(koralo_runtime_tests PROPERTIES TIMEOUT 120)
    add_executable(koralo_long_project_stability_tests tests_native/test_long_project_stability.cpp)
    target_link_libraries(koralo_long_project_stability_tests PRIVATE koralo_core)
    add_test(NAME koralo_long_project_stability_tests COMMAND koralo_long_project_stability_tests)
    set_tests_properties(koralo_long_project_stability_tests PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1800)
    set_property(TEST koralo_long_project_stability_tests APPEND PROPERTY ENVIRONMENT "KORALO_STABILITY_IMAGE=${CMAKE_SOURCE_DIR}/tests_native/data/stability_plush.jpg")
    set_tests_properties(koralo_media_tests PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)
    set_tests_properties(koralo_native_tests PROPERTIES TIMEOUT 60)
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/test-tmp" "${CMAKE_BINARY_DIR}/test-data" "${CMAKE_BINARY_DIR}/test-cache")
    file(COPY "${CMAKE_SOURCE_DIR}/locales" DESTINATION "${CMAKE_BINARY_DIR}")
    set(KORALO_TEST_ENV
        "TMPDIR=${CMAKE_BINARY_DIR}/test-tmp"
        "KORALO_DATA_DIR=${CMAKE_BINARY_DIR}/test-data"
        "KORALO_CACHE_DIR=${CMAKE_BINARY_DIR}/test-cache"
    )
    set_tests_properties(koralo_render_ui_tests koralo_export_improvements_tests koralo_multitrack_tests koralo_native_tests koralo_media_tests koralo_render_planner_tests koralo_render_graph_tests koralo_playback_controller_tests koralo_runtime_tests koralo_effect_tests koralo_animation_tests koralo_media_concurrency_tests PROPERTIES ENVIRONMENT "${KORALO_TEST_ENV}")
    set_tests_properties(koralo_long_project_stability_tests PROPERTIES ENVIRONMENT "${KORALO_TEST_ENV};KORALO_STABILITY_IMAGE=${CMAKE_SOURCE_DIR}/tests_native/data/stability_plush.jpg")
endif()


install(DIRECTORY bin/ DESTINATION bin OPTIONAL)
install(FILES assets/branding/koralo_icon.png assets/branding/koralo_splash.png DESTINATION assets/branding OPTIONAL)
install(DIRECTORY locales/ DESTINATION locales)
