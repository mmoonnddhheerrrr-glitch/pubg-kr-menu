// ─────────────────────────────────────────────────────────────────────────────
//  Main.cpp — All-in-one: (PUBG Mobile KR Edition)
//    NativeActivityHelper + TouchHook + AImGui + Stream-Hide menu
// ─────────────────────────────────────────────────────────────────────────────
#include <cstdint>
#include <cstring>
#include <thread>
#include <chrono>
#include <fstream>
#include <string>
#include <signal.h>
#include <unistd.h>
#include <dlfcn.h>

#include <jni.h>
#include <android/input.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include "ANwCreator.hpp"

#include <imgui.h>
#include <imgui_impl_android.h>
#include <imgui_impl_opengl3.h>
#include <dobby.h>

#define TAG "StreamHide"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// ═════════════════════════════════════════════════════════════════════════════
//  SECTION 1 — NativeActivity helper
// ═════════════════════════════════════════════════════════════════════════════
static JavaVM*         g_vm          = nullptr;
static ANativeActivity g_nativeActivity{};
static bool            g_initialized = false;

JNIEXPORT jint JNICALL
JNI_OnLoad(JavaVM* vm, void*)
{
    g_vm = vm;
    LOGD("JNI_OnLoad: vm=%p", vm);
    return JNI_VERSION_1_6;
}

static jobject GetCurrentActivity(JNIEnv* env)
{
    jclass atCls = env->FindClass("android/app/ActivityThread");
    if (!atCls) return nullptr;

    jmethodID curAT = env->GetStaticMethodID(
        atCls, "currentActivityThread", "()Landroid/app/ActivityThread;");
    if (!curAT) return nullptr;

    jobject at = env->CallStaticObjectMethod(atCls, curAT);
    if (!at) return nullptr;

    jfieldID mActF = env->GetFieldID(atCls, "mActivities", "Landroid/util/ArrayMap;");
    if (!mActF) return nullptr;

    jobject mAct = env->GetObjectField(at, mActF);
    if (!mAct) return nullptr;

    jclass amCls     = env->GetObjectClass(mAct);
    jmethodID valMid = env->GetMethodID(amCls, "values", "()Ljava/util/Collection;");
    if (!valMid) return nullptr;

    jobject values = env->CallObjectMethod(mAct, valMid);
    if (!values) return nullptr;

    jclass colCls   = env->GetObjectClass(values);
    jmethodID itMid = env->GetMethodID(colCls, "iterator", "()Ljava/util/Iterator;");
    if (!itMid) return nullptr;

    jobject iter  = env->CallObjectMethod(values, itMid);
    jclass itCls  = env->GetObjectClass(iter);
    jmethodID hasN = env->GetMethodID(itCls, "hasNext", "()Z");
    jmethodID nxt  = env->GetMethodID(itCls, "next",    "()Ljava/lang/Object;");

    while (env->CallBooleanMethod(iter, hasN))
    {
        jobject rec   = env->CallObjectMethod(iter, nxt);
        if (!rec) continue;
        jclass recCls = env->GetObjectClass(rec);
        jfieldID aF   = env->GetFieldID(recCls, "activity", "Landroid/app/Activity;");
        if (!aF) continue;
        jobject act   = env->GetObjectField(rec, aF);
        if (act) return act;
    }
    return nullptr;
}

extern "C"
ANativeActivity* GetANativeActivity()
{
    if (g_initialized) return &g_nativeActivity;
    if (!g_vm)         return nullptr;

    JNIEnv* env = nullptr;
    if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return nullptr;

    jobject act = GetCurrentActivity(env);
    if (!act) return nullptr;

    memset(&g_nativeActivity, 0, sizeof(g_nativeActivity));
    g_nativeActivity.vm    = g_vm;
    g_nativeActivity.env   = env;
    g_nativeActivity.clazz = env->NewGlobalRef(act);
    g_initialized = true;
    return &g_nativeActivity;
}

// ═════════════════════════════════════════════════════════════════════════════
//  SECTION 2 — Touch state + hook
// ═════════════════════════════════════════════════════════════════════════════
namespace {
    struct TouchState {
        float x    = 0.0f;
        float y    = 0.0f;
        bool  down = false;
    };
    TouchState g_Touch;
}

