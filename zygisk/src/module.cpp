#include <android/log.h>
#include <fcntl.h>
#include <jni.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "lsplant.hpp"
#include "shadowhook.h"
#include "zygisk.hpp"
#include "payload_dex.h"

using zygisk::Api;
using zygisk::AppSpecializeArgs;

#define LOG_TAG "XGAirDrop"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Quick Share's external-provider check runs in the main GMS process only.
// Never touch gms.unstable (DroidGuard / Play Integrity) or gms.persistent.
static bool is_target(const char *process) {
    return process != nullptr && strcmp(process, "com.google.android.gms") == 0;
}

// -----------------------------------------------------------------------------
// JNI Native hook implementation for Java payload
// -----------------------------------------------------------------------------
static jboolean JNICALL nativeHook(JNIEnv *env, jclass /* clazz */, jobject target,
                                   jobject callback, jobject hookerObj) {
    if (!target || !callback || !hookerObj) {
        LOGE("nativeHook: invalid arguments (target=%p, callback=%p, hookerObj=%p)",
             target, callback, hookerObj);
        return JNI_FALSE;
    }
    jobject backup = lsplant::Hook(env, target, hookerObj, callback);
    if (backup != nullptr) {
        LOGI("[✓] lsplant::Hook succeeded");
        return JNI_TRUE;
    } else {
        LOGE("[X] lsplant::Hook failed");
        return JNI_FALSE;
    }
}

// Same as nativeHook, but returns the backup Method so the callback can call the original.
static jobject JNICALL nativeHookBackup(JNIEnv *env, jclass /* clazz */, jobject target,
                                        jobject callback, jobject hookerObj) {
    if (!target || !callback || !hookerObj) return nullptr;
    jobject backup = lsplant::Hook(env, target, hookerObj, callback);
    if (backup == nullptr) LOGE("[X] lsplant::Hook (with backup) failed");
    return backup;
}

// -----------------------------------------------------------------------------
// LSPlant hooker callbacks powered by ShadowHook
// -----------------------------------------------------------------------------
static std::mutex g_hook_mutex;
static std::unordered_map<void *, void *> g_hook_map;  // target -> stub

static void *inline_hooker(void *target, void *hooker) {
    void *orig = nullptr;
    void *stub = shadowhook_hook_func_addr(target, hooker, &orig);
    if (stub != nullptr) {
        std::lock_guard<std::mutex> lock(g_hook_mutex);
        g_hook_map[target] = stub;
        return orig;
    }
    LOGE("shadowhook_hook_func_addr failed for target %p: errno %d (%s)",
         target, shadowhook_get_errno(), shadowhook_to_errmsg(shadowhook_get_errno()));
    return nullptr;
}

static bool inline_unhooker(void *target) {
    void *stub = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_hook_mutex);
        auto it = g_hook_map.find(target);
        if (it != g_hook_map.end()) {
            stub = it->second;
            g_hook_map.erase(it);
        }
    }
    if (stub != nullptr) {
        int ret = shadowhook_unhook(stub);
        return ret == 0;
    }
    LOGW("inline_unhooker: stub not found for target %p", target);
    return false;
}

static void *g_libart_handle = nullptr;
static void *art_symbol_resolver(std::string_view symbol_name) {
    if (!g_libart_handle) {
        g_libart_handle = shadowhook_dlopen("libart.so");
        if (!g_libart_handle) {
            LOGE("shadowhook_dlopen(libart.so) failed");
            return nullptr;
        }
    }
    std::string name(symbol_name);
    void *sym = shadowhook_dlsym_symtab(g_libart_handle, name.c_str());
    if (!sym) {
        sym = shadowhook_dlsym(g_libart_handle, name.c_str());
    }
    return sym;
}

// Global persistent storage for payload.dex memory backing direct ByteBuffer
static std::vector<uint8_t> g_payload_dex;

// -----------------------------------------------------------------------------
// Zygisk Module Class
// -----------------------------------------------------------------------------
class XgaModule : public zygisk::ModuleBase {
public:
    void onLoad(Api *api, JNIEnv *env) override {
        this->api = api;
        this->env = env;
    }

