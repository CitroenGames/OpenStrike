#pragma once

#include <cstdint>
#include <string>

namespace openstrike
{
struct HudState
{
    bool visible = false;
    bool alive = true;
    bool grounded = true;
    bool connected = false;
    bool carrying_c4 = false;
    bool has_defuser = false;

    int health = 100;
    int max_health = 100;
    int armor = 0;
    int money = 800;
    int ammo_in_clip = 12;
    int reserve_ammo = 24;
    int kills = 0;
    int deaths = 0;
    int counter_terrorist_score = 0;
    int terrorist_score = 0;
    int ping_ms = 0;

    float speed = 0.0F;
    float crosshair_gap = 18.0F;
    double round_time_seconds = 115.0;

    std::string team_name = "Counter-Terrorists";
    std::string weapon_name = "USP-S";
    std::string round_phase = "WARMUP";
    std::string network_label = "Local";
    std::string kill_feed;

    std::uint64_t revision = 0;
};
}
