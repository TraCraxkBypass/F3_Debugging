#include <jni.h>
#include <android/input.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>
#include <string>
#include <vector>

#include "pl/Hook.h"
#include "pl/Gloss.h"
#include "ImGui/imgui.h"
#include "ImGui/backends/imgui_impl_opengl3.h"
#include "ImGui/backends/imgui_impl_android.h"

// Global Variables
static bool g_Initialized = false;
static int g_Width = 0, g_Height = 0;
static EGLContext g_TargetContext = EGL_NO_CONTEXT;
static EGLSurface g_TargetSurface = EGL_NO_SURFACE;

// Function Pointers
static EGLBoolean (*orig_eglSwapBuffers)(EGLDisplay, EGLSurface) = nullptr;
static void (*orig_Input)(void*, void*, void*) = nullptr;

// Hook Input - Dibuat sangat ringan untuk elak lag/crash
static void hook_Input(void* thiz, void* a1, void* a2) {
    if (orig_Input) orig_Input(thiz, a1, a2);
    if (g_Initialized && thiz) {
        ImGui_ImplAndroid_HandleInputEvent((AInputEvent*)thiz);
    }
}

// State Management - Kunci utama elak crash
struct GLState {
    GLint last_program, last_texture, last_active_texture, last_array_buffer, last_element_array_buffer, last_vertex_array;
    GLint last_viewport[4], last_scissor_box[4];
    GLboolean last_enable_blend, last_enable_depth_test, last_enable_scissor_test, last_enable_cull_face;
};

void SaveState(GLState& s) {
    glGetIntegerv(GL_CURRENT_PROGRAM, &s.last_program);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &s.last_texture);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &s.last_active_texture);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s.last_array_buffer);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &s.last_element_array_buffer);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s.last_vertex_array);
    glGetIntegerv(GL_VIEWPORT, s.last_viewport);
    glGetIntegerv(GL_SCISSOR_BOX, s.last_scissor_box);
    s.last_enable_blend = glIsEnabled(GL_BLEND);
    s.last_enable_depth_test = glIsEnabled(GL_DEPTH_TEST);
    s.last_enable_scissor_test = glIsEnabled(GL_SCISSOR_TEST);
    s.last_enable_cull_face = glIsEnabled(GL_CULL_FACE);
}

void RestoreState(const GLState& s) {
    glUseProgram(s.last_program);
    glActiveTexture(s.last_active_texture);
    glBindTexture(GL_TEXTURE_2D, s.last_texture);
    glBindBuffer(GL_ARRAY_BUFFER, s.last_array_buffer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s.last_element_array_buffer);
    glBindVertexArray(s.last_vertex_array);
    glViewport(s.last_viewport[0], s.last_viewport[1], s.last_viewport[2], s.last_viewport[3]);
    glScissor(s.last_scissor_box[0], s.last_scissor_box[1], s.last_scissor_box[2], s.last_scissor_box[3]);
    if (s.last_enable_blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (s.last_enable_depth_test) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (s.last_enable_scissor_test) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (s.last_enable_cull_face) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
}

static void DrawMinecraftF3() {
    ImGuiIO& io = ImGui::GetIO();
    
    // Konfigurasi Window (Transparent & Overlay)
    ImGui::SetNextWindowPos(ImVec2(10, 10));
    ImGui::Begin("LeftF3", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs);

    // Baris 1: Info Versi & FPS
    ImGui::Text("Minecraft 1.20.1 (Modded/Android)");
    ImGui::Text("%d fps (%d ms) T: inf", (int)io.Framerate, (int)(1000.0f / io.Framerate));

    // Baris 2: Render Info
    const char* renderer = (const char*)glGetString(GL_RENDERER);
    ImGui::Text("Integrated GPU: %s", renderer ? renderer : "Unknown");
    
    // Baris 3: Koordinat (Contoh dinamik)
    static float fakeX = 142.5f, fakeY = 64.0f, fakeZ = -210.3f;
    ImGui::Text("XYZ: %.3f / %.5f / %.3f", fakeX, fakeY, fakeZ);
    ImGui::Text("Block: %d %d %d", (int)fakeX, (int)fakeY, (int)fakeZ);
    ImGui::Text("Chunk: %d %d %d in %d %d %d", (int)fakeX%16, (int)fakeY%16, (int)fakeZ%16, (int)fakeX/16, (int)fakeY/16, (int)fakeZ/16);
    
    // Baris 4: Facing
    ImGui::Text("Facing: north (Towards negative Z) (0.0 / 0.0)");
    
    ImGui::End();

    // Menu Kanan (Info Memori & Sistem)
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 10, 10), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::Begin("RightF3", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs);
    
    const char* vendor = (const char*)glGetString(GL_VENDOR);
    ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "Java: 17.0.1 64bit"); // Simulasi info java
    ImGui::Text("Mem: %d%% %d/%dMB", 45, 1200, 4096);
    ImGui::Text("CPU: %s", vendor ? vendor : "ARMv8");
    ImGui::Text("Display: %dx%d (%s)", g_Width, g_Height, (const char*)glGetString(GL_VERSION));
    
    ImGui::End();
}

static void InitImGui() {
    if (g_Initialized) return;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    
    // Scaling biar nampak macam F3 asli
    float scale = (float)g_Height / 1080.0f;
    if (scale < 1.0f) scale = 1.0f;
    io.FontGlobalScale = scale * 1.2f;

    ImGui_ImplAndroid_Init();
    ImGui_ImplOpenGL3_Init("#version 300 es");
    g_Initialized = true;
}

static EGLBoolean hook_eglSwapBuffers(EGLDisplay dpy, EGLSurface surf) {
    if (!orig_eglSwapBuffers) return EGL_FALSE;

    eglQuerySurface(dpy, surf, EGL_WIDTH, &g_Width);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &g_Height);

    if (g_Width > 100 && g_Height > 100) {
        InitImGui();

        GLState state;
        SaveState(state); // Ambil semua state game sekarang

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplAndroid_NewFrame(g_Width, g_Height);
        ImGui::NewFrame();

        DrawMinecraftF3();

        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        RestoreState(state); // Pulangkan state game supaya game tak crash
    }

    return orig_eglSwapBuffers(dpy, surf);
}

static void* MainThread(void*) {
    sleep(6); // Tunggu lebih lama sikit bagi game settle load lib
    
    GlossInit(true);
    
    void* hEGL = GlossOpen("libEGL.so");
    if (hEGL) {
        void* swap = (void*)GlossSymbol(hEGL, "eglSwapBuffers", nullptr);
        if (swap) GlossHook(swap, (void*)hook_eglSwapBuffers, (void**)&orig_eglSwapBuffers);
    }

    void* hInput = GlossOpen("libinput.so");
    if (hInput) {
        void* sym = (void*)GlossSymbol(hInput, "_ZN7android13InputConsumer21initializeMotionEventEPNS_11MotionEventEPKNS_12InputMessageE", nullptr);
        if (sym) GlossHook(sym, (void*)hook_Input, (void**)&orig_Input);
    }

    return nullptr;
}

__attribute__((constructor))
void Start() {
    pthread_t t;
    pthread_create(&t, nullptr, MainThread, nullptr);
}