extern "C" void ImGui_SetTouchEvent(int action, float x, float y)
{
    int masked = action & AMOTION_EVENT_ACTION_MASK;
    switch (masked)
    {
        case AMOTION_EVENT_ACTION_DOWN:
        case AMOTION_EVENT_ACTION_POINTER_DOWN:
        case AMOTION_EVENT_ACTION_MOVE:
            g_Touch.down = true;
            g_Touch.x = x;
            g_Touch.y = y;
            break;
        case AMOTION_EVENT_ACTION_UP:
        case AMOTION_EVENT_ACTION_POINTER_UP:
        case AMOTION_EVENT_ACTION_CANCEL:
            g_Touch.down = false;
            g_Touch.x = x;
            g_Touch.y = y;
            break;
        default:
            break;
    }
}

static int (*orig_AInputQueue_getEvent)(AInputQueue*, AInputEvent**) = nullptr;

static int hooked_AInputQueue_getEvent(AInputQueue* queue, AInputEvent** outEvent)
{
    int res = orig_AInputQueue_getEvent(queue, outEvent);
    if (res >= 0 && outEvent && *outEvent)
    {
        AInputEvent* ev = *outEvent;
        if (AInputEvent_getType(ev) == AINPUT_EVENT_TYPE_MOTION)
        {
            ImGui_SetTouchEvent(
                AMotionEvent_getAction(ev),
                AMotionEvent_getX(ev, 0),
                AMotionEvent_getY(ev, 0));
        }
    }
    return res;
}

__attribute__((constructor))
static void InitTouchHook()
{
    void* sym = DobbySymbolResolver("libandroid.so", "AInputQueue_getEvent");
    if (!sym) { LOGE("AInputQueue_getEvent not found"); return; }
    dobby_enable_near_branch_trampoline();
    int ret = DobbyHook(sym,
                        (void*)hooked_AInputQueue_getEvent,
                        (void**)&orig_AInputQueue_getEvent);
    if (ret != 0) LOGE("DobbyHook failed: %d", ret);
    else          LOGD("Touch hook OK");
}

// ═════════════════════════════════════════════════════════════════════════════
//  SECTION 3 — AImGui  (inlined from AImGui.cpp)
// ═════════════════════════════════════════════════════════════════════════════
using JNIEnvHelper = ::android::anwcreator::detail::jni::JNIEnvironment;

namespace android
{
    struct AImGuiOptions {
        ANativeActivity* activity       = nullptr;
        bool             skipScreenshot = true;
    };

    class AImGui
    {
    public:
        explicit AImGui(const AImGuiOptions& opts) : m_opts(opts)
        {
            InitEnvironment();
        }
        ~AImGui() { UnInitEnvironment(); }

        void BeginFrame()
        {
            static bool sVisInit   = false;
            static int  sLastVis   = -2;

            if (m_opts.activity)
            {
                int vis = GetActivityWindowVisibility(m_opts.activity);
                if (vis != -1)
                {
                    if (!sVisInit)
                    {
                        sLastVis = vis;
                        sVisInit = true;
                    }
                    else if (vis != sLastVis)
                    {
                        if (sLastVis != 0 && vis == 0)
                        {
                            UnInitEnvironment();
                            InitEnvironment();
                        }
                        sLastVis = vis;
                    }
                }
            }

            if (!m_state) return;

            if (m_opts.activity && m_nativeWindow)
                ANwCreator::EnsureVisible(m_opts.activity, m_nativeWindow);

            if (m_imguiCtx)
            {
                ImGui::SetCurrentContext(m_imguiCtx);
                ImGuiIO& io    = ImGui::GetIO();
                io.DisplaySize = ImVec2((float)m_w, (float)m_h);
                io.MousePos    = ImVec2(g_Touch.x, g_Touch.y);
                io.MouseDown[0] = g_Touch.down;
            }

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplAndroid_NewFrame();
            ImGui::NewFrame();
        }

        void EndFrame()
        {
            if (!m_state) return;

            ImGui::Render();
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

            if (m_display != EGL_NO_DISPLAY && m_surface != EGL_NO_SURFACE)
                eglSwapBuffers(m_display, m_surface);
        }

        constexpr operator bool() const { return m_state; }

