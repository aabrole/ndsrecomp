#include "retroachievements.h"

#if defined(NDS_HAVE_RETROACHIEVEMENTS) && defined(__ANDROID__)
#include "state.h"

#include <jni.h>
#include <SDL.h>
#include <SDL_system.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

extern "C" {
#include "rc_client.h"
#include "rc_consoles.h"
}

namespace {

rc_client_t* g_client = nullptr;
bool g_hardcore = false;
std::string g_rom_path;
std::string g_user;

// Java bridge: MyGame class (global ref) + static method ids.
jclass g_java_class = nullptr;
jmethodID g_m_http = nullptr;   // raHttpRequest(long, String, String, String)
jmethodID g_m_notify = nullptr; // raNotify(String, String)
jmethodID g_m_token = nullptr;  // raStoreToken(String, String)

struct Pending {
    rc_client_server_callback_t callback;
    void* callback_data;
};
struct Response {
    uint64_t id;
    int status;
    std::string body;
};
std::mutex g_mtx;
std::unordered_map<uint64_t, Pending> g_pending;
std::vector<Response> g_responses;
uint64_t g_next_id = 1;

JNIEnv* env_for_this_thread() {
    return static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
}

void notify(const char* title, const char* body) {
    std::fprintf(stderr, "[ra] %s: %s\n", title, body ? body : "");
    JNIEnv* env = env_for_this_thread();
    if (!env || !g_java_class || !g_m_notify) return;
    jstring jt = env->NewStringUTF(title);
    jstring jb = env->NewStringUTF(body ? body : "");
    env->CallStaticVoidMethod(g_java_class, g_m_notify, jt, jb);
    env->DeleteLocalRef(jt);
    env->DeleteLocalRef(jb);
}

void store_token(const char* user, const char* token) {
    JNIEnv* env = env_for_this_thread();
    if (!env || !g_java_class || !g_m_token || !user || !token) return;
    jstring ju = env->NewStringUTF(user);
    jstring jt = env->NewStringUTF(token);
    env->CallStaticVoidMethod(g_java_class, g_m_token, ju, jt);
    env->DeleteLocalRef(ju);
    env->DeleteLocalRef(jt);
}

// RA's Nintendo DS map: $000000-$3FFFFF = main RAM @ 0x02000000,
// $1000000-$1003FFF = DTCM (wherever CP15 currently maps it).
uint32_t RC_CCONV read_memory(uint32_t address, uint8_t* buffer,
                              uint32_t num_bytes, rc_client_t*) {
    for (uint32_t i = 0; i < num_bytes; ++i) {
        const uint32_t a = address + i;
        if (a < 0x400000u) {
            buffer[i] = bus_debug_read8(9, 0x02000000u + a);
        } else if (a >= 0x1000000u && a < 0x1004000u) {
            if (!g_cp15.dtcm_enable) return i;
            buffer[i] = bus_debug_read8(9, g_cp15.dtcm_base + (a - 0x1000000u));
        } else {
            return i;
        }
    }
    return num_bytes;
}

void RC_CCONV server_call(const rc_api_request_t* request,
                          rc_client_server_callback_t callback,
                          void* callback_data, rc_client_t*) {
    uint64_t id;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        id = g_next_id++;
        g_pending[id] = Pending{callback, callback_data};
    }
    JNIEnv* env = env_for_this_thread();
    if (!env || !g_java_class || !g_m_http) {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_responses.push_back(Response{id, RC_API_SERVER_RESPONSE_CLIENT_ERROR,
                                       "no http bridge"});
        return;
    }
    jstring jurl = env->NewStringUTF(request->url ? request->url : "");
    jstring jpost = request->post_data ? env->NewStringUTF(request->post_data)
                                       : nullptr;
    jstring jtype = request->content_type
                        ? env->NewStringUTF(request->content_type) : nullptr;
    env->CallStaticVoidMethod(g_java_class, g_m_http,
                              static_cast<jlong>(id), jurl, jpost, jtype);
    env->DeleteLocalRef(jurl);
    if (jpost) env->DeleteLocalRef(jpost);
    if (jtype) env->DeleteLocalRef(jtype);
}

void RC_CCONV log_message(const char* message, const rc_client_t*) {
    std::fprintf(stderr, "[ra] %s\n", message);
}

void RC_CCONV event_handler(const rc_client_event_t* event, rc_client_t*) {
    char line[256];
    switch (event->type) {
    case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
        std::snprintf(line, sizeof(line), "%s (%u pts)\n%s",
                      event->achievement->title, event->achievement->points,
                      event->achievement->description);
        notify("Achievement unlocked", line);
        break;
    case RC_CLIENT_EVENT_GAME_COMPLETED:
        notify("Game mastered", "Every achievement earned.");
        break;
    case RC_CLIENT_EVENT_SERVER_ERROR:
        std::snprintf(line, sizeof(line), "%s: %s",
                      event->server_error->api,
                      event->server_error->error_message);
        std::fprintf(stderr, "[ra] server error %s\n", line);
        break;
    case RC_CLIENT_EVENT_DISCONNECTED:
        std::fprintf(stderr, "[ra] disconnected; unlocks queued\n");
        break;
    case RC_CLIENT_EVENT_RECONNECTED:
        std::fprintf(stderr, "[ra] reconnected; queued unlocks sent\n");
        break;
    default:
        break;
    }
}

