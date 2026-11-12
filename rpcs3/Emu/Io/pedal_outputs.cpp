#include "stdafx.h"
#include "pedal_outputs.h"

#ifdef _WIN32
// WinSock2 must come before anything that pulls in windows.h
#include <WinSock2.h>
#include <ws2tcpip.h>
#endif

#include "Emu/System.h"
#include "Emu/system_utils.hpp"
#include "Emu/Memory/vm.h"
#include "Emu/Io/MouseHandler.h"
#include "Emu/IdManager.h"
#include "Utilities/File.h"
#include "Utilities/StrUtil.h"
#include "Emu/RSX/Overlays/overlay_message.h"
#include "Emu/system_config.h"
#include "Input/raw_mouse_config.h"
#include "util/yaml.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <chrono>
#include <cmath>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

LOG_CHANNEL(outputs_log, "HOTR");

#ifdef _WIN32
#include <zstd.h>

// ---------------------------------------------------------------------------------------------
// Outputs file format  (<emulator folder>/outputs/<anything>.ini)
//
//   [Game]
//   Rom = rpcs3_tc4                  ; name Hook of the Reaper sees (its DefaultLG file name)
//   Match = timecrisis4.self         ; text in the running game's executable path or title ID
//                                    ; (several allowed, separated by commas)
//   Debug = 1                        ; optional: write every value change to RPCS3.log
//   AmmoNeedsLife = 1                ; optional: Pn_Ammo is sent as 0 while Pn_Life is 0 (player not in the game)
//   Pn_Playing = <address>           ; optional output: 0 = player not in the game -> Pn_Ammo and Pn_Life are sent as 0
//   KickWhen = ShootFlag 1           ; optional: recoil kicks only while output ShootFlag is 1
//                                    ; (a range works too: KickWhen = GameState 4-8)
//   HoldWhen = ShootFlag 0           ; optional: while ShootFlag is 0 (or a range, like KickWhen), holding a gun's trigger
//                                    ; (at least HoldDelay ms) sends Pn_Hold 1/0 every 100 ms
//   HoldDelay = 500                  ; optional: how long the trigger must be held before Pn_Hold starts (0 - 5000 ms)
//   HoldGrace = 500                  ; optional: HoldWhen stays on until its value has been off this long (0 - 5000 ms),
//                                    ; for game flags that flicker off for a frame now and then
//   HoldWhen = WheelTimer 1-1000 changing ; "changing": only while that value is also still changing (a countdown
//                                    ; that stops partway when the wheel ends early)
//   PlayingLatch = 1                 ; optional: once Pn_Playing has been on, keep showing life/ammo (no LED "death"
//                                    ; flash when a stage ends and the game sets Pn_Playing back to 0)
//   TriggerStart = 1                 ; optional: while a player is in the stage (Pn_Playing not 0) but has had no life
//                                    ; for a second (continue screen, or Player 2 not joined yet), a new trigger pull
//                                    ; presses Start once (then nothing for 2 s, so it can't pause the game)
//   TriggerStart = 3                 ; like 1, but on any screen: a player with no life (not joined yet, or on the
//                                    ; continue screen) presses Start with a new trigger pull - joining from the menus
//   TriggerStart = 2                 ; the same, and also outside a stage (Pn_Playing 0: menus, skip screens, Player 2
//                                    ; joining on the main menu) every new pull presses Start - not while HoldWhen is on
//   DamagePause = 300                ; optional: no recoil kicks for that player this long after Pn_Damaged, so the
//                                    ; damage rumble is not cut off by the next kick (0 - 2000 ms)
//   TriggerRecoil = 125              ; optional, unlimited auto-fire games: kick on every trigger pull and every 125 ms
//                                    ; while held (only while KickWhen allows and that player's Pn_Life > 0)
//   Capture = 1                      ; optional: memory snapshot after every trigger pull (for finding addresses;
//                                    ; files are named ..._P1shot / _P2shot). Remove when done.
//
//   [Outputs]
//   P1_Ammo = u8 0x00A1B2C3          ; plain address
//   P1_Life = u16 0x10050000 +0x24   ; pointer: read 32-bit pointer at the address, then add +0x24
//                                    ; (more +offsets = more pointer steps)
//   Types: u8 u16 u32 s8 s16 s32 f32 (PS3 memory is big endian; this is handled for you)
//
//   Games whose values move around (different place each boot): in [Game] give bytes that always sit
//   next to the values, and write output addresses as @+offset / @-offset from where they were found:
//   Find = 0000000e 00000005 00000002 00000096   ; hex bytes, spaces ignored; first match is used
//   FindIn = 0x30000000 0x40000000               ; optional search range (this is the default)
//   P1_Life = u32 @-0x6BC
//
// Outputs made automatically from the values above (per player Pn):
//   Pn_CtmRecoil  1 then 0 for every bullet fired (Pn_Ammo goes down by 1-5, or Pn_Shots goes up)
//                 (several counters per player work too: P1_Shots_Stage1, P1_Shots_Stage2 ... each kicks when it goes up)
//                 Several weapons: Pn_Ammo_<anything> (one per weapon) each kick; Pn_Ammo is then display only
//   Optional after the address on an Ammo / Shots line:
//     kick=P1_Grenade   this weapon's kicks go to output P1_Grenade instead of P1_CtmRecoil (own feel in the DefaultLG file)
//     pulse=150         each kick is held on 150 ms instead of 40
//   Pn_Damaged    1 then 0 when Pn_Life goes down but not to 0 (never on the killing hit)
//   Pn_Death      1 then 0 when Pn_Life goes down to 0
//
// Finding addresses: press F10 while a game runs to save a memory snapshot (plus a screenshot in RPCS3's
// screenshots folder, so you can see what was on screen) to
// <emulator folder>/outputs/dumps/ (compressed). Snapshots taken at known values (9 bullets, 8 bullets...)
// are compared to find where the game keeps them.
// ---------------------------------------------------------------------------------------------