    private:
        static int GetActivityWindowVisibility(ANativeActivity* activity)
        {
            if (!activity || !activity->vm || !activity->clazz) return -1;
            JNIEnvHelper jniEnv(activity->vm);
            if (!jniEnv.IsValid()) return -1;
            JNIEnv* env = jniEnv;

            jobject actObj  = activity->clazz;
            jclass  actCls  = env->GetObjectClass(actObj);
            if (!actCls) return -1;

            jmethodID getWin = env->GetMethodID(actCls, "getWindow", "()Landroid/view/Window;");
            if (!getWin) return -1;
            jobject win = env->CallObjectMethod(actObj, getWin);
            if (!win) return -1;

            jclass winCls = env->GetObjectClass(win);
            jmethodID getDV = env->GetMethodID(winCls, "getDecorView", "()Landroid/view/View;");
            if (!getDV) { env->DeleteLocalRef(win); env->DeleteLocalRef(winCls); return -1; }

            jobject dv = env->CallObjectMethod(win, getDV);
            if (!dv) { env->DeleteLocalRef(winCls); env->DeleteLocalRef(win); return -1; }

            jclass dvCls = env->GetObjectClass(dv);
            jmethodID getVis = env->GetMethodID(dvCls, "getWindowVisibility", "()I");
            if (!getVis) {
                env->DeleteLocalRef(dvCls); env->DeleteLocalRef(dv);
                env->DeleteLocalRef(winCls); env->DeleteLocalRef(win);
                return -1;
            }
            jint vis = env->CallIntMethod(dv, getVis);
            env->DeleteLocalRef(dvCls); env->DeleteLocalRef(dv);
            env->DeleteLocalRef(winCls); env->DeleteLocalRef(win);
            return (int)vis;
        }

