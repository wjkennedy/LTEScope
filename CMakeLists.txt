cmake_minimum_required(VERSION 3.16)
project(LTEScope C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "" FORCE)
endif()

if(APPLE OR EMSCRIPTEN)
    set(CELLSCOPE_LTE_DEFAULT OFF)
else()
    set(CELLSCOPE_LTE_DEFAULT ON)
endif()

if(EMSCRIPTEN)
    # CMake records Emscripten's raw sysroot include dir as an implicit system
    # path, which forces libc++'s C headers ahead of the C++ wrappers and
    # breaks <cmath>/<complex>. Keep the compiler's own search order and let
    # em++ provide the headers naturally.
    list(FILTER CMAKE_C_IMPLICIT_INCLUDE_DIRECTORIES EXCLUDE REGEX "/sysroot/include$")
    list(FILTER CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES EXCLUDE REGEX "/sysroot/include$")
endif()

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
find_package(OpenGL REQUIRED)
find_package(PkgConfig)
include(FetchContent)

set(GLFW_FOUND FALSE)
if(EMSCRIPTEN)
    set(GLFW_FOUND TRUE)
    set(GLFW_INCLUDE_DIRS "")
    set(GLFW_LIBRARIES "")
    message(STATUS "Using Emscripten-provided GLFW/WebGL runtime")
else()
    find_package(glfw3 CONFIG QUIET)
    if(glfw3_FOUND)
        set(GLFW_FOUND TRUE)
    elseif(PkgConfig_FOUND)
        pkg_check_modules(GLFW QUIET glfw3)
        if(GLFW_FOUND)
            set(GLFW_FOUND TRUE)
        endif()
    endif()
    if(NOT GLFW_FOUND)
        find_path(GLFW_INCLUDE_DIRS NAMES GLFW/glfw3.h
            PATHS /opt/homebrew/opt/glfw /usr/local/opt/glfw /opt/local)
        find_library(GLFW_LIBRARIES NAMES glfw3 glfw
            PATHS /opt/homebrew/opt/glfw/lib /usr/local/opt/glfw/lib /opt/local/lib)
        if(GLFW_INCLUDE_DIRS AND GLFW_LIBRARIES)
            set(GLFW_FOUND TRUE)
        endif()
    endif()
    if(NOT GLFW_FOUND)
        message(STATUS "GLFW package not found; fetching glfw 3.4 from source archive")
        set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(glfw
            URL https://github.com/glfw/glfw/releases/download/3.4/glfw-3.4.zip
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
        FetchContent_MakeAvailable(glfw)
        set(GLFW_FOUND TRUE)
    endif()
endif()

# RTL-SDR
set(HAS_RTLSDR_LIB FALSE)
if(PkgConfig_FOUND)
    pkg_check_modules(RTLSDR QUIET librtlsdr rtl-sdr)
endif()
if(RTLSDR_FOUND)
    set(HAS_RTLSDR_LIB TRUE)
elseif(NOT EMSCRIPTEN)
    find_library(RTLSDR_LIBRARIES NAMES rtlsdr librtlsdr
        PATHS /opt/homebrew/opt/librtlsdr/lib /usr/local/opt/librtlsdr/lib /opt/local/lib)
    find_path(RTLSDR_INCLUDE_DIRS NAMES rtl-sdr.h
        PATHS /opt/homebrew/opt/librtlsdr/include /usr/local/opt/librtlsdr/include /opt/local/include)
    if(RTLSDR_LIBRARIES AND RTLSDR_INCLUDE_DIRS)
        set(HAS_RTLSDR_LIB TRUE)
    endif()
endif()
if(NOT HAS_RTLSDR_LIB)
    set(RTLSDR_LIBRARIES "")
    set(RTLSDR_INCLUDE_DIRS "")
    message(STATUS "RTL-SDR support disabled: librtlsdr not found")
else()
    message(STATUS "RTL-SDR support enabled: ${RTLSDR_LIBRARIES}")
endif()

# HackRF (native source). The MSYS2 libhackrf.pc has a hardcoded build-machine
# libusb include path, so locate the header/library directly instead of via
# pkg-config. hackrf.h itself doesn't pull in libusb, so no extra include is
# needed for our wrapper.
set(HAS_HACKRF_LIB FALSE)
find_path(HACKRF_INCLUDE_DIRS NAMES libhackrf/hackrf.h hackrf.h
          PATH_SUFFIXES libhackrf
          PATHS /opt/homebrew/opt/hackrf/include /usr/local/opt/hackrf/include /opt/local/include)
find_library(HACKRF_LIBRARIES NAMES hackrf libhackrf
              PATHS /opt/homebrew/opt/hackrf/lib /usr/local/opt/hackrf/lib /opt/local/lib)
if(HACKRF_INCLUDE_DIRS AND HACKRF_LIBRARIES)
    set(HAS_HACKRF_LIB TRUE)
endif()
if(HAS_HACKRF_LIB)
    message(STATUS "HackRF support enabled: ${HACKRF_LIBRARIES}")
else()
    set(HACKRF_INCLUDE_DIRS "")
    set(HACKRF_LIBRARIES "")
    message(STATUS "HackRF support disabled: libhackrf not found")
endif()

# Airspy (native source, optional).
# Vendored headers always available; library detection is optional.
set(AIRSPY_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/third_party/airspy)
find_library(AIRSPY_LIBRARIES NAMES airspy libairspy)
set(AIRSPY_FOUND FALSE)
if(AIRSPY_LIBRARIES)
    set(AIRSPY_FOUND TRUE)
    message(STATUS "Airspy link library found: ${AIRSPY_LIBRARIES}")
endif()

# LibreSDR (USRP B210 clone) via UHD. Linux-only: UHD loads our custom FPGA
# bitstream (libresdr_b210.bin) onto the device by passing fpga=<path> in the
# device args at make() time. Prefer pkg-config (uses the /usr/local UHD install
# and avoids the imported-target quirk in some UHDConfig.cmake versions), then
# fall back to the CMake config package.
set(LIBRESDR_FOUND FALSE)
if(NOT WIN32)
    set(UHD_FOUND FALSE)
    if(PkgConfig_FOUND)
        pkg_check_modules(UHD QUIET uhd)
    endif()
    if(NOT UHD_FOUND)
        find_package(UHD CONFIG QUIET)
    endif()
    if(UHD_FOUND)
        message(STATUS "UHD found: ${UHD_VERSION}")
        set(LIBRESDR_FOUND TRUE)
    endif()
endif()

if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/imgui/imgui.cpp")
    set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)