extern atomic_t<bool> g_user_asked_for_screenshot; // RSXThread.cpp: next frame is saved as a screenshot
namespace
{
	using clock_type = std::chrono::steady_clock;

	struct output_def
	{
		std::string name;
		u32 size = 1;
		bool is_signed = false;
		bool is_float = false;
		u32 address = 0;
		std::vector<s64> offsets; // non-empty = pointer chain
		bool relative = false;    // address written as @+0x.. / @-0x..: counted from where [Game] Find was found
		std::string kick;         // kick=<name>: this weapon's recoil goes to <name> instead of Pn_CtmRecoil
		int kick_ms = 40;         // pulse=<ms>: how long each kick is held on
	};

	struct game_def
	{
		std::string file;
		std::string rom;
		std::vector<std::string> match;
		bool debug = false;
		bool capture = false;      // Capture = 1: memory snapshot automatically after every gun trigger pull (finding addresses)
		std::string kick_when;     // KickWhen = <output> <value>: recoil kicks only while that output has that value
		s64 kick_when_value = 0;   //   value, or a range <lo>-<hi>
		s64 kick_when_hi = 0;
		std::string hold_when;     // HoldWhen = <output> <value>: while it has that value, holding a gun's trigger sends Pn_Hold
		s64 hold_when_value = 0;   //   (1/0 every 100 ms, e.g. rumble while turning a wheel); value or range <lo>-<hi>
		s64 hold_when_hi = 0;
		int hold_delay_ms = 500;   // HoldDelay = <ms>: trigger held this long before Pn_Hold starts
		int hold_grace_ms = 0;     // HoldGrace = <ms>: HoldWhen stays on until its value has been off this long
		bool hold_changing = false; // HoldWhen ... changing: the value must also still be changing
		bool playing_latch = false; // PlayingLatch = 1: Pn_Playing only gates until it has been on once
		bool trigger_start_anywhere = false; // TriggerStart = 3: no-life rule also outside a stage (join from any screen)
		bool trigger_start_menus = false; // TriggerStart = 2: also outside a stage (menus, skip screens, joining)
		bool trigger_start = false; // TriggerStart = 1: trigger = Start while in the stage with no life (continue)
		int damage_pause_ms = 0;    // DamagePause = <ms>: no kicks for a player this long after Pn_Damaged
		bool ammo_needs_life = false; // AmmoNeedsLife = 1: Pn_Ammo is sent as 0 while Pn_Life is 0 (player not playing)
		int trigger_recoil_ms = 0;  // TriggerRecoil = <ms>: games with unlimited auto-fire - kick on every trigger pull and
		                            //   every <ms> while it is held (only while KickWhen allows and the player's life > 0)
		std::vector<u8> find;      // Find = bytes (hex) the game keeps next to its values; outputs with @ addresses follow it
		u32 find_lo = 0x30000000;  // FindIn = <start> <end> (default 0x30000000 0x40000000)
		u32 find_hi = 0x40000000;
		std::vector<output_def> outputs;
	};

	struct pulse
	{
		std::string name;
		clock_type::time_point when;
		int value;
	};

	std::atomic<bool> g_stop{false};
	std::array<std::atomic<bool>, 4> g_trigger_start{}; // TriggerStart: read by cellGem
	std::thread g_thread;
	std::once_flag g_once;

	std::string lower(std::string s)
	{
		for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return s;
	}

	bool parse_number(std::string s, s64& out)
	{
		s = fmt::trim(s);
		if (s.empty()) return false;
		bool neg = false;
		if (s[0] == '+' || s[0] == '-') { neg = s[0] == '-'; s = s.substr(1); }
		try
		{
			usz pos = 0;
			const u64 v = std::stoull(s, &pos, 0);
			if (pos != s.size()) return false;
			out = neg ? -static_cast<s64>(v) : static_cast<s64>(v);
			return true;
		}
		catch (...)
		{
			return false;
		}
	}

	bool parse_output(const std::string& name, const std::string& value, output_def& out)
	{
		const std::vector<std::string> parts = fmt::split(value, {" ", "\t"});
		if (parts.size() < 2) return false;

		const std::string type = lower(parts[0]);
		out.name = name;
		out.is_float = type == "f32";
		out.is_signed = type.starts_with('s');
		if (type == "u8" || type == "s8") out.size = 1;
		else if (type == "u16" || type == "s16") out.size = 2;
		else if (type == "u32" || type == "s32" || type == "f32") out.size = 4;
		else return false;

		s64 addr = 0;
		if (parts[1].starts_with('@'))
		{
			out.relative = true;
			if (!parse_number(parts[1].substr(1), addr)) return false;
		}
		else if (!parse_number(parts[1], addr)) return false;
		out.address = static_cast<u32>(addr);

		for (usz i = 2; i < parts.size(); i++)
		{
			const std::string pl = lower(parts[i]);
			if (pl.starts_with("kick="))
			{
				out.kick = parts[i].substr(5);
				continue;
			}
			if (pl.starts_with("pulse="))
			{
				s64 ms = 0;
				if (!parse_number(parts[i].substr(6), ms) || ms < 10 || ms > 2000) return false;
				out.kick_ms = static_cast<int>(ms);
				continue;
			}

			s64 off = 0;
			if (!parse_number(parts[i], off)) return false;
			out.offsets.push_back(off);
		}
		return true;
	}

