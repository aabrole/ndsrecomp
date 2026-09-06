#include "retroachievements.h"

#if defined(NDS_HAVE_RETROACHIEVEMENTS) && defined(__ANDROID__)
#include "state.h"

#include <jni.h>
#include <android/log.h>
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
#include "rc_hash.h"
}

namespace {

rc_client_t* g_client = nullptr;
bool g_hardcore = false;
std::string g_rom_path;
std::string g_user;
std::string g_hash_override;

// Java bridge: MyGame class (global ref) + static method ids.
jclass g_java_class = nullptr;
jmethodID g_m_http = nullptr;   // raHttpRequest(long, String, String, String)
jmethodID g_m_notify = nullptr; // raNotify(String, String)
jmethodID g_m_token = nullptr;  // raStoreToken(String, String)
jmethodID g_m_status = nullptr; // raStatus(String)

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

thread_local JNIEnv* t_browse_env = nullptr;

JNIEnv* env_for_this_thread() {
    if (t_browse_env) return t_browse_env;
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

void status_line(const char* line) {
    JNIEnv* env = env_for_this_thread();
    if (!env || !g_java_class || !g_m_status) return;
    jstring js = env->NewStringUTF(line ? line : "");
    env->CallStaticVoidMethod(g_java_class, g_m_status, js);
    env->DeleteLocalRef(js);
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
        {
            rc_client_user_game_summary_t summary{};
            rc_client_get_user_game_summary(g_client, &summary);
            std::snprintf(line, sizeof(line), "RA  %s  ·  %u / %u achievements%s",
                          g_user.c_str(), summary.num_unlocked_achievements,
                          summary.num_core_achievements,
                          g_hardcore ? "  ·  hardcore" : "");
            status_line(line);
        }
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
        status_line(result == RC_NO_GAME_LOADED
            ? "RA: this ROM hash is not linked on retroachievements.org"
            : "RA: game load failed");
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
    std::snprintf(line, sizeof(line), "RA  %s  ·  %u / %u achievements%s",
                  g_user.c_str(), summary.num_unlocked_achievements,
                  summary.num_core_achievements,
                  g_hardcore ? "  ·  hardcore" : "");
    if (!g_hash_override.empty())
        std::strncat(line, "  ·  hash override", sizeof(line) - std::strlen(line) - 1);
    status_line(line);
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
    if (!g_hash_override.empty()) {
        // Log the ROM's real hash first so the substitution is on record.
        char real_hash[33] = {};
        rc_hash_iterator_t iterator;
        rc_hash_initialize_iterator(&iterator, g_rom_path.c_str(), nullptr, 0);
        iterator.consoles[0] = RC_CONSOLE_NINTENDO_DS;
        iterator.consoles[1] = 0;
        if (rc_hash_generate(real_hash, RC_CONSOLE_NINTENDO_DS, &iterator))
            std::fprintf(stderr, "[ra] ROM hash %s; reporting override %s "
                         "(user-enabled)\n", real_hash, g_hash_override.c_str());
        rc_hash_destroy_iterator(&iterator);
        rc_client_begin_load_game(client, g_hash_override.c_str(),
                                  load_game_callback, nullptr);
        return;
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
    g_m_status = env->GetStaticMethodID(cls, "raStatus", "(Ljava/lang/String;)V");
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

// ---------------------------------------------------------------------------
// Browse mode (settings screen): log in with the stored token, load the game,
// and return the achievement list with the user's unlock state as JSON. Runs
// a private client on the calling Java thread and pumps HTTP responses
// itself; refuses to run while the in-game client is active.
namespace {
struct BrowseState {
    int login_result = -1;
    int load_result = -1;
    bool login_done = false;
    bool load_done = false;
    std::string error;
};

void RC_CCONV browse_login_cb(int result, const char* msg, rc_client_t*, void* ud) {
    auto* st = static_cast<BrowseState*>(ud);
    st->login_result = result; st->login_done = true;
    if (result != RC_OK && msg) st->error = msg;
}
void RC_CCONV browse_load_cb(int result, const char* msg, rc_client_t*, void* ud) {
    auto* st = static_cast<BrowseState*>(ud);
    st->load_result = result; st->load_done = true;
    if (result != RC_OK && msg) st->error = msg;
}
void RC_CCONV browse_event(const rc_client_event_t*, rc_client_t*) {}

void pump_responses_once() {
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
}

bool pump_until(rc_client_t* client, const bool& flag, int timeout_ms) {
    for (int waited = 0; !flag && waited < timeout_ms; waited += 20) {
        pump_responses_once();
        rc_client_idle(client);
        SDL_Delay(20);
    }
    return flag;
}

void json_escape(std::string& out, const char* v) {
    out += '"';
    for (const char* c = v ? v : ""; *c; ++c) {
        switch (*c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        default:
            if (static_cast<unsigned char>(*c) < 0x20) out += ' ';
            else out += *c;
        }
    }
    out += '"';
}
}  // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_com_thor_mph_SettingsActivity_nativeRaBrowse(JNIEnv* env, jclass,
                                                  jstring juser, jstring jtoken,
                                                  jstring jrom, jstring joverride) {
    if (g_client) return env->NewStringUTF("{\"error\":\"close the game first\"}");
    const char* user = env->GetStringUTFChars(juser, nullptr);
    const char* token = env->GetStringUTFChars(jtoken, nullptr);
    const char* rom = env->GetStringUTFChars(jrom, nullptr);
    const char* over = joverride ? env->GetStringUTFChars(joverride, nullptr) : nullptr;
    t_browse_env = env;
    std::string out;
    rc_client_t* client = rc_client_create(read_memory, server_call);
    BrowseState st;
    if (!client) {
        out = "{\"error\":\"client\"}";
    } else {
        rc_client_set_event_handler(client, browse_event);
        rc_client_set_hardcore_enabled(client, 0);
        rc_client_begin_login_with_token(client, user, token, browse_login_cb, &st);
        if (!pump_until(client, st.login_done, 20000) || st.login_result != RC_OK) {
            out = "{\"error\":"; json_escape(out, st.error.empty() ? "login timed out" : st.error.c_str()); out += "}";
        } else {
            if (over && *over)
                rc_client_begin_load_game(client, over, browse_load_cb, &st);
            else
                rc_client_begin_identify_and_load_game(client, RC_CONSOLE_NINTENDO_DS,
                                                       rom, nullptr, 0, browse_load_cb, &st);
            if (!pump_until(client, st.load_done, 30000) || st.load_result != RC_OK) {
                out = "{\"error\":"; json_escape(out, st.load_result == RC_NO_GAME_LOADED
                    ? "game not recognized (hash not linked on RA)"
                    : (st.error.empty() ? "load timed out" : st.error.c_str())); out += "}";
            } else {
                const rc_client_game_t* game = rc_client_get_game_info(client);
                rc_client_user_game_summary_t sum{};
                rc_client_get_user_game_summary(client, &sum);
                out = "{\"game\":"; json_escape(out, game ? game->title : "");
                out += ",\"unlocked\":" + std::to_string(sum.num_unlocked_achievements);
                out += ",\"total\":" + std::to_string(sum.num_core_achievements);
                out += ",\"points\":" + std::to_string(sum.points_unlocked);
                out += ",\"achievements\":[";
                rc_client_achievement_list_t* list = rc_client_create_achievement_list(
                    client, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE,
                    RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_LOCK_STATE);
                bool first = true;
                for (uint32_t b = 0; list && b < list->num_buckets; ++b) {
                    const rc_client_achievement_bucket_t& bk = list->buckets[b];
                    for (uint32_t i = 0; i < bk.num_achievements; ++i) {
                        const rc_client_achievement_t* a = bk.achievements[i];
                        char url[256] = {};
                        rc_client_achievement_get_image_url(a, a->state, url, sizeof(url));
                        if (!first) out += ',';
                        first = false;
                        out += "{\"title\":"; json_escape(out, a->title);
                        out += ",\"description\":"; json_escape(out, a->description);
                        out += ",\"points\":" + std::to_string(a->points);
                        out += ",\"unlocked\":" + std::string(a->state == RC_CLIENT_ACHIEVEMENT_STATE_UNLOCKED ? "true" : "false");
                        out += ",\"unlock_time\":" + std::to_string(static_cast<long long>(a->unlock_time));
                        out += ",\"bucket\":"; json_escape(out, bk.label);
                        out += ",\"badge\":"; json_escape(out, url);
                        out += "}";
                    }
                }
                if (list) rc_client_destroy_achievement_list(list);
                out += "]}";
            }
        }
        rc_client_destroy(client);
    }
    t_browse_env = nullptr;
    env->ReleaseStringUTFChars(juser, user);
    env->ReleaseStringUTFChars(jtoken, token);
    env->ReleaseStringUTFChars(jrom, rom);
    if (over) env->ReleaseStringUTFChars(joverride, over);
    __android_log_print(ANDROID_LOG_INFO, "ThorMPH", "RA browse: %zu bytes", out.size());
    return env->NewStringUTF(out.c_str());
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
    g_hash_override = options.hash_override;
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
