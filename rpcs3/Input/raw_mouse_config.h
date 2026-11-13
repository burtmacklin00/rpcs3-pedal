#pragma once

#include "Utilities/Config.h"
#include "Utilities/mutex.h"

#include <array>

#ifdef _WIN32
#include <windows.h>
#endif

static const std::map<std::string_view, int> raw_mouse_button_map
{
	{ "", 0 },
#ifdef _WIN32
	{ "Button 1", RI_MOUSE_BUTTON_1_UP },
	{ "Button 2", RI_MOUSE_BUTTON_2_UP },
	{ "Button 3", RI_MOUSE_BUTTON_3_UP },
	{ "Button 4", RI_MOUSE_BUTTON_4_UP },
	{ "Button 5", RI_MOUSE_BUTTON_5_UP },
#endif
};

struct raw_mouse_config : cfg::node
{
public:
	using cfg::node::node;

	cfg::string device{this, "Device", ""};

	cfg::_float<10, 1000> mouse_acceleration{ this, "Mouse Acceleration", 100.0f, true };

	cfg::string mouse_button_1{ this, "Button 1", "Button 1", true };
	cfg::string mouse_button_2{ this, "Button 2", "Button 2", true };
	cfg::string mouse_button_3{ this, "Button 3", "Button 3", true };
	cfg::string mouse_button_4{ this, "Button 4", "Button 4", true };
	cfg::string mouse_button_5{ this, "Button 5", "Button 5", true };
	cfg::string mouse_button_6{ this, "Button 6", "", true };
	cfg::string mouse_button_7{ this, "Button 7", "", true };
	cfg::string mouse_button_8{ this, "Button 8", "", true };

	// Foot pedal (Pedal Config): while this keyboard key is held, "Pedal Button" counts as pressed
	// for this player - on top of the gun's own button, which keeps working.
	// Pedal Key = Windows virtual-key code in decimal (e.g. 67 = C), empty = off.
	cfg::string pedal_key{ this, "Pedal Key", "", true };
	cfg::string pedal_button{ this, "Pedal Button", "Button 2", true };

	// More keyboard keys for this player, same idea as the pedal: "<key>=<button>, <key>=<button>"
	// e.g. "49=Button 7, 53=Button 6" (key 1 = Start, key 5 = Select). Empty = off.
	cfg::string key_buttons{ this, "Key Buttons", "", true };

	// From the MLGB lightgun fork. Only used by The Shoot (BCUS98222) and HOTD Overkill Extended Cut (BLUS30774);
	// set per game in profiles/*.yml. Aim scale: % of the screen the gun covers, from the centre (100 = normal).
	cfg::uint<10, 200> move_aim_scale_h{ this, "Move Aim Scale Horizontal", 100, true };
	cfg::uint<10, 200> move_aim_scale_v{ this, "Move Aim Scale Vertical", 100, true };
	// The Shoot gestures: Showtime spin (Square input), point down (Triangle input), off-screen = point up
	cfg::_bool shoot_spin_360{ this, "The Shoot Spin 360", false, true };
	cfg::_bool shoot_point_down{ this, "The Shoot Point Down", false, true };
	cfg::_bool shoot_point_up_offscreen{ this, "The Shoot Point Up Offscreen", false, true };
	cfg::_bool shoot_offscreen_middle{ this, "The Shoot Offscreen Middle", false, true };
	// Pedal build: bowling (Graveyard Alley) - hold Circle + the trigger, let go of the trigger = underarm swing
	cfg::_bool shoot_bowl{ this, "The Shoot Bowl", false, true };
	// the swing (so it can be tuned in the profile without a new build)
	cfg::uint<100, 3000> shoot_bowl_time{ this, "The Shoot Bowl Time", 450, true };          // ms
	cfg::uint<0, 3000> shoot_bowl_forward{ this, "The Shoot Bowl Forward", 900, true };      // mm towards the screen
	cfg::uint<0, 3000> shoot_bowl_up{ this, "The Shoot Bowl Up", 550, true };                // mm upwards during the swing
	cfg::uint<0, 2000> shoot_bowl_low{ this, "The Shoot Bowl Low", 350, true };              // mm lower while holding the ball
	cfg::uint<0, 2000> shoot_bowl_back{ this, "The Shoot Bowl Back", 250, true };            // mm further back while holding it
	cfg::_int<-180, 180> shoot_bowl_pitch_start{ this, "The Shoot Bowl Pitch Start", -90, true }; // degrees, -90 = pointing down
	cfg::_int<-180, 180> shoot_bowl_pitch_end{ this, "The Shoot Bowl Pitch End", 30, true };
	cfg::uint<0, 3000> shoot_bowl_steer{ this, "The Shoot Bowl Steer", 800, true };          // mm sideways for aiming at the screen edge
	cfg::uint<0, 1000> shoot_bowl_follow{ this, "The Shoot Bowl Follow", 150, true };        // ms the arm keeps going after the release
	// Pedal build: Deep Strike bat - hold right click and move the gun sideways, the bat turns with it
	cfg::_bool shoot_bat{ this, "The Shoot Bat", false, true };
	cfg::_int<-720, 720> shoot_bat_turn{ this, "The Shoot Bat Turn", 180, true };            // degrees the bat sweeps (minus = the other way)
	cfg::uint<50, 2000> shoot_bat_time{ this, "The Shoot Bat Time", 250, true };              // ms for the swing
	cfg::uint<0, 360> shoot_bat_height{ this, "The Shoot Bat Height", 90, true };             // degrees up / down for aiming over the whole screen height

	cfg::string& get_button_by_index(int index);
	cfg::string& get_button(int code);

	static constexpr std::string_view key_prefix = "Key ";

	static std::string get_button_name(std::string_view value);
	static std::string get_button_name(s32 button_code);
	static std::string get_key_name(s32 scan_code);
};

struct raw_mice_config : cfg::node
{
	raw_mice_config();

	shared_mutex m_mutex;
	static constexpr std::string_view cfg_id = "raw_mouse";
	std::array<std::shared_ptr<raw_mouse_config>, 4> players;
	atomic_t<bool> reload_requested = false;

	bool load();
	void save();
};

extern raw_mice_config g_cfg_raw_mouse;