	std::vector<game_def> load_games()
	{
		std::vector<game_def> games;
		const std::string dir = rpcs3::utils::get_emu_dir() + "outputs/";

		for (const auto& entry : fs::dir(dir))
		{
			if (entry.is_directory || !lower(entry.name).ends_with(".ini")) continue;

			fs::file f(dir + entry.name);
			if (!f) continue;

			game_def game;
			game.file = entry.name;
			std::string section;

			for (std::string line : fmt::split(f.to_string(), {"\n"}))
			{
				if (const usz c = line.find(';'); c != umax) line.resize(c);
				line = fmt::trim(line, " \t\r");
				if (line.empty()) continue;

				if (line.front() == '[' && line.back() == ']')
				{
					section = lower(line.substr(1, line.size() - 2));
					continue;
				}

				const usz eq = line.find('=');
				if (eq == umax) continue;
				const std::string key = fmt::trim(line.substr(0, eq), " \t");
				const std::string val = fmt::trim(line.substr(eq + 1), " \t");

				if (section == "game")
				{
					if (lower(key) == "rom") game.rom = val;
					else if (lower(key) == "match")
					{
						for (const std::string& m : fmt::split(val, {","}))
						{
							if (const std::string t = lower(fmt::trim(m)); !t.empty()) game.match.push_back(t);
						}
					}
					else if (lower(key) == "debug") game.debug = val == "1" || lower(val) == "true";
					else if (lower(key) == "capture") game.capture = val == "1" || lower(val) == "true";
					else if (lower(key) == "ammoneedslife") game.ammo_needs_life = val == "1" || lower(val) == "true";
					else if (lower(key) == "triggerrecoil")
					{
						s64 ms = 0;
						if (parse_number(val, ms) && ms >= 40 && ms <= 2000) game.trigger_recoil_ms = static_cast<int>(ms);
						else outputs_log.error("%s: TriggerRecoil must be 40 - 2000 (ms)", entry.name);
					}
					else if (lower(key) == "playinglatch") game.playing_latch = val == "1" || lower(val) == "true";
					else if (lower(key) == "triggerstart")
					{
						game.trigger_start = val == "1" || val == "2" || val == "3" || lower(val) == "true";
						game.trigger_start_menus = val == "2";
						game.trigger_start_anywhere = val == "3";
					}
					else if (lower(key) == "damagepause")
					{
						s64 ms = 0;
						if (parse_number(val, ms) && ms >= 0 && ms <= 2000) game.damage_pause_ms = static_cast<int>(ms);
						else outputs_log.error("%s: DamagePause must be 0 - 2000 (ms)", entry.name);
					}
					else if (lower(key) == "holdgrace")
					{
						s64 ms = 0;
						if (parse_number(val, ms) && ms >= 0 && ms <= 5000) game.hold_grace_ms = static_cast<int>(ms);
						else outputs_log.error("%s: HoldGrace must be 0 - 5000 (ms)", entry.name);
					}
					else if (lower(key) == "holddelay")
					{
						s64 ms = 0;
						if (parse_number(val, ms) && ms >= 0 && ms <= 5000) game.hold_delay_ms = static_cast<int>(ms);
						else outputs_log.error("%s: HoldDelay must be 0 - 5000 (ms)", entry.name);
					}
					else if (lower(key) == "kickwhen" || lower(key) == "holdwhen")
					{
						auto r = fmt::split(val, {" ", "\t"});
						const bool changing = r.size() == 3 && lower(r[2]) == "changing";
						if (changing) r.pop_back();
						s64 lo = 0, hi = 0;
						bool ok = r.size() == 2;
						if (ok)
						{
							// "<value>" or "<lo>-<hi>"
							const usz dash = r[1].find('-', 1);
							if (dash != umax)
								ok = parse_number(r[1].substr(0, dash), lo) && parse_number(r[1].substr(dash + 1), hi) && lo <= hi;
							else
								ok = parse_number(r[1], lo) && ((hi = lo), true);
						}
						if (ok)
						{
							const bool kick = lower(key) == "kickwhen";
							(kick ? game.kick_when : game.hold_when) = r[0];
							(kick ? game.kick_when_value : game.hold_when_value) = lo;
							(kick ? game.kick_when_hi : game.hold_when_hi) = hi;
							if (!kick) game.hold_changing = changing;
						}
						else outputs_log.error("%s: could not read '%s = %s'", entry.name, key, val);
					}
					else if (lower(key) == "find")
					{
						std::string hex;
						for (char c : val) if (std::isxdigit(static_cast<unsigned char>(c))) hex += c;
						game.find.clear();
						for (usz i = 0; i + 1 < hex.size(); i += 2) game.find.push_back(static_cast<u8>(std::stoul(hex.substr(i, 2), nullptr, 16)));
					}
					else if (lower(key) == "findin")
					{
						const auto r = fmt::split(val, {" ", "\t", ","});
						s64 lo = 0, hi = 0;
						if (r.size() == 2 && parse_number(r[0], lo) && parse_number(r[1], hi) && lo < hi)
						{
							game.find_lo = static_cast<u32>(lo);
							game.find_hi = static_cast<u32>(std::min<s64>(hi, 0xFFFFF000));
						}
					}
				}
				else if (section == "outputs")
				{
					output_def out;
					if (parse_output(key, val, out)) game.outputs.push_back(std::move(out));
					else outputs_log.error("%s: could not read '%s = %s'", entry.name, key, val);
				}
			}

			if (!game.rom.empty() && !game.match.empty())
			{
				games.push_back(std::move(game));
			}
		}
		return games;
	}

	bool read_value(const output_def& o, u32 found, s64& out)
	{
		if (o.relative && !found) return false;
		u32 addr = o.relative ? found + o.address : o.address;

		for (usz i = 0; i < o.offsets.size(); i++)
		{
			be_t<u32> ptr{};
			if (!vm::try_access(addr, &ptr, 4, false)) return false;
			addr = static_cast<u32>(static_cast<s64>(+ptr) + o.offsets[i]);
		}

		u8 buf[4]{};
		if (!vm::try_access(addr, buf, o.size, false)) return false;

		u32 raw = 0;
		for (u32 i = 0; i < o.size; i++) raw = (raw << 8) | buf[i];

		if (o.is_float)
		{
			f32 f;
			std::memcpy(&f, &raw, 4);
			out = std::isfinite(f) ? static_cast<s64>(std::lround(f)) : 0;
		}
		else if (o.is_signed)
		{
			out = o.size == 1 ? static_cast<s8>(raw) : o.size == 2 ? static_cast<s16>(raw) : static_cast<s32>(raw);
		}
		else
		{
			out = raw;
		}
		return true;
	}

