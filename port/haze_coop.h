#pragma once
#include "net/session.h"

void haze_coop_register();
void haze_coop_host();
void haze_coop_join(const std::string& code);
void haze_coop_leave();
void haze_coop_update();
haze::net::Status haze_coop_status();
void haze_coop_menu_register();
struct HazeCoopAction { uint64_t generation=0; bool host=false; };
HazeCoopAction haze_coop_take_action();