        bool InitEnvironment()
        {
            if (!m_opts.activity) return false;

            ANwCreator::CreateOptions co;
            co.name           = "AImGui";
            co.skipScreenshot = m_opts.skipScreenshot;

            m_nativeWindow = ANwCreator::Create(m_opts.activity, co);
            if (!m_nativeWindow) return false;

            ANativeWindow_acquire(m_nativeWindow);

            auto di = ANwCreator::GetDisplayInfo(m_opts.activity);
            m_w = di.width;
            m_h = di.height;

            m_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
            if (m_display == EGL_NO_DISPLAY) return false;
            if (!eglInitialize(m_display, nullptr, nullptr)) return false;

            const EGLint attribs[] = {
                EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
                EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                EGL_RED_SIZE,   8, EGL_GREEN_SIZE, 8,
                EGL_BLUE_SIZE,  8, EGL_ALPHA_SIZE, 8,
                EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
                EGL_SAMPLE_BUFFERS, 0,
                EGL_NONE
            };
            EGLint numCfg = 0; EGLConfig cfg{};
            if (!eglChooseConfig(m_display, attribs, &cfg, 1, &numCfg) || !numCfg)
                return false;

            EGLint fmt;
            eglGetConfigAttrib(m_display, cfg, EGL_NATIVE_VISUAL_ID, &fmt);
            ANativeWindow_setBuffersGeometry(m_nativeWindow, 0, 0, fmt);

            m_surface = eglCreateWindowSurface(m_display, cfg, m_nativeWindow, nullptr);
            if (m_surface == EGL_NO_SURFACE) return false;

            const EGLint ctxAttr[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
            m_context = eglCreateContext(m_display, cfg, EGL_NO_CONTEXT, ctxAttr);
            if (m_context == EGL_NO_CONTEXT) return false;

            if (!eglMakeCurrent(m_display, m_surface, m_surface, m_context))
                return false;

            IMGUI_CHECKVERSION();
            m_imguiCtx = ImGui::CreateContext();
            if (!m_imguiCtx) return false;

            ImGui::SetCurrentContext(m_imguiCtx);
            ImGui::GetIO().IniFilename = nullptr;
            ImGui::StyleColorsDark();
            ImGui::GetStyle().ScaleAllSizes(3.0f);

            ImFontConfig fc; fc.SizePixels = 22.0f;
            ImGui::GetIO().Fonts->AddFontDefault(&fc);

            if (!ImGui_ImplAndroid_Init(m_nativeWindow)) return false;
            if (!ImGui_ImplOpenGL3_Init("#version 300 es")) return false;

            glViewport(0, 0, m_w, m_h);
            glClearColor(0.f, 0.f, 0.f, 0.f);

            return (m_state = true);
        }

        void UnInitEnvironment()
        {
            m_state = false;
            if (m_imguiCtx)
            {
                ImGui::SetCurrentContext(m_imguiCtx);
                ImGui_ImplOpenGL3_Shutdown();
                ImGui_ImplAndroid_Shutdown();
                ImGui::DestroyContext(m_imguiCtx);
                m_imguiCtx = nullptr;
            }
            if (m_display != EGL_NO_DISPLAY)
            {
                eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
                if (m_context != EGL_NO_CONTEXT) { eglDestroyContext(m_display, m_context); m_context = EGL_NO_CONTEXT; }
                if (m_surface != EGL_NO_SURFACE) { eglDestroySurface(m_display, m_surface); m_surface = EGL_NO_SURFACE; }
                eglTerminate(m_display);
                m_display = EGL_NO_DISPLAY;
            }
            if (m_nativeWindow)
            {
                ANativeWindow_release(m_nativeWindow);
                ANwCreator::Destroy(m_opts.activity, m_nativeWindow);
                m_nativeWindow = nullptr;
            }
        }

        bool             m_state       = false;
        int32_t          m_w           = -1;
        int32_t          m_h           = -1;
        AImGuiOptions    m_opts;
        ANativeWindow*   m_nativeWindow = nullptr;
        EGLDisplay       m_display      = EGL_NO_DISPLAY;
        EGLSurface       m_surface      = EGL_NO_SURFACE;
        EGLContext       m_context      = EGL_NO_CONTEXT;
        ImGuiContext*    m_imguiCtx     = nullptr;
    };

} // namespace android

// ═════════════════════════════════════════════════════════════════════════════
//  SECTION 4 — Config (محدث للنسخة الكورية com.pubg.krmobile)
// ═════════════════════════════════════════════════════════════════════════════
static const char* CFG_PATH = "/data/data/com.pubg.krmobile/files/streamhide.cfg";

static bool LoadHideCfg()
{
    std::ifstream f(CFG_PATH);
    if (!f.is_open()) return false;
    std::string line;
    while (std::getline(f, line))
        if (line == "hide_on_record=true") return true;
    return false;
}

static void SaveHideCfg(bool value)
{
    std::ofstream f(CFG_PATH, std::ios::trunc);
    if (!f.is_open()) return;
    f << "hide_on_record=" << (value ? "true" : "false") << "\n";
    f.flush();
}

// ═════════════════════════════════════════════════════════════════════════════
//  SECTION 5 — Menu loop
// ═════════════════════════════════════════════════════════════════════════════
__attribute__((constructor))
static void MyStart()
{
    std::thread([] {

        ANativeActivity* activity = nullptr;
        while (!activity)
        {
            activity = GetANativeActivity();
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        bool hideOnRecord = LoadHideCfg();

        android::AImGui imgui({ activity, hideOnRecord });

        bool state = true;
        while (state)
        {
            imgui.BeginFrame();

            ImGuiIO& io  = ImGui::GetIO();
            ImVec2   wSz = ImVec2(580.0f, 220.0f);
            ImGui::SetNextWindowSize(wSz, ImGuiCond_Always);
            ImGui::SetNextWindowPos(
                ImVec2((io.DisplaySize.x - wSz.x) * 0.5f,
                       (io.DisplaySize.y - wSz.y) * 0.5f),
                ImGuiCond_Always);

            ImGui::Begin("Stream Hide (KR)", nullptr,
                ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoMove   |
                ImGuiWindowFlags_NoCollapse);

            ImGui::Spacing();

            if (hideOnRecord)
                ImGui::TextColored(ImVec4(0.2f,1.0f,0.3f,1.0f),
                    "[ON]  Menu HIDDEN on record / screenshot");
            else
                ImGui::TextColored(ImVec4(1.0f,0.4f,0.4f,1.0f),
                    "[OFF] Menu VISIBLE on record / screenshot");

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            bool prev = hideOnRecord;
            ImGui::Checkbox("Hide menu on record / screenshot", &hideOnRecord);

            if (hideOnRecord != prev)
            {
                SaveHideCfg(hideOnRecord); 
                kill(getpid(), SIGKILL);   
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextDisabled("Toggle -> cfg saved -> game killed -> applied on next launch");

            ImGui::End();

            imgui.EndFrame();
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }

    }).detach();
}
