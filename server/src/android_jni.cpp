// Android JNI：在前台服务里跑与桌面相同的 Dice!Next 核心，面板仍是现有 WebUI。
#ifdef DICENEXT_ANDROID

#include <drogon/drogon.h>
#include <jni.h>
#include <ares.h>
#include <android/log.h>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char* argv[]);

namespace {

constexpr const char* kTag = "DiceNext";
std::atomic<bool> g_running{false};

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
    setenv("DICENEXT_CONTAINER", "1", 1);
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