else()
    message(STATUS "ImGui sources not found; fetching upstream docking branch")
    FetchContent_Declare(imgui
        URL https://github.com/ocornut/imgui/archive/refs/heads/docking.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(imgui)
    set(IMGUI_DIR ${imgui_SOURCE_DIR})
endif()

if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/implot/implot.cpp")
    set(IMPLOT_DIR ${CMAKE_SOURCE_DIR}/third_party/implot)
else()
    message(STATUS "ImPlot sources not found; fetching upstream master branch")
    FetchContent_Declare(implot
        URL https://github.com/epezent/implot/archive/refs/heads/master.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(implot)
    set(IMPLOT_DIR ${implot_SOURCE_DIR})
endif()

# ---------------------------------------------------------------------------
# LTE stack (srsRAN2 + FALCON + LTESniffer), vendored under third_party/lte.
# Enabled by default; builds the decode libraries + the LTEScope LTE engine
# bridge (exposed via the cellscope_lte aggregate target).
# ---------------------------------------------------------------------------
option(HAS_LTE "Build the vendored LTE (srsRAN/FALCON/LTESniffer) stack" ${CELLSCOPE_LTE_DEFAULT})
if(HAS_LTE)
    add_subdirectory(third_party/lte)
endif()

# ---------------------------------------------------------------------------
# Dear ImGui + ImPlot (vendored)
# ---------------------------------------------------------------------------
if(EMSCRIPTEN)
    add_library(imgui_objs OBJECT
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
        ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
        ${IMPLOT_DIR}/implot.cpp
        ${IMPLOT_DIR}/implot_items.cpp
    )
    target_include_directories(imgui_objs PRIVATE
        ${IMGUI_DIR}
        ${IMGUI_DIR}/backends
        ${IMPLOT_DIR}
    )
    target_compile_definitions(imgui_objs PRIVATE GL_SILENCE_DEPRECATION)
    add_library(imgui INTERFACE)
    target_sources(imgui INTERFACE $<TARGET_OBJECTS:imgui_objs>)
    target_include_directories(imgui INTERFACE
        ${IMGUI_DIR}
        ${IMGUI_DIR}/backends
        ${IMPLOT_DIR}
    )
    # Keep the browser UI support objects small; aggressive optimization makes
    # the wasm link hit section-size limits in the current toolchain.
    set_source_files_properties(
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
        ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
        ${IMPLOT_DIR}/implot.cpp
        ${IMPLOT_DIR}/implot_items.cpp
        PROPERTIES COMPILE_OPTIONS "-O1")
else()
    add_library(imgui STATIC
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/imgui_demo.cpp
        ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
        ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
        ${IMPLOT_DIR}/implot.cpp
        ${IMPLOT_DIR}/implot_items.cpp
    )
    target_include_directories(imgui PUBLIC
        ${IMGUI_DIR}
        ${IMGUI_DIR}/backends
        ${IMPLOT_DIR}
    )
    # implot_items.cpp is extremely template-heavy: ~5 min to compile at -O2.
    # It is not performance-critical for us, so build it at -O1 (~1 min). The last
    # -O flag wins in GCC, so this overrides the RelWithDebInfo -O2.
    set_source_files_properties(${IMPLOT_DIR}/implot_items.cpp PROPERTIES COMPILE_OPTIONS "-O1")
    if(APPLE)
        target_compile_definitions(imgui PUBLIC GL_SILENCE_DEPRECATION)
    endif()
    target_link_libraries(imgui PUBLIC OpenGL::GL)
    if(TARGET glfw)
        target_link_libraries(imgui PUBLIC glfw)
    else()
        target_include_directories(imgui PUBLIC ${GLFW_INCLUDE_DIRS})
        target_link_libraries(imgui PUBLIC ${GLFW_LIBRARIES})
        target_link_directories(imgui PUBLIC ${GLFW_LIBRARY_DIRS})
    endif()
endif()

# ---------------------------------------------------------------------------
# LTEScope application
# ---------------------------------------------------------------------------
add_executable(LTEScope
    src/main.cpp
    src/signal/processing.cpp
    src/voice/voice_ops.cpp
    src/gui/gui_panels.cpp
    src/session/session.cpp
    src/state/config.cpp
    src/sdr/rtl_sdr_source.cpp
    src/sdr/esp_sdr_source.cpp
    src/sdr/hackrf_source.cpp
    src/sdr/wav_file_source.cpp
    src/sdr/iq_recorder.cpp
    src/dsp/jfft.cpp
    src/gui/waterfall.cpp
    src/decode/decoder.cpp
    src/decode/decoder_manager.cpp
    src/decode/band_plan.cpp
    src/decode/json_mini.cpp
    src/util/log.cpp
    src/decode/egc/egc_decoder.cpp
    src/i18n/i18n.cpp
    src/update/version_check.cpp
)
if(NOT EMSCRIPTEN)
    target_sources(LTEScope PRIVATE src/sdr/sdrpp_server_source.cpp)
endif()
target_include_directories(LTEScope PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${RTLSDR_INCLUDE_DIRS}
    ${HACKRF_INCLUDE_DIRS}
)
if(WIN32 AND NOT EMSCRIPTEN)
    target_compile_definitions(LTEScope PRIVATE _USE_MATH_DEFINES)
endif()
if(APPLE AND NOT EMSCRIPTEN)
    target_compile_definitions(LTEScope PRIVATE GL_SILENCE_DEPRECATION)
    target_link_libraries(LTEScope PRIVATE
        /opt/homebrew/opt/llvm/lib/c++/libc++.dylib
        /opt/homebrew/opt/llvm/lib/c++/libc++abi.dylib)
endif()
if(HAS_RTLSDR_LIB)
    target_compile_definitions(LTEScope PRIVATE HAS_RTLSDR_LIB=1)
    target_link_directories(LTEScope PRIVATE ${RTLSDR_LIBRARY_DIRS})
    target_link_libraries(LTEScope PRIVATE ${RTLSDR_LIBRARIES})
endif()
if(HAS_HACKRF_LIB)
    target_compile_definitions(LTEScope PRIVATE HAS_HACKRF_LIB=1)
endif()
if(AIRSPY_FOUND)
    target_sources(LTEScope PRIVATE src/sdr/airspy_source.cpp)
    target_compile_definitions(LTEScope PRIVATE HAS_AIRSPY=1)
    target_include_directories(LTEScope PRIVATE ${AIRSPY_INCLUDE_DIRS})
endif()
if(LIBRESDR_FOUND)
    target_sources(LTEScope PRIVATE src/sdr/libresdr_source.cpp)
    target_compile_definitions(LTEScope PRIVATE HAS_LIBRESDR=1)
    target_include_directories(LTEScope PRIVATE ${UHD_INCLUDE_DIRS})
    target_link_directories(LTEScope PRIVATE ${UHD_LIBRARY_DIRS})
endif()
if(NOT EMSCRIPTEN)
    find_library(ZSTD_LIBRARIES NAMES zstd libzstd
        PATHS /opt/homebrew/opt/zstd/lib /usr/local/opt/zstd/lib /opt/local/lib
        REQUIRED)
    find_path(ZSTD_INCLUDE_DIRS NAMES zstd.h
        PATHS /opt/homebrew/opt/zstd/include /usr/local/opt/zstd/include /opt/local/include
        REQUIRED)
    target_include_directories(LTEScope PRIVATE ${ZSTD_INCLUDE_DIRS})
    target_link_libraries(LTEScope PRIVATE imgui ${ZSTD_LIBRARIES})
else()
    target_link_libraries(LTEScope PRIVATE imgui)
endif()
if(EMSCRIPTEN)
    target_link_options(LTEScope PRIVATE
        "-sUSE_GLFW=3"
        "-sUSE_WEBGL2=1"
        "-sFULL_ES3=1"
        "-sALLOW_MEMORY_GROWTH=1"
        "-sASSERTIONS=1"
        "-sEXIT_RUNTIME=0"
        "-sEXPORTED_FUNCTIONS=['_main','_cellscope_web_rtl_prepare_result','_cellscope_web_rtl_sample_block','_cellscope_web_esp_prepare_result','_cellscope_web_esp_sample_block']"
        "--pre-js=${CMAKE_SOURCE_DIR}/src/sdr/webrtlsdr_bridge.js"
        "--pre-js=${CMAKE_SOURCE_DIR}/src/sdr/webesp_bridge.js")
    configure_file(${CMAKE_SOURCE_DIR}/web/ltescope.html.in
        ${CMAKE_BINARY_DIR}/LTEScope.html
        @ONLY)
    configure_file(${CMAKE_SOURCE_DIR}/web/flash.html
        ${CMAKE_BINARY_DIR}/flash.html COPYONLY)
    configure_file(${CMAKE_SOURCE_DIR}/THIRD_PARTY_NOTICES.md
        ${CMAKE_BINARY_DIR}/THIRD_PARTY_NOTICES.md COPYONLY)
    file(COPY ${CMAKE_SOURCE_DIR}/web/firmware
        DESTINATION ${CMAKE_BINARY_DIR})
endif()
if(HAS_RTLSDR_LIB)
    target_link_libraries(LTEScope PRIVATE ${RTLSDR_LIBRARIES})
endif()
if(HAS_HACKRF_LIB)
    target_include_directories(LTEScope PRIVATE ${HACKRF_INCLUDE_DIRS})
    target_link_libraries(LTEScope PRIVATE ${HACKRF_LIBRARIES})
endif()
if(AIRSPY_FOUND)
    target_link_libraries(LTEScope PRIVATE ${AIRSPY_LIBRARIES})
endif()
if(LIBRESDR_FOUND)
    target_link_libraries(LTEScope PRIVATE ${UHD_LIBRARIES})
endif()
if(HAS_LTE)
    target_link_libraries(LTEScope PRIVATE cellscope_lte)
    target_compile_definitions(LTEScope PRIVATE HAS_LTE=1)
    target_sources(LTEScope PRIVATE src/lte/lte_diag.cpp)
    if(WIN32)
        target_link_libraries(LTEScope PRIVATE dbghelp psapi)
    endif()
endif()

if(WIN32)
    target_link_libraries(LTEScope PRIVATE opengl32 gdi32 comdlg32 ole32 winmm ws2_32 winhttp shell32)

    # Build as a GUI-subsystem app so no console window opens alongside the GUI.
    # MinGW provides a WinMain shim, so the existing int main() still works.
    set_target_properties(LTEScope PROPERTIES WIN32_EXECUTABLE TRUE)

    # Embed the application icon (icon.ico in the project root). Windows and
    # GLFW both pick up the first icon resource for the exe and window.
    if(EXISTS "${CMAKE_SOURCE_DIR}/icon.ico")
        enable_language(RC)
        set(FRAMESYNC_ICON "${CMAKE_SOURCE_DIR}/icon.ico")
        configure_file("${CMAKE_SOURCE_DIR}/app.rc.in" "${CMAKE_BINARY_DIR}/app.rc" @ONLY)
        target_sources(LTEScope PRIVATE "${CMAKE_BINARY_DIR}/app.rc")
    endif()

    # Copy MinGW runtime DLLs next to the executable so it runs standalone
    # (PyCharm, double-click) without the MSYS2 bin dir on PATH.
    get_filename_component(MINGW_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(FRAMESYNC_RUNTIME_DLLS
        libgcc_s_seh-1.dll
        libwinpthread-1.dll
        libstdc++-6.dll
        glfw3.dll
        librtlsdr.dll
        libhackrf.dll
        libusb-1.0.dll
        libzstd.dll
        zlib1.dll
    )
    if(AIRSPY_FOUND)
        list(APPEND FRAMESYNC_RUNTIME_DLLS libairspy.dll)
    endif()
    if(HAS_LTE)
        list(APPEND FRAMESYNC_RUNTIME_DLLS libfftw3f-3.dll)
    endif()
    foreach(dll ${FRAMESYNC_RUNTIME_DLLS})
        if(EXISTS "${MINGW_BIN}/${dll}")
            add_custom_command(TARGET LTEScope POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${MINGW_BIN}/${dll}" "$<TARGET_FILE_DIR:LTEScope>"
                COMMENT "Copying ${dll}")
        else()
            message(WARNING "Runtime DLL not found: ${MINGW_BIN}/${dll}")
        endif()
    endforeach()
endif()
