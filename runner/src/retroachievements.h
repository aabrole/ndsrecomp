// RetroAchievements client (rcheevos rc_client) for the Android build.
//
// The emulation thread owns the client: nds_ra_frame() runs once per emulated
// frame, drains HTTP responses that the Java side delivered from a background
// thread, and ticks rc_client_do_frame(). Memory reads use the bus's
// side-effect-free debug read, so achievement logic never perturbs timing.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

struct NdsRaOptions {
    bool enabled = false;
    bool hardcore = false;
    std::string user;
    std::string token;     // preferred; obtained from a previous login
    std::string password;  // used only when no token is stored yet
    std::string rom_path;  // hashed with rc_hash to identify the game
};

bool nds_ra_init(const NdsRaOptions& options);
void nds_ra_frame();
bool nds_ra_hardcore_active();
void nds_ra_shutdown();