    void preAppSpecialize(AppSpecializeArgs *args) override {
        const char *process = nullptr;
        if (args->nice_name) {
            process = env->GetStringUTFChars(args->nice_name, nullptr);
        }
        is_target_process = is_target(process) && hook_allowed();
        process_name = (process != nullptr) ? process : "";
        LOGI("%s %s", is_target_process ? "+ target" : "- skip",
             process ? process : "(null)");
        if (process) {
            env->ReleaseStringUTFChars(args->nice_name, process);
        }

        if (!is_target_process) {
            // Unload immediately from non-target processes (e.g. gms.unstable)
            api->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
        }
    }

    // Two LSPlant engines in the GMS process make it hang, so stand down whenever GMS
    // Tweaks is active; it forces the same flags. The root companion can see /data/adb.
    bool hook_allowed() {
        int fd = api->connectCompanion();
        if (fd < 0) return false;
        uint8_t allowed = 0;
        if (read(fd, &allowed, 1) != 1) allowed = 0;
        close(fd);
        if (!allowed) LOGI("GMS Tweaks is active; leaving the GMS flags to it");
        return allowed == 1;
    }

    void postAppSpecialize([[maybe_unused]] const AppSpecializeArgs *args) override {
        if (!is_target_process) {
            return;
        }
        LOGI("[*] postAppSpecialize entered in target process");

        // 1. shadowhook_init
        int sh_init = shadowhook_init(SHADOWHOOK_MODE_SHARED, false);
        if (sh_init != 0) {
            LOGE("shadowhook_init failed: errno %d (%s)",
                 shadowhook_get_errno(), shadowhook_to_errmsg(shadowhook_get_errno()));
            return;
        }
        LOGI("[✓] shadowhook_init initialized");

        // 2. lsplant::Init
        lsplant::InitInfo init_info{
            .inline_hooker = inline_hooker,
            .inline_unhooker = inline_unhooker,
            .art_symbol_resolver = art_symbol_resolver,
            .art_symbol_prefix_resolver = nullptr
        };
        if (!lsplant::Init(env, init_info)) {
            LOGE("lsplant::Init failed");
            return;
        }
        LOGI("[✓] lsplant::Init succeeded");

        // 3. Load payload.dex from the dex embedded in this .so (no file access needed,
        //    which avoids getModuleDir and SELinux restrictions on /data/adb from app domain).
        g_payload_dex.assign(kPayloadDex, kPayloadDex + kPayloadDexLen);
        LOGI("[\xe2\x9c\x93] Loaded embedded payload.dex (%zu bytes)", g_payload_dex.size());

        // 4. Create InMemoryDexClassLoader
        jobject byte_buffer = env->NewDirectByteBuffer(g_payload_dex.data(), g_payload_dex.size());
        if (!byte_buffer || env->ExceptionCheck()) {
            env->ExceptionClear();
            LOGE("Failed to create direct ByteBuffer for payload.dex");
            return;
        }

        // Retrieve current process ClassLoader
        jobject gms_loader = nullptr;
        jclass thread_cls = env->FindClass("java/lang/Thread");
        if (thread_cls) {
            jmethodID cur_thread_mid = env->GetStaticMethodID(
                thread_cls, "currentThread", "()Ljava/lang/Thread;");
            if (cur_thread_mid) {
                jobject cur_thread = env->CallStaticObjectMethod(thread_cls, cur_thread_mid);
                if (cur_thread) {
                    jmethodID get_ccl_mid = env->GetMethodID(
                        thread_cls, "getContextClassLoader", "()Ljava/lang/ClassLoader;");
                    if (get_ccl_mid) {
                        gms_loader = env->CallObjectMethod(cur_thread, get_ccl_mid);
                    }
                }
            }
        }
        if (!gms_loader) {
            jclass cl_cls = env->FindClass("java/lang/ClassLoader");
            if (cl_cls) {
                jmethodID get_sys_cl_mid = env->GetStaticMethodID(
                    cl_cls, "getSystemClassLoader", "()Ljava/lang/ClassLoader;");
                if (get_sys_cl_mid) {
                    gms_loader = env->CallStaticObjectMethod(cl_cls, get_sys_cl_mid);
                }
            }
        }

        jclass imd_cls = env->FindClass("dalvik/system/InMemoryDexClassLoader");
        if (!imd_cls || env->ExceptionCheck()) {
            env->ExceptionClear();
            LOGE("FindClass(dalvik/system/InMemoryDexClassLoader) failed");
            return;
        }
        jmethodID imd_init = env->GetMethodID(
            imd_cls, "<init>", "(Ljava/nio/ByteBuffer;Ljava/lang/ClassLoader;)V");
        if (!imd_init || env->ExceptionCheck()) {
            env->ExceptionClear();
            LOGE("InMemoryDexClassLoader.<init>(ByteBuffer, ClassLoader) not found");
            return;
        }
        jobject dex_loader = env->NewObject(imd_cls, imd_init, byte_buffer, gms_loader);
        if (!dex_loader || env->ExceptionCheck()) {
            env->ExceptionDescribe();
            env->ExceptionClear();
            LOGE("Failed to create InMemoryDexClassLoader");
            return;
        }
        LOGI("[✓] InMemoryDexClassLoader instantiated");

        // 5. Load Entry class and register nativeHook
        jmethodID load_class_mid = env->GetMethodID(
            imd_cls, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
        if (!load_class_mid || env->ExceptionCheck()) {
            env->ExceptionClear();
            LOGE("InMemoryDexClassLoader.loadClass method not found");
            return;
        }
        jstring entry_name = env->NewStringUTF("com.midori.xga.Entry");
        auto entry_cls = static_cast<jclass>(
            env->CallObjectMethod(dex_loader, load_class_mid, entry_name));
        if (!entry_cls || env->ExceptionCheck()) {
            env->ExceptionDescribe();
            env->ExceptionClear();
            LOGE("Failed to load class com.midori.xga.Entry");
            return;
        }

        static JNINativeMethod methods[] = {
            {
                const_cast<char *>("nativeHook"),
                const_cast<char *>(
                    "(Ljava/lang/reflect/Method;Ljava/lang/reflect/Method;Ljava/lang/Object;)Z"),
                reinterpret_cast<void *>(nativeHook)
            },
            {
                const_cast<char *>("nativeHookBackup"),
                const_cast<char *>(
                    "(Ljava/lang/reflect/Method;Ljava/lang/reflect/Method;Ljava/lang/Object;)Ljava/lang/reflect/Method;"),
                reinterpret_cast<void *>(nativeHookBackup)
            }
        };
        if (env->RegisterNatives(entry_cls, methods, sizeof(methods) / sizeof(methods[0])) != 0) {
            env->ExceptionDescribe();
            env->ExceptionClear();
            LOGE("RegisterNatives failed for nativeHook");
            return;
        }
        LOGI("[✓] JNI nativeHook registered successfully");

        // 6. Call Entry.entry(ClassLoader gmsLoader)
        jmethodID entry_mid = env->GetStaticMethodID(
            entry_cls, "entry", "(Ljava/lang/String;Ljava/lang/ClassLoader;)V");
        if (!entry_mid || env->ExceptionCheck()) {
            env->ExceptionDescribe();
            env->ExceptionClear();
            LOGE("Method com.midori.xga.Entry.entry(String, ClassLoader) not found");
            return;
        }
        LOGI("[*] Calling Entry.entry(process, gmsLoader)...");
        jstring jproc = env->NewStringUTF(process_name.c_str());
        env->CallStaticVoidMethod(entry_cls, entry_mid, jproc, gms_loader);
        if (env->ExceptionCheck()) {
            env->ExceptionDescribe();
            env->ExceptionClear();
            LOGE("Exception occurred while executing Entry.entry()");
        } else {
            LOGI("[✓] Entry.entry() completed");
        }
    }

private:
    Api *api = nullptr;
    JNIEnv *env = nullptr;
    bool is_target_process = false;
    std::string process_name;
};

REGISTER_ZYGISK_MODULE(XgaModule)

static bool module_active(const std::string &dir) {
    return access(dir.c_str(), F_OK) == 0 &&
           access((dir + "/disable").c_str(), F_OK) != 0 &&
           access((dir + "/remove").c_str(), F_OK) != 0;
}

static void companion_handler(int fd) {
    uint8_t allowed = module_active("/data/adb/modules/gmstweaks") ? 0 : 1;
    write(fd, &allowed, 1);
}

REGISTER_ZYGISK_COMPANION(companion_handler)