void RC_CCONV load_game_callback(int result, const char* error_message,
                                 rc_client_t* client, void*) {
    if (result != RC_OK) {
        char line[256];
        std::snprintf(line, sizeof(line), "%s",
                      error_message ? error_message : "unknown error");
        notify(result == RC_NO_GAME_LOADED ? "RetroAchievements: game not recognized"
                                          : "RetroAchievements: load failed",
               line);
        return;
    }
    const rc_client_game_t* game = rc_client_get_game_info(client);
    rc_client_user_game_summary_t summary{};
    rc_client_get_user_game_summary(client, &summary);
    char line[256];
    std::snprintf(line, sizeof(line), "%s\n%u of %u achievements unlocked%s",
                  game ? game->title : "?",
                  summary.num_unlocked_achievements,
                  summary.num_core_achievements,
                  g_hardcore ? " (hardcore)" : "");
    notify("RetroAchievements ready", line);
}

void RC_CCONV login_callback(int result, const char* error_message,
                             rc_client_t* client, void*) {
    if (result != RC_OK) {
        notify("RetroAchievements login failed",
               error_message ? error_message : "unknown error");
        return;
    }
    const rc_client_user_t* user = rc_client_get_user_info(client);
    if (user) {
        store_token(user->username, user->token);
        char line[128];
        std::snprintf(line, sizeof(line), "Logged in as %s", user->display_name);
        notify("RetroAchievements", line);
    }
    rc_client_begin_identify_and_load_game(client, RC_CONSOLE_NINTENDO_DS,
                                           g_rom_path.c_str(), nullptr, 0,
                                           load_game_callback, nullptr);
}

}  // namespace

extern "C" JNIEXPORT void JNICALL
Java_com_thor_mph_MyGame_nativeRaBind(JNIEnv* env, jclass cls) {
    if (g_java_class) env->DeleteGlobalRef(g_java_class);
    g_java_class = static_cast<jclass>(env->NewGlobalRef(cls));
    g_m_http = env->GetStaticMethodID(
        cls, "raHttpRequest",
        "(JLjava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
    g_m_notify = env->GetStaticMethodID(
        cls, "raNotify", "(Ljava/lang/String;Ljava/lang/String;)V");
    g_m_token = env->GetStaticMethodID(
        cls, "raStoreToken", "(Ljava/lang/String;Ljava/lang/String;)V");
    if (env->ExceptionCheck()) env->ExceptionClear();
}

extern "C" JNIEXPORT void JNICALL
Java_com_thor_mph_MyGame_nativeRaHttpResponse(JNIEnv* env, jclass, jlong id,
                                              jint status, jbyteArray body) {
    Response r{static_cast<uint64_t>(id), static_cast<int>(status), {}};
    if (body) {
        const jsize n = env->GetArrayLength(body);
        r.body.resize(static_cast<size_t>(n));
        if (n) env->GetByteArrayRegion(body, 0, n,
                                       reinterpret_cast<jbyte*>(&r.body[0]));
    }
    std::lock_guard<std::mutex> lk(g_mtx);
    g_responses.push_back(std::move(r));
}

bool nds_ra_init(const NdsRaOptions& options) {
    if (!options.enabled || options.user.empty() ||
        (options.token.empty() && options.password.empty()) ||
        options.rom_path.empty())
        return false;
    g_client = rc_client_create(read_memory, server_call);
    if (!g_client) return false;
    g_hardcore = options.hardcore;
    g_rom_path = options.rom_path;
    g_user = options.user;
    rc_client_enable_logging(g_client, RC_CLIENT_LOG_LEVEL_INFO, log_message);
    rc_client_set_event_handler(g_client, event_handler);
    rc_client_set_hardcore_enabled(g_client, g_hardcore ? 1 : 0);
    std::fprintf(stderr, "[ra] rcheevos %s, user %s, hardcore %s\n",
                 "12.4.0", g_user.c_str(),
                 g_hardcore ? "on" : "off");
    if (!options.token.empty())
        rc_client_begin_login_with_token(g_client, g_user.c_str(),
                                         options.token.c_str(),
                                         login_callback, nullptr);
    else
        rc_client_begin_login_with_password(g_client, g_user.c_str(),
                                            options.password.c_str(),
                                            login_callback, nullptr);
    return true;
}

void nds_ra_frame() {
    if (!g_client) return;
    std::vector<Response> ready;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        ready.swap(g_responses);
    }
    for (Response& r : ready) {
        Pending p{};
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            auto it = g_pending.find(r.id);
            if (it == g_pending.end()) continue;
            p = it->second;
            g_pending.erase(it);
        }
        rc_api_server_response_t resp{};
        resp.body = r.body.data();
        resp.body_length = r.body.size();
        resp.http_status_code = r.status;
        p.callback(&resp, p.callback_data);
    }
    rc_client_do_frame(g_client);
}

bool nds_ra_hardcore_active() { return g_client != nullptr && g_hardcore; }

void nds_ra_shutdown() {
    if (!g_client) return;
    rc_client_destroy(g_client);
    g_client = nullptr;
}

#else
bool nds_ra_init(const NdsRaOptions&) { return false; }
void nds_ra_frame() {}
bool nds_ra_hardcore_active() { return false; }
void nds_ra_shutdown() {}
#endif
