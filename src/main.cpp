// LTEScope - Inmarsat decoder
// Phase 1: RTL-SDR -> IQ ring -> FFT -> spectrum + scrolling waterfall.

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"

#include <GLFW/glfw3.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include <cmath>
#include <complex>

#include "core/app.h"
#include "core/default_layout.h"
#include "core/main_funcs.h"
#include "i18n/i18n.h"
#include "util/log.h"
#include "version.h"
#include "gui/waterfall.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>

bool openWavDialog(char* out, int outLen)
{
    char file[1024] = "";
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = "WAV / IQ files\0*.wav\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = sizeof(file);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrInitialDir = "D:\\";
    if (GetOpenFileNameA(&ofn))
    {
        std::strncpy(out, file, outLen - 1);
        out[outLen - 1] = 0;
        return true;
    }
    return false;
}
#else
bool openWavDialog(char*, int) { return false; }
#endif

#if !defined(_WIN32)
#ifndef __EMSCRIPTEN__
static void capture_stderr_to_file()
{
    if (std::freopen("cellscope_raw.log", "a", stderr)) {
        std::setvbuf(stderr, nullptr, _IONBF, 0);
    }
}
#else
static void capture_stderr_to_file() {}
#endif
#endif

static void glfw_error_callback(int error, const char* description)
{
    std::fprintf(stderr, "GLFW error %d: %s\n", error, description);
}

namespace {
App*        g_app = nullptr;
GLFWwindow* g_window = nullptr;
const ImVec4 g_clear_color = ImVec4(0.06f, 0.07f, 0.09f, 1.0f);

static void render_frame()
{
    App& app = *g_app;
    GLFWwindow* window = g_window;

    // Finalize an async source start (LibreSDR/UHD) once the worker thread
    // has opened the device. Done on the GUI thread so startActive() only
    // touches shared state (rings, decoders, active source) from here.
    if (app.startReady.exchange(false))
    {
        if (app.startThread.joinable())
            app.startThread.join();
        if (app.startErr.empty())
            startActive(app); // device already prepared -> returns quickly
        else
            app.status = "Error: " + app.startErr;
        app.starting.store(false);
    }

    // Apply the built-in default dock layout (fresh run / version bump /
    // Reset Layout). Done between frames as ImGui prefers.
    if (app.forceDefaultLayout)
    {
        ImGui::LoadIniSettingsFromMemory(kDefaultLayoutIni);
        app.forceDefaultLayout = false;
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    drawDockHost(app);

    if (app.active->running())
        processFft(app.viewA, app, app.active->centerFreq(), app.active->sampleRate());
    if (app.dualMode && app.sdrB.running())
        processFft(app.viewB, app, app.sdrB.centerFreq(), app.sdrB.sampleRate());

    if (app.active->running())
        updateRateChange(app);

    // Refresh saved decoder list for persistent restart (non-8400 only)
    if (app.saveDecoders && app.active->running())
    {
        app.savedDecoders.clear();
        for (auto& st : app.decoders.status())
            if (st.baud != 8400)
                app.savedDecoders.push_back({st.freqMHz, st.baud});
        app.savedDecodersB.clear();
        for (auto& st : app.decodersB.status())
            if (st.baud != 8400)
                app.savedDecodersB.push_back({st.freqMHz, st.baud});
    }

    drawControls(app);
    {
        std::string t = std::string(_L("Spectrum")) + "###Spectrum";
        drawSpectrum(app, app.viewA, app.decoders, t.c_str(), true, false);
    }
    {
        std::string t = std::string(_L("Waterfall")) + "###Waterfall";
        drawWaterfall(app, app.viewA, t.c_str());
    }
    if (app.dualMode)
    {
        {
            std::string t = std::string(_L("Spectrum")) + "###Spectrum (B)";
            drawSpectrum(app, app.viewB, app.decodersB, t.c_str(), true, true);
        }
        {
            std::string t = std::string(_L("Waterfall")) + "###Waterfall (B)";
            drawWaterfall(app, app.viewB, t.c_str());
        }
    }
    drawAbout(app);
    drawLte(app);
#ifdef HAS_LTE
    drawLteUes(app);
    drawLteTraffic(app);
    drawLteCalls(app);
    drawRawDebug(app);
#endif

    int display_w, display_h;
    glfwGetFramebufferSize(window, &display_w, &display_h);
    glViewport(0, 0, display_w, display_h);
    glClearColor(g_clear_color.x, g_clear_color.y, g_clear_color.z, g_clear_color.w);
    glClear(GL_COLOR_BUFFER_BIT);

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    glfwSwapBuffers(window);
}
} // namespace

int main(int, char**)
{
#if !defined(_WIN32)
    capture_stderr_to_file();
#endif
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit())
        return 1;

#if defined(__EMSCRIPTEN__)
    const char* glsl_version = nullptr;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#elif defined(__APPLE__)
    const char* glsl_version = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#else
    const char* glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif

    GLFWwindow* window = glfwCreateWindow(1400, 900, "LTEScope", nullptr, nullptr);
    if (!window)
    {
        glfwTerminate();
        return 1;
    }
    g_window = window;
    glfwMakeContextCurrent(window);
#ifndef __EMSCRIPTEN__
    glfwSwapInterval(1);
#endif

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    App app;

    // Load persisted settings BEFORE building the font atlas so fontSize
    // from cellscope.ini is available for the font loader.
    cfgRegisterHandler(app);
    io.IniFilename = "cellscope.ini";
    ImGui::LoadIniSettingsFromDisk(io.IniFilename);

    // English-only strings pass through _L() as identity.
    i18nInit();

    // Font: use Roboto-Medium (vendored TTF, scales cleanly at any size).
    // Clamp to sensible range and scale the style sizes proportionally.
    if (app.fontSize < 8)  app.fontSize = 8;
    if (app.fontSize > 24) app.fontSize = 24;
    {
        const char* fontPaths[] = {
            "third_party/imgui/misc/fonts/Roboto-Medium.ttf",
            "../third_party/imgui/misc/fonts/Roboto-Medium.ttf",
        };
        bool loaded = false;
        for (auto& fp : fontPaths)
        {
            FILE* f = std::fopen(fp, "rb");
            if (f) { std::fclose(f); loaded = (io.Fonts->AddFontFromFileTTF(fp, (float)app.fontSize) != nullptr); break; }
        }
        if (!loaded)
            io.Fonts->AddFontDefault(); // fallback to built-in ProggyClean
    }
    ImGui::GetStyle().ScaleAllSizes((float)app.fontSize / 13.0f);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    // If the saved layout predates the current default (or none was saved),
    // rebuild the canonical default layout so users get the intended arrangement.
    if (app.layoutVersion != kLayoutVersion)
    {
        app.forceDefaultLayout = true;
        app.layoutVersion = kLayoutVersion;
    }

    buildWindow(app.viewA, kFftSizes[app.fftSizeIdx], app.dbMin);
    buildWindow(app.viewB, kFftSizes[app.fftSizeIdx], app.dbMin);
    app.devices = app.sdr.listDevices();
    app.espDevices = app.espSdr.listDevices();
#ifndef __EMSCRIPTEN__
    app.verCheck.start("ltescope", CELLSCOPE_VERSION);
#endif
    scanBandPlans(app.bandPlanDir, app.bandPlanNames, app.bandPlanPaths);
    if (app.bandPlanIdx >= 0 && app.bandPlanIdx < (int)app.bandPlanPaths.size())
        app.bandPlanLoaded = loadBandPlan(app.bandPlanPaths[app.bandPlanIdx]);
    if (app.bandPlanIdxB >= 0 && app.bandPlanIdxB < (int)app.bandPlanPaths.size())
        app.bandPlanLoadedB = loadBandPlan(app.bandPlanPaths[app.bandPlanIdxB]);

    g_app = &app;
#ifdef __EMSCRIPTEN__
    emscripten_set_main_loop(render_frame, 0, true);
#else
    while (!glfwWindowShouldClose(window))
    {
        glfwPollEvents();
        render_frame();
    }
#endif

    // Persist settings + dock layout to cellscope.ini before shutting down.
    ImGui::SaveIniSettingsToDisk(io.IniFilename);

    if (app.startThread.joinable())
        app.startThread.join();

    app.decoders.stop();
    app.decodersB.stop();
    app.sdr.stop();
    app.sdrB.stop();
    app.wav.stop();
#ifndef __EMSCRIPTEN__
    app.server.stop();
#endif
    app.hack.stop();
#ifdef HAS_LTE
    app.lteEngine.stop();
#endif

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