	// "P1_Ammo" -> ("P1_", "Ammo")
	bool split_player(const std::string& name, std::string& prefix, std::string& what)
	{
		if (name.size() < 4 || (name[0] != 'P' && name[0] != 'p') || name[1] < '1' || name[1] > '4' || name[2] != '_') return false;
		prefix = "P" + std::string(1, name[1]) + "_";
		what = lower(name.substr(3));
		return true;
	}

	struct server
	{
		SOCKET listen_sock = INVALID_SOCKET;
		std::vector<SOCKET> clients;

		std::vector<game_def> games;
		const game_def* active = nullptr;
		u32 found = 0;                         // where [Game] Find was found (0 = not yet)
		clock_type::time_point last_scan{};

		bool find_still_there() const
		{
			if (!found || !active) return false;
			std::vector<u8> cur(active->find.size());
			return vm::try_access(found, cur.data(), static_cast<u32>(cur.size()), false) && cur == active->find;
		}

		void scan_for_find()
		{
			const std::vector<u8>& pat = active->find;
			std::vector<u8> buf(4096 + pat.size());
			usz keep = 0; // bytes carried over from the previous page (pattern across a page edge)
			u32 prev_end = 0;
			for (u64 a = active->find_lo; a < active->find_hi; a += 4096)
			{
				if (!vm::check_addr(static_cast<u32>(a), vm::page_readable, 4096) ||
					!vm::try_access(static_cast<u32>(a), buf.data() + keep, 4096, false))
				{
					keep = 0;
					continue;
				}
				if (prev_end != a) keep = 0;
				const usz len = keep + 4096;
				const auto it = std::search(buf.begin(), buf.begin() + len, pat.begin(), pat.end());
				if (it != buf.begin() + len)
				{
					found = static_cast<u32>(a - keep + (it - buf.begin()));
					outputs_log.notice("%s: Find pattern at 0x%x", active->rom, found);
					return;
				}
				keep = std::min<usz>(pat.size() - 1, len);
				std::memmove(buf.data(), buf.data() + len - keep, keep);
				prev_end = static_cast<u32>(a + 4096);
			}
		}
		std::string active_key;
		std::map<std::string, s64> values;     // last value of every output (watched + made), as sent
		std::map<std::string, s64> raw;        // last value read from the game (watched outputs)
		std::map<std::string, bool> have;      // watched output read at least once
		std::deque<pulse> pulses;

