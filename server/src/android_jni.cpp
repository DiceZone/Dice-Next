// Android JNI：在前台服务里跑与桌面相同的 Dice!Next 核心，面板仍是现有 WebUI。
#ifdef DICENEXT_ANDROID

#include <drogon/drogon.h>
#include <jni.h>
#include <ares.h>
#include <android/log.h>
#include <dlfcn.h>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "platform/android_host.h"

int main(int argc, char* argv[]);

namespace {

constexpr const char* kTag = "DiceNext";
std::atomic<bool> g_running{false};
JavaVM* g_jvm = nullptr;
jclass g_nativeClass = nullptr;
jmethodID g_installApk = nullptr;

void prependPath(const std::string& dir) {
    const char* old = std::getenv("PATH");
    std::string next = dir;
    if (old && *old) {
        next += ':';
        next += old;
    }
    setenv("PATH", next.c_str(), 1);
}

}  // namespace

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    g_jvm = vm;
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK || !env)
        return JNI_ERR;
    const jclass local = env->FindClass("zone/dice/dicenext/Native");
    if (!local) return JNI_ERR;
    g_nativeClass = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    if (!g_nativeClass) return JNI_ERR;
    g_installApk = env->GetStaticMethodID(
        g_nativeClass, "installApk", "(Ljava/lang/String;)Ljava/lang/String;");
    if (!g_installApk) return JNI_ERR;
    return JNI_VERSION_1_6;
}

bool androidInstallApk(const std::string& path, std::string& error) {
    if (!g_jvm || !g_nativeClass || !g_installApk) {
        error = "Android installer is not ready";
        return false;
    }
    JNIEnv* env = nullptr;
    bool attached = false;
    const jint envState = g_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (envState == JNI_EDETACHED) {
        if (g_jvm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) {
            error = "cannot attach to the Java VM";
            return false;
        }
        attached = true;
    } else if (envState != JNI_OK || !env) {
        error = "cannot obtain a JNI environment";
        return false;
    }
    jstring jpath = env->NewStringUTF(path.c_str());
    if (!jpath) {
        if (attached) g_jvm->DetachCurrentThread();
        error = "cannot pass the APK path to Java";
        return false;
    }
    auto jresult = static_cast<jstring>(
        env->CallStaticObjectMethod(g_nativeClass, g_installApk, jpath));
    env->DeleteLocalRef(jpath);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        if (attached) g_jvm->DetachCurrentThread();
        error = "package installer threw an exception";
        return false;
    }
    bool ok = true;
    if (jresult) {
        const char* text = env->GetStringUTFChars(jresult, nullptr);
        if (text && text[0] != '\0') {
            error = text;
            ok = false;
        }
        if (text) env->ReleaseStringUTFChars(jresult, text);
        env->DeleteLocalRef(jresult);
    }
    if (attached) g_jvm->DetachCurrentThread();
    return ok;
}

extern "C" JNIEXPORT void JNICALL
Java_zone_dice_dicenext_Native_initDns(JNIEnv* env, jclass, jobject connectivityManager) {
    JavaVM* jvm = nullptr;
    if (env->GetJavaVM(&jvm) != JNI_OK || !jvm || !connectivityManager) {
        __android_log_print(ANDROID_LOG_WARN, kTag, "c-ares Android DNS 初始化参数无效");
        return;
    }
    ares_library_init(ARES_LIB_INIT_ALL);
    ares_library_init_jvm(jvm);
    const int code = ares_library_init_android(connectivityManager);
    __android_log_print(code == ARES_SUCCESS ? ANDROID_LOG_INFO : ANDROID_LOG_WARN, kTag,
                        "c-ares Android DNS init=%d", code);
}

extern "C" JNIEXPORT jint JNICALL
Java_zone_dice_dicenext_Native_run(JNIEnv* env, jclass, jstring filesDir) {
    if (g_running.exchange(true)) return 1;
    const char* dir = env->GetStringUTFChars(filesDir, nullptr);
    const std::string root = dir ? dir : "";
    env->ReleaseStringUTFChars(filesDir, dir);
    if (root.empty() || ::chdir(root.c_str()) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, kTag, "无法进入数据目录 %s", root.c_str());
        g_running = false;
        return 1;
    }
    ::mkdir((root + "/tmp").c_str(), 0755);
    ::mkdir((root + "/bin").c_str(), 0755);
    ::mkdir((root + "/web").c_str(), 0755);
    ::mkdir((root + "/data").c_str(), 0755);
    ::mkdir((root + "/config").c_str(), 0755);
    setenv("HOME", root.c_str(), 1);
    setenv("TMPDIR", (root + "/tmp").c_str(), 1);
    setenv("TMP", (root + "/tmp").c_str(), 1);
    setenv("TEMP", (root + "/tmp").c_str(), 1);
    prependPath("/system/bin");
    prependPath(root + "/bin");
    const auto cert = root + "/cacert.pem";
    if (::access(cert.c_str(), R_OK) == 0) {
        setenv("SSL_CERT_FILE", cert.c_str(), 1);
        setenv("CURL_CA_BUNDLE", cert.c_str(), 1);
        __android_log_print(ANDROID_LOG_INFO, kTag, "TLS CA %s", cert.c_str());
    } else {
        __android_log_print(ANDROID_LOG_WARN, kTag, "缺少 cacert.pem，HTTPS 可能失败");
    }
    ::signal(SIGPIPE, SIG_IGN);

    char name[] = "dice-next";
    char* argv[] = {name, nullptr};
    const int code = main(1, argv);
    g_running = false;
    return code;
}

extern "C" JNIEXPORT void JNICALL
Java_zone_dice_dicenext_Native_stop(JNIEnv*, jclass) {
    try { drogon::app().quit(); } catch (...) {}
}

extern "C" JNIEXPORT jboolean JNICALL
Java_zone_dice_dicenext_Native_isRunning(JNIEnv*, jclass) {
    return g_running.load() ? JNI_TRUE : JNI_FALSE;
}

#endif
