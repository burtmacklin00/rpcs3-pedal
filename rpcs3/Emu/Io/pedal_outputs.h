#pragma once

#include <string>

// Pedal build: Hook of the Reaper outputs.
// Sends MAME-style network outputs (TCP port 8000, "name = value\r") for PS3 light gun games.
// Which game memory to watch comes from text files in <emulator folder>/outputs/*.ini, so new
// games or addresses never need a new emulator build.

namespace pedal_outputs
{
	// Starts the outputs server once (only if the outputs folder has at least one .ini file).
	void start();
}

// Pedal build: per-game profiles.
// <emulator folder>/profiles/*.yml - each file lists the game it is for (Match) and only the settings that
// differ for that game (Config: RPCS3 settings, Raw Mouse: gun buttons). Applied automatically at every boot.
namespace pedal_profiles
{
	// Called while booting, after the normal config is loaded. boot_path = what is being booted (game file or savestate)
	void apply_config(const std::string& boot_path, const std::string& title_id);

	// Called after raw_mouse.yml is loaded: applies the game's "Raw Mouse" changes (in memory only, the file is never changed)
	void apply_raw_mouse();
}