		bool open()
		{
			WSADATA wsa{};
			if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;

			listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
			if (listen_sock == INVALID_SOCKET) return false;

			sockaddr_in addr{};
			addr.sin_family = AF_INET;
			addr.sin_port = htons(8000);
			addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

			if (bind(listen_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(listen_sock, 4) != 0)
			{
				outputs_log.error("Could not open TCP port 8000 for Hook of the Reaper (is MAME or another output program running?)");
				closesocket(listen_sock);
				listen_sock = INVALID_SOCKET;
				return false;
			}

			u_long nonblock = 1;
			ioctlsocket(listen_sock, FIONBIO, &nonblock);
			outputs_log.notice("Hook of the Reaper outputs ready on TCP port 8000");
			return true;
		}

		void close_all()
		{
			for (SOCKET c : clients) closesocket(c);
			clients.clear();
			if (listen_sock != INVALID_SOCKET) closesocket(listen_sock);
			listen_sock = INVALID_SOCKET;
			WSACleanup();
		}

		void send_to(SOCKET c, const std::string& msg)
		{
			send(c, msg.data(), static_cast<int>(msg.size()), 0);
		}

		void broadcast(const std::string& msg)
		{
			for (auto it = clients.begin(); it != clients.end();)
			{
				if (send(*it, msg.data(), static_cast<int>(msg.size()), 0) == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK)
				{
					closesocket(*it);
					it = clients.erase(it);
				}
				else
				{
					++it;
				}
			}
		}

		void set(const std::string& name, s64 value)
		{
			auto [it, added] = values.try_emplace(name, value);
			if (!added && it->second == value) return;
			it->second = value;

			if (active && active->debug)
			{
				outputs_log.notice("%s = %d", name, value);
			}
			broadcast(fmt::format("%s = %d\r", name, value));
		}

		void accept_clients()
		{
			while (true)
			{
				SOCKET c = accept(listen_sock, nullptr, nullptr);
				if (c == INVALID_SOCKET) break;

				u_long nonblock = 1;
				ioctlsocket(c, FIONBIO, &nonblock);
				BOOL nodelay = TRUE;
				setsockopt(c, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
				clients.push_back(c);
				outputs_log.notice("Hook of the Reaper connected");

				if (active)
				{
					send_to(c, fmt::format("mame_start = %s\r", active->rom));
					for (const auto& [name, value] : values)
					{
						send_to(c, fmt::format("%s = %d\r", name, value));
					}
				}
			}

			// Drop clients that closed the connection
			for (auto it = clients.begin(); it != clients.end();)
			{
				char buf[256];
				const int r = recv(*it, buf, sizeof(buf), 0);
				if (r == 0 || (r == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK))
				{
					closesocket(*it);
					it = clients.erase(it);
				}
				else
				{
					++it;
				}
			}
		}

		void stop_game()
		{
			if (!active) return;
			outputs_log.notice("Game stopped: %s", active->rom);
			broadcast("mame_stop = 1\r");
			active = nullptr;
			active_key.clear();
			values.clear();
			raw.clear();
			found = 0;
			hold_state = {};
			hold_next = {};
			hold_start = {};
			trigger_was_held = {};
			trigger_needs_release = {};
			damage_until = {};
			playing_seen = {};
			hold_seen = false;
			hold_mode_last_on = {};
			for (auto& t : g_trigger_start) t = false;
			ts_was_zero = {};
			ts_press_until = {};
			ts_cooldown = {};
			ts_last_triggers = 0;
			trigger_next_kick = {};
			have.clear();
			pulses.clear();
		}

		void check_game()
		{
			if (Emu.IsStopped())
			{
				stop_game();
				return;
			}

			// Only look at the boot information once the game is fully running (it changes while booting)
			if (!Emu.IsRunning() && !Emu.IsPaused())
			{
				return;
			}

			const std::string exe = Emu.argv.empty() ? std::string() : Emu.argv[0];
			const std::string key = Emu.GetTitleID() + "|" + exe;
			if (key == active_key) return;

			stop_game();
			active_key = key;
			games = load_games(); // read the files again for every new game, so edits apply on the next boot

			const std::string exe_l = lower(exe);
			const std::string id_l = lower(Emu.GetTitleID());

			for (const game_def& g : games)
			{
				for (const std::string& m : g.match)
				{
					if (exe_l.find(m) != umax || id_l == m)
					{
						active = &g;
						break;
					}
				}
				if (active) break;
			}

			if (active)
			{
				outputs_log.notice("Game started: %s (%s, %d outputs)", active->rom, active->file, active->outputs.size());
				broadcast(fmt::format("mame_start = %s\r", active->rom));
			}
		}

		std::array<clock_type::time_point, 4> damage_until{};
		std::array<bool, 4> playing_seen{};

		static u32 player_index(const std::string& prefix)
		{
			return prefix.size() >= 2 && (prefix[0] == 'P' || prefix[0] == 'p') && prefix[1] >= '1' && prefix[1] <= '4' ? static_cast<u32>(prefix[1] - '1') : 4u;
		}

		// remove waiting pulses of an output (a 1 already sent still gets its 0)
		void drop_pulses(const std::string& name, clock_type::time_point now)
		{
			bool on = values.contains(name) && values[name] == 1;
			std::erase_if(pulses, [&](const pulse& p) { return p.name == name && p.when > now; });
			if (on) pulses.push_back({name, now + std::chrono::milliseconds(40), 0});
		}

		// TriggerStart: player in the stage (Pn_Playing not 0) with no life -> a NEW trigger pull presses Start once.
		// Waits 1 s after the life ran out (still shooting when you die must not pause the game), sends one short
		// Start press per pull, then nothing for 2 s (the game may take a moment to give the life back).
		std::array<clock_type::time_point, 4> ts_zero_since{};
		std::array<clock_type::time_point, 4> ts_press_until{};
		std::array<clock_type::time_point, 4> ts_cooldown{};
		std::array<bool, 4> ts_was_zero{};
		u8 ts_last_triggers = 0;

		void update_trigger_start(clock_type::time_point now)
		{
			const u8 triggers = (active && active->trigger_start) ? read_triggers() : 0;
			for (u32 i = 0; i < 4; i++)
			{
				bool zero = false;
				if (active && active->trigger_start)
				{
					const std::string p = "p" + std::to_string(i + 1) + "_";
					const s64* playing = raw_value(p + "playing");
					const s64* life = raw_value(p + "life");
					zero = life && *life <= 0 && ((playing && *playing != 0) || active->trigger_start_anywhere);
				}
				if (zero && !ts_was_zero[i]) ts_zero_since[i] = now;
				ts_was_zero[i] = zero;

				const bool pulled = (triggers & (1u << i)) && !(ts_last_triggers & (1u << i));
				if (zero && pulled && now - ts_zero_since[i] >= std::chrono::milliseconds(1000) && now >= ts_cooldown[i])
				{
					ts_press_until[i] = now + std::chrono::milliseconds(150);
					ts_cooldown[i] = now + std::chrono::milliseconds(2000);
				}

				// TriggerStart = 2: outside a stage (Pn_Playing 0: menus, skip screens, joining on the main menu) every
				// new pull also presses Start once - but not while HoldWhen is on (the wheel)
				if (active && active->trigger_start_menus && pulled && !hold_mode_on && now >= ts_cooldown[i])
				{
					const s64* playing = raw_value("p" + std::to_string(i + 1) + "_playing");
					if (playing && *playing == 0)
					{
						ts_press_until[i] = now + std::chrono::milliseconds(150);
						ts_cooldown[i] = now + std::chrono::milliseconds(500);
					}
				}
				g_trigger_start[i] = now < ts_press_until[i];
			}
			ts_last_triggers = triggers;
		}

		void add_pulses(const std::string& name, int count, clock_type::time_point now, int length_ms, int gap_ms)
		{
			// queue after any pulses of the same output that are still waiting
			clock_type::time_point start = now;
			for (const pulse& p : pulses)
			{
				if (p.name == name && p.when + std::chrono::milliseconds(gap_ms) > start) start = p.when + std::chrono::milliseconds(gap_ms);
			}
			for (int i = 0; i < count; i++)
			{
				const auto t = start + std::chrono::milliseconds(i * (length_ms + gap_ms));
				pulses.push_back({name, t, 1});
				pulses.push_back({name, t + std::chrono::milliseconds(length_ms), 0});
			}
		}

		// does the running game's file have an output whose name starts with this (e.g. "P1_ammo_")?
		bool active_has(const std::string& start_lower) const
		{
			if (!active) return false;
			for (const output_def& o : active->outputs)
			{
				if (lower(o.name).starts_with(start_lower)) return true;
			}
			return false;
		}

		void poll_values()
		{
			if (!active || !Emu.IsRunning()) return;

			const auto now = clock_type::now();

			if (!active->find.empty() && !find_still_there())
			{
				// look again at most twice a second (the game may move its data, e.g. a new stage)
				found = 0;
				if (now - last_scan < std::chrono::milliseconds(500)) return;
				last_scan = now;
				scan_for_find();
				if (!found) return;
				// values of the old place are not compared with the new place
				have.clear();
			}

			for (const output_def& o : active->outputs)
			{
				s64 v = 0;
				if (!read_value(o, found, v)) continue;

				const auto old_it = raw.find(o.name);
				const bool had = have[o.name] && old_it != raw.end();
				const s64 old = had ? old_it->second : v;
				raw[o.name] = v;
				have[o.name] = true;

				std::string prefix, what;
				const bool is_player = split_player(o.name, prefix, what);

				// AmmoNeedsLife / Pn_Playing: the ammo (and with Pn_Playing the life) shown is sent after the loop,
				// as 0 while that player is not in the game
				const bool gated = is_player && ((active->ammo_needs_life && what == "ammo") ||
					(what == "ammo" || what == "life") && raw_value(lower(prefix) + "playing"));
				if (!gated) set(o.name, v);

				if (!had || !is_player) continue;

				// Games with several weapons: P1_Ammo_Pistol, P1_Ammo_MG ... each kick when they go down.
				// If a player has any of those, plain P1_Ammo is only the number shown (no kick from it).
				const bool ammo_kicks = what.starts_with("ammo_") || (what == "ammo" && !active_has(lower(prefix) + "ammo_"));

				const bool paused = player_index(prefix) < 4 && now < damage_until[player_index(prefix)];

				if (ammo_kicks && v < old && old - v <= 5 && kick_allowed() && !paused)
				{
					add_pulses(o.kick.empty() ? prefix + "CtmRecoil" : o.kick, static_cast<int>(old - v), now, o.kick_ms, 40);
				}
				else if ((what == "shots" || what.starts_with("shots_")) && v > old && v - old <= 5 && kick_allowed() && !paused)
				{
					add_pulses(o.kick.empty() ? prefix + "CtmRecoil" : o.kick, static_cast<int>(v - old), now, o.kick_ms, 40);
				}
				else if (what == "life" && v < old)
				{
					add_pulses(prefix + (v > 0 ? "Damaged" : "Death"), 1, now, 150, 0);
					if (const u32 pi = player_index(prefix); pi < 4 && v > 0)
					{
						damage_until[pi] = now + std::chrono::milliseconds(active->damage_pause_ms);
						// drop kicks already waiting so the rumble gets through
						if (active->damage_pause_ms > 0) drop_pulses(prefix + "CtmRecoil", now);
					}
				}
			}

			for (const output_def& o : active->outputs)
			{
				std::string prefix, what;
				if (!split_player(o.name, prefix, what) || (what != "ammo" && what != "life")) continue;
				const auto a = raw.find(o.name);
				if (a == raw.end()) continue;

				// Pn_Playing (if the game file has it): 0 = player not in the game -> ammo and life shown as 0
				if (const s64* playing = raw_value(lower(prefix) + "playing"))
				{
					const u32 pi = player_index(prefix);
					if (pi < 4 && *playing != 0) playing_seen[pi] = true;
					const bool shown = *playing != 0 || (active->playing_latch && pi < 4 && playing_seen[pi]);
					set(o.name, shown ? a->second : 0);
				}
				else if (active->ammo_needs_life && what == "ammo")
				{
					const s64* life = raw_value(lower(prefix) + "life");
					set(o.name, life && *life > 0 ? a->second : 0);
				}
			}

			update_hold(now);
			update_trigger_recoil(now);
			update_trigger_start(now);
		}

		std::array<bool, 4> trigger_was_held{};
		std::array<bool, 4> trigger_needs_release{}; // pulled while kicks were not allowed (wheel, X crosshairs): no kicks until let go
		std::array<clock_type::time_point, 4> trigger_next_kick{};

		void update_trigger_recoil(clock_type::time_point now)
		{
			if (active->trigger_recoil_ms <= 0) return;

			const u8 triggers = read_triggers();
			const bool state_ok = kick_allowed();

			for (u32 i = 0; i < 4; i++)
			{
				const std::string prefix = "P" + std::to_string(i + 1) + "_";
				const bool held = (triggers & (1u << i)) != 0;
				const s64* life = raw_value(lower(prefix) + "life");
				const bool allowed = state_ok && (!life || *life > 0) && now >= damage_until[i];

				// a pull that started while kicks were not allowed (e.g. still holding from the wheel) never kicks -
				// the player has to let go and pull again. Short blips during a pull that started in play are fine.
				if (!held) trigger_needs_release[i] = false;
				else if (!trigger_was_held[i] && !allowed) trigger_needs_release[i] = true;

				if (held && allowed && !trigger_needs_release[i] && (!trigger_was_held[i] || now >= trigger_next_kick[i]))
				{
					add_pulses(prefix + "CtmRecoil", 1, now, 40, 0);
					trigger_next_kick[i] = now + std::chrono::milliseconds(active->trigger_recoil_ms);
				}

				trigger_was_held[i] = held;
			}
		}

		// value read from the game for an output name (any letter case), or nullptr
		const s64* raw_value(const std::string& name_lower) const
		{
			for (const auto& [name, v] : raw)
			{
				if (lower(name) == name_lower) return &v;
			}
			return nullptr;
		}

		bool kick_allowed() const
		{
			if (active->kick_when.empty()) return true;
			const s64* v = raw_value(lower(active->kick_when));
			return v && *v >= active->kick_when_value && *v <= active->kick_when_hi;
		}

		// Gun triggers held right now (bit 0 = player 1)
		static u8 read_triggers()
		{
			u8 triggers = 0;
			if (auto* handler = g_fxo->try_get<MouseHandlerBase>())
			{
				std::lock_guard lock(handler->mutex);
				const auto& mice = handler->GetMice();
				for (usz i = 0; i < mice.size() && i < 4; i++)
				{
					if (mice[i].buttons & CELL_MOUSE_BUTTON_1) triggers |= static_cast<u8>(1u << i);
				}
			}
			return triggers;
		}

		std::array<clock_type::time_point, 4> hold_next{};
		std::array<clock_type::time_point, 4> hold_start{}; // when the trigger was pressed (0 = not held)
		std::array<int, 4> hold_state{};
		bool hold_seen = false;
		bool hold_mode_on = false; // HoldWhen is true right now (e.g. the wheel is on screen)
		clock_type::time_point hold_mode_last_on{};
		s64 hold_last_value = 0;
		clock_type::time_point hold_last_change{};

		void update_hold(clock_type::time_point now)
		{
			hold_mode_on = false;
			if (active->hold_when.empty()) return;

			const s64* flag = raw_value(lower(active->hold_when));
			bool mode = flag && *flag >= active->hold_when_value && *flag <= active->hold_when_hi;
			if (flag && (!hold_seen || *flag != hold_last_value))
			{
				hold_seen = true;
				hold_last_value = *flag;
				hold_last_change = now;
			}
			// "changing": only while the value moved in the last 300 ms (a countdown that is still running)
			if (active->hold_changing && now - hold_last_change > std::chrono::milliseconds(300)) mode = false;
			// HoldGrace: the game's flag can flicker off for a frame or two - stay on until it has been off this long
			if (mode) hold_mode_last_on = now;
			else if (active->hold_grace_ms > 0 && hold_mode_last_on != clock_type::time_point{} && now - hold_mode_last_on < std::chrono::milliseconds(active->hold_grace_ms)) mode = true;
			hold_mode_on = mode;
			const u8 triggers = mode ? read_triggers() : 0;

			for (u32 i = 0; i < 4; i++)
			{
				const std::string name = "P" + std::to_string(i + 1) + "_Hold";
				if (!(triggers & (1u << i)))
				{
					hold_start[i] = {};
				}
				else if (hold_start[i] == clock_type::time_point{})
				{
					hold_start[i] = now;
				}

				// only a real hold counts (HoldDelay, default half a second) - quick trigger pulls in menus never send Pn_Hold
				if ((triggers & (1u << i)) && now - hold_start[i] >= std::chrono::milliseconds(active->hold_delay_ms))
				{
					if (now >= hold_next[i])
					{
						hold_state[i] ^= 1;
						hold_next[i] = now + std::chrono::milliseconds(100);
						set(name, hold_state[i]);
					}
				}
				else if (hold_state[i])
				{
					hold_state[i] = 0;
					set(name, 0);
				}
			}
		}

		void run_pulses()
		{
			const auto now = clock_type::now();
			std::stable_sort(pulses.begin(), pulses.end(), [](const pulse& a, const pulse& b) { return a.when < b.when; });
			while (!pulses.empty() && pulses.front().when <= now)
			{
				const pulse p = pulses.front();
				pulses.pop_front();
				// pulses always send, even when the value repeats (1 after 1 never happens: every 1 has its 0)
				values[p.name] = p.value;
				if (active && active->debug) outputs_log.notice("%s = %d", p.name, p.value);
				broadcast(fmt::format("%s = %d\r", p.name, p.value));
			}
		}

		int dump_count = 0;

		// Capture mode: trigger pulls seen on each gun, and when to take the snapshot
		u8 last_triggers = 0;
		clock_type::time_point capture_at{};
		std::string capture_tag;

		void check_capture()
		{
			if (!active || !active->capture || !Emu.IsRunning()) return;

			const u8 triggers = read_triggers();
			const u8 pressed = triggers & ~last_triggers;
			last_triggers = triggers;
			const auto now = clock_type::now();

			if (pressed && capture_at == clock_type::time_point{})
			{
				for (u32 i = 0; i < 4; i++)
				{
					if (pressed & (1u << i)) { capture_tag = "_P" + std::to_string(i + 1) + "shot"; break; }
				}
				// a moment after the shot, so the game has counted it
				capture_at = now + std::chrono::milliseconds(400);
			}

			if (capture_at != clock_type::time_point{} && now >= capture_at)
			{
				capture_at = {};
				dump_memory(capture_tag);
			}
		}

		void dump_memory(const std::string& tag = {})
		{
			if (Emu.IsStopped()) return;

			// also take a normal RPCS3 screenshot (screenshots folder) so each snapshot has a picture of the screen
			g_user_asked_for_screenshot = true;

			const std::string dir = rpcs3::utils::get_emu_dir() + "outputs/dumps/";
			fs::create_path(dir);

			const std::string base = (active ? active->rom : Emu.GetTitleID()) + "_" + std::to_string(++dump_count) + tag;
			const std::string path = dir + base + ".dump.zst";
			fs::file f(path, fs::rewrite);
			if (!f)
			{
				outputs_log.error("Could not write %s", path);
				return;
			}

			ZSTD_CCtx* zc = ZSTD_createCCtx();
			ZSTD_CCtx_setParameter(zc, ZSTD_c_compressionLevel, 3);
			std::vector<u8> out(ZSTD_CStreamOutSize());
			std::vector<u8> page(4 + 4096);
			usz pages = 0;

			const auto write_chunk = [&](const void* data, usz size, ZSTD_EndDirective mode)
			{
				ZSTD_inBuffer in{data, size, 0};
				while (true)
				{
					ZSTD_outBuffer ob{out.data(), out.size(), 0};
					const usz rem = ZSTD_compressStream2(zc, &ob, &in, mode);
					f.write(out.data(), ob.pos);
					if (ZSTD_isError(rem)) break;
					if (mode == ZSTD_e_end ? rem == 0 : in.pos == in.size) break;
				}
			};

			// Every readable, non-empty 4 KB page below the video memory: [u32 address (little endian)][4096 bytes]
			for (u64 addr = 0x10000; addr < 0xC0000000; addr += 4096)
			{
				if (!vm::check_addr(static_cast<u32>(addr), vm::page_readable, 4096)) continue;
				if (!vm::try_access(static_cast<u32>(addr), page.data() + 4, 4096, false)) continue;
				if (std::all_of(page.begin() + 4, page.end(), [](u8 b) { return b == 0; })) continue;

				const u32 a = static_cast<u32>(addr);
				std::memcpy(page.data(), &a, 4);
				write_chunk(page.data(), page.size(), ZSTD_e_continue);
				pages++;
			}
			write_chunk(nullptr, 0, ZSTD_e_end);
			ZSTD_freeCCtx(zc);

			outputs_log.notice("Memory snapshot %s saved (%d pages)", path, pages);
			rsx::overlays::queue_message(std::string("Memory snapshot saved: ") + base, 3'000'000);
		}

		void operator()()
		{
			if (!open()) return;

			while (!g_stop)
			{
				accept_clients();
				check_game();
				poll_values();
				run_pulses();
				check_capture();

				// F10 = memory snapshot (for finding ammo / life addresses)
				if ((GetAsyncKeyState(VK_F10) & 1) && Emu.IsRunning())
				{
					dump_memory();
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}

			stop_game();
			close_all();
		}
	};

	struct stopper
	{
		~stopper()
		{
			g_stop = true;
			if (g_thread.joinable()) g_thread.join();
		}
	} g_stopper;
}

namespace pedal_outputs
{
	bool trigger_is_start(u32 player)
	{
		return player < 4 && g_trigger_start[player].load();
	}

	void start()
	{
		std::call_once(g_once, []()
		{
			const std::string dir = rpcs3::utils::get_emu_dir() + "outputs/";
			bool any = false;
			for (const auto& entry : fs::dir(dir))
			{
				if (!entry.is_directory && lower(entry.name).ends_with(".ini")) { any = true; break; }
			}
			if (!any)
			{
				outputs_log.notice("No outputs/*.ini files: Hook of the Reaper outputs are off");
				return;
			}
			g_thread = std::thread([]() { server{}(); });

			// Stop the server before the emulator's own globals go away when RPCS3 closes
			std::atexit([]()
			{
				g_stop = true;
				if (g_thread.joinable()) g_thread.join();
			});
		});
	}
}

#else

namespace pedal_outputs
{
	void start() {}
	bool trigger_is_start(u32) { return false; }
}

#endif

// ---------------------------------------------------------------------------------------------
// Per-game profiles (all platforms)
//
//   <emulator folder>/profiles/Time Crisis 4.yml:
//
//   Match: [timecrisis4.self, Time Crisis 4]   # file name of the game file or savestate (part of it), or a title ID
//   Config:                                     # only the RPCS3 settings that differ for this game
//     Video:
//       Frame limit: 50
//   Raw Mouse:                                  # optional: gun button changes for this game only
//     Player 1:
//       Button 7: Button 2
// ---------------------------------------------------------------------------------------------

LOG_CHANNEL(profiles_log, "Profiles");

namespace
{
	std::mutex g_profile_mutex;
	std::string g_profile_raw_mouse; // YAML of the active profile's "Raw Mouse" part

	std::string lower_ascii(std::string s)
	{
		for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return s;
	}
}

namespace pedal_profiles
{
	void apply_config(const std::string& boot_path, const std::string& title_id)
	{
		std::lock_guard lock(g_profile_mutex);
		g_profile_raw_mouse.clear();

		const std::string dir = rpcs3::utils::get_emu_dir() + "profiles/";
		if (!fs::is_dir(dir)) return;

		// Only the file name counts (folder names like "Time Crisis Razing Storm" would match every game on the disc)
		std::string file = boot_path;
		if (const usz pos = file.find_last_of("/\\"); pos != umax) file = file.substr(pos + 1);
		const std::string file_l = lower_ascii(file);
		const std::string id_l = lower_ascii(title_id);

		std::vector<std::string> names;
		for (const auto& entry : fs::dir(dir))
		{
			if (!entry.is_directory && lower_ascii(entry.name).ends_with(".yml")) names.push_back(entry.name);
		}
		std::sort(names.begin(), names.end());

		// Two passes: a file name match wins over a title ID match
		for (int pass = 0; pass < 2; pass++)
		{
			for (const std::string& name : names)
			{
				fs::file f(dir + name);
				if (!f) continue;

				auto [root, error] = yaml_load(f.to_string());
				if (!error.empty() || !root.IsMap())
				{
					profiles_log.error("profiles/%s: %s", name, error);
					continue;
				}

				std::vector<std::string> match;
				if (const YAML::Node m = root["Match"]; m.IsSequence())
				{
					for (const auto& v : m) match.push_back(lower_ascii(v.Scalar()));
				}
				else if (m.IsScalar())
				{
					match.push_back(lower_ascii(m.Scalar()));
				}

				bool hit = false;
				for (const std::string& m : match)
				{
					if (m.empty()) continue;
					if (pass == 0 && file_l.find(m) != umax) hit = true;
					if (pass == 1 && id_l == m) hit = true;
				}
				if (!hit) continue;

				profiles_log.notice("Using game profile profiles/%s for '%s' (%s)", name, file, title_id);

				if (const YAML::Node cfg = root["Config"]; cfg && cfg.IsMap())
				{
					if (!g_cfg.from_string(YAML::Dump(cfg)))
					{
						profiles_log.error("profiles/%s: could not apply Config", name);
					}
				}

				if (const YAML::Node rm = root["Raw Mouse"]; rm && rm.IsMap())
				{
					g_profile_raw_mouse = YAML::Dump(rm);
				}
				return;
			}
		}
	}

	void apply_raw_mouse()
	{
		std::lock_guard lock(g_profile_mutex);
		if (g_profile_raw_mouse.empty()) return;

		if (g_cfg_raw_mouse.from_string(g_profile_raw_mouse))
		{
			profiles_log.notice("Applied the game profile's Raw Mouse settings");
		}
		else
		{
			profiles_log.error("Could not apply the game profile's Raw Mouse settings");
		}
	}
}
