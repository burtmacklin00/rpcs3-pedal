#include "stdafx.h"
#include "cellGem.h"
#include "cellCamera.h"

#include "Emu/Cell/lv2/sys_event.h"
#include "Emu/Cell/lv2/sys_memory.h"
#include "Emu/Cell/PPUModule.h"
#include "Emu/Cell/timers.hpp"
#include "Emu/Io/MouseHandler.h"
#include "Emu/Io/PadHandler.h"
#include "Emu/Io/gem_config.h"
#include "Emu/Io/interception.h"
#include "Emu/system_config.h"
#include "Emu/System.h"
#include "Emu/Io/pedal_outputs.h"
#include "Emu/IdManager.h"
#include "Emu/RSX/Overlays/overlay_cursor.h"
#include "Input/pad_thread.h"
#include "Input/ps_move_config.h"
#include "Input/raw_mouse_config.h"
#include "Input/lightgun_spin.h"
#include "Input/lightgun_aim_scale.h"
#include "Input/ps_move_tracker.h"

#ifdef HAVE_LIBEVDEV
#include "Emu/Io/evdev_gun_handler.h"
#endif

#include <cmath> // for fmod
#include <type_traits>
#include <deque>

LOG_CHANNEL(cellGem);

template <>
void fmt_class_string<CellGemError>::format(std::string& out, u64 arg)
{
	format_enum(out, arg, [](auto error)
	{
		switch (error)
		{
			STR_CASE(CELL_GEM_ERROR_RESOURCE_ALLOCATION_FAILED);
			STR_CASE(CELL_GEM_ERROR_ALREADY_INITIALIZED);
			STR_CASE(CELL_GEM_ERROR_UNINITIALIZED);
			STR_CASE(CELL_GEM_ERROR_INVALID_PARAMETER);
			STR_CASE(CELL_GEM_ERROR_INVALID_ALIGNMENT);
			STR_CASE(CELL_GEM_ERROR_UPDATE_NOT_FINISHED);
			STR_CASE(CELL_GEM_ERROR_UPDATE_NOT_STARTED);
			STR_CASE(CELL_GEM_ERROR_CONVERT_NOT_FINISHED);
			STR_CASE(CELL_GEM_ERROR_CONVERT_NOT_STARTED);
			STR_CASE(CELL_GEM_ERROR_WRITE_NOT_FINISHED);
			STR_CASE(CELL_GEM_ERROR_NOT_A_HUE);
		}

		return unknown;
	});
}

template <>
void fmt_class_string<CellGemStatus>::format(std::string& out, u64 arg)
{
	format_enum(out, arg, [](auto error)
	{
		switch (error)
		{
			STR_CASE(CELL_GEM_NOT_CONNECTED);
			STR_CASE(CELL_GEM_SPHERE_NOT_CALIBRATED);
			STR_CASE(CELL_GEM_SPHERE_CALIBRATING);
			STR_CASE(CELL_GEM_COMPUTING_AVAILABLE_COLORS);
			STR_CASE(CELL_GEM_HUE_NOT_SET);
			STR_CASE(CELL_GEM_NO_VIDEO);
			STR_CASE(CELL_GEM_TIME_OUT_OF_RANGE);
			STR_CASE(CELL_GEM_NOT_CALIBRATED);
			STR_CASE(CELL_GEM_NO_EXTERNAL_PORT_DEVICE);
		}

		return unknown;
	});
}

template <>
void fmt_class_string<CellGemVideoConvertFormatEnum>::format(std::string& out, u64 arg)
{
	format_enum(out, arg, [](auto format)
	{
		switch (format)
		{
			STR_CASE(CELL_GEM_NO_VIDEO_OUTPUT);
			STR_CASE(CELL_GEM_RGBA_640x480);
			STR_CASE(CELL_GEM_YUV_640x480);
			STR_CASE(CELL_GEM_YUV422_640x480);
			STR_CASE(CELL_GEM_YUV411_640x480);
			STR_CASE(CELL_GEM_RGBA_320x240);
			STR_CASE(CELL_GEM_BAYER_RESTORED);
			STR_CASE(CELL_GEM_BAYER_RESTORED_RGGB);
			STR_CASE(CELL_GEM_BAYER_RESTORED_RASTERIZED);
		}

		return unknown;
	});
}

// last 4 out of 7 ports (7,6,5,4). index starts at 1
static u32 port_num(u32 gem_num)
{
	ensure(gem_num < CELL_GEM_MAX_NUM);
	return CELL_PAD_MAX_PORT_NUM - gem_num;
}

// last 4 out of 7 ports (6,5,4,3). index starts at 0
static u32 pad_num(u32 gem_num)
{
	ensure(gem_num < CELL_GEM_MAX_NUM);
	return (CELL_PAD_MAX_PORT_NUM - 1) - gem_num;
}

// **********************
// * HLE helper structs *
// **********************

#ifdef HAVE_LIBEVDEV
struct gun_handler
{
public:
	gun_handler() = default;

	static constexpr auto thread_name = "Evdev Gun Thread"sv;

	evdev_gun_handler handler{};
	atomic_t<u32> num_devices{0};

	void operator()()
	{
		if (g_cfg.io.move != move_handler::gun)
		{
			return;
		}

		while (thread_ctrl::state() != thread_state::aborting && !Emu.IsStopped())
		{
			const bool is_active = !Emu.IsPaused() && handler.is_init();

			if (is_active)
			{
				for (u32 i = 0; i < num_devices; i++)
				{
					std::scoped_lock lock(handler.mutex);
					handler.poll(i);
				}
			}

			thread_ctrl::wait_for(is_active ? 1000 : 10000);
		}
	}
};

using gun_thread = named_thread<gun_handler>;

#endif

cfg_gems g_cfg_gem_real;
cfg_fake_gems g_cfg_gem_fake;
cfg_mouse_gems g_cfg_gem_mouse;

// Pedal build: games that use the MLGB lightgun fork's motion data instead of the v0.0.32 behaviour:
// The Shoot (BCUS98222) and The House of the Dead: Overkill Extended Cut (BLUS30774)
static inline bool gem_mlgb_title()
{
	if (g_cfg.io.move != move_handler::raw_mouse) return false;
	const std::string id = Emu.GetTitleID();
	return id == "BCUS98222" || id == "BLUS30774";
}

// Pedal build: mouse / raw mouse guns behave exactly like RPCS3 v0.0.32
// (buttons + combos, aim data, visibility, calibration and status flags).
static inline bool gem_v032_mouse()
{
	return (g_cfg.io.move == move_handler::mouse || g_cfg.io.move == move_handler::raw_mouse) && !gem_mlgb_title();
}

// Pedal build: the v0.0.32 button layout and combos are used by every mouse / raw mouse game,
// including the MLGB games (so Key Buttons, the pedal and profiles work the same everywhere)
static inline bool gem_v032_buttons()
{
	return g_cfg.io.move == move_handler::mouse || g_cfg.io.move == move_handler::raw_mouse;
}

struct gem_config_data
{
public:
	void operator()();

	static constexpr auto thread_name = "Gem Thread"sv;

	atomic_t<u8> state = 0;

	struct gem_color
	{
		ENABLE_BITWISE_SERIALIZATION;

		f32 r, g, b;

		gem_color() : r(0.0f), g(0.0f), b(0.0f) {}
		gem_color(f32 r_, f32 g_, f32 b_)
		{
			r = std::clamp(r_, 0.0f, 1.0f);
			g = std::clamp(g_, 0.0f, 1.0f);
			b = std::clamp(b_, 0.0f, 1.0f);
		}

		static inline const gem_color& get_default_color(u32 gem_num)
		{
			static const gem_color gold = gem_color(1.0f, 0.85f, 0.0f);
			static const gem_color green = gem_color(0.0f, 1.0f, 0.0f);
			static const gem_color red = gem_color(1.0f, 0.0f, 0.0f);
			static const gem_color pink = gem_color(0.9f, 0.0f, 0.5f);

			switch (gem_num)
			{
			case 0: return green;
			case 1: return gold;
			case 2: return red;
			case 3: return pink;
			default: fmt::throw_exception("unexpected gem_num %d", gem_num);
			}
		}
	};

	struct gem_controller
	{
		u32 status = CELL_GEM_STATUS_DISCONNECTED;         // Connection status (CELL_GEM_STATUS_DISCONNECTED or CELL_GEM_STATUS_READY)
		u32 ext_status = 0;                                // External port connection status
		u32 ext_id = 0;                                    // External device ID (type). For example SHARP_SHOOTER_DEVICE_ID
		u32 port = 0;                                      // Assigned port
		bool enabled_magnetometer = true;                  // Whether the magnetometer is enabled (probably used for additional rotational precision)
		bool calibrated_magnetometer = false;              // Whether the magnetometer is calibrated
		bool enabled_filtering = false;                    // Whether filtering is enabled
		bool enabled_tracking = false;                     // Whether tracking is enabled
		bool enabled_LED = false;                          // Whether the LED is enabled
		bool hue_set = false;                              // Whether the hue was set
		u8 rumble = 0;                                     // Rumble intensity
		gem_color sphere_rgb = {};                         // RGB color of the sphere LED
		u32 hue = 0;                                       // Tracking hue of the motion controller
		f32 distance_mm{3000.0f};                          // Distance from the camera in mm
		f32 radius{5.0f};                                  // Radius of the sphere in camera pixels
		bool radius_valid = false;                         // If the radius and distance of the sphere was computed. Also used for visibility.

		bool is_calibrating{false};                        // Whether or not we are currently calibrating
		u64 calibration_start_us{0};                       // The start timestamp of the calibration in microseconds
		u64 calibration_status_flags = 0;                  // The calibration status flags

		u8 firing_mode = button_flags::ss_firing_mode_1;   // The firing mode of the sharpshooter. Used with fake PS-Move. This is a physical switch with 3 positions, not a button that can be pressed.

		static constexpr u64 calibration_time_us = 500000; // The calibration supposedly takes 0.5 seconds (500000 microseconds)
	};

	CellGemAttribute attribute = {};
	CellGemVideoConvertAttribute vc_attribute = {};
	s32 video_data_out_size = -1;
	std::vector<u8> video_data_in;
	u64 runtime_status_flags = 0; // The runtime status flags
	u64 v032_status_flags = 0; // Pedal build: v0.0.32 global status flags (mouse / raw mouse)
	bool enable_pitch_correction = false;
	u32 inertial_counter = 0;

	std::array<gem_controller, CELL_GEM_MAX_NUM> controllers;
	u32 connected_controllers = 0;
	atomic_t<bool> video_conversion_in_progress{false};
	atomic_t<bool> updating{false};
	u32 camera_frame{};
	u32 memory_ptr{};

	shared_mutex mtx;

	u64 start_timestamp_us = 0;

	std::array<ps_move_data, CELL_GEM_MAX_NUM> fake_move_data {}; // No need to be in savestate

	// MLGB lightgun fork: transient synthetic samples for The Shoot's timestamp-based input queries
	// and gesture / motion state (not in savestates)
	std::mutex mouse_history_mutex;
	std::array<std::deque<CellGemState>, CELL_GEM_MAX_NUM> mouse_history;
	u64 mouse_history_epoch = 0;
	std::array<lightgun_spin, CELL_GEM_MAX_NUM> mouse_spin;
	std::array<lightgun_middle_gesture, CELL_GEM_MAX_NUM> mouse_middle_gesture;
	std::array<ps_move_data::vect<3>, CELL_GEM_MAX_NUM> mouse_spin_origin;
	std::array<lightgun_bowl, CELL_GEM_MAX_NUM> mouse_bowl;                 // Pedal build: The Shoot bowling swing
	std::array<ps_move_data::vect<3>, CELL_GEM_MAX_NUM> mouse_bowl_origin;
	std::array<lightgun_bat, CELL_GEM_MAX_NUM> mouse_bat;                   // Pedal build: The Shoot Deep Strike bat
	std::array<u64, CELL_GEM_MAX_NUM> mouse_orientation_time{};
	std::array<ps_move_data::vect<4>, CELL_GEM_MAX_NUM> mouse_previous_orientation{};

	atomic_t<u32> m_wake_up = 0;
	atomic_t<u32> m_done = 0;

	void wake_up()
	{
		m_wake_up.release(1);
		m_wake_up.notify_one();
	}

	void done()
	{
		m_done.release(1);
		m_done.notify_one();
	}

	bool wait_for_result(ppu_thread& ppu)
	{
		// Notify gem thread that the initial state after loading a savestate can be updated.
		if (m_done.compare_and_swap_test(2, 0))
		{
			m_done.notify_one();
		}

		while (!m_done && !ppu.is_stopped())
		{
			thread_ctrl::wait_on(m_done, 0);
		}

		if (ppu.is_stopped())
		{
			ppu.state += cpu_flag::again;
			return false;
		}

		m_done = 0;
		return true;
	}

	// helper functions
	bool is_controller_ready(u32 gem_num) const
	{
		return controllers[gem_num].status == CELL_GEM_STATUS_READY;
	}

	void update_connections()
	{
		connected_controllers = 0;

		const auto update_connection = [this](u32 i, bool connected)
		{
			if (connected)
			{
				connected_controllers++;
				controllers[i].status = CELL_GEM_STATUS_READY;
				controllers[i].port = port_num(i);
			}
			else
			{
				controllers[i].status = CELL_GEM_STATUS_DISCONNECTED;
				controllers[i].port = 0;
			}
		};

		switch (g_cfg.io.move)
		{
		case move_handler::real:
		case move_handler::fake:
		{
			std::lock_guard lock(pad::g_pad_mutex);
			const auto handler = pad::get_pad_thread(true);
			if (!handler) break;

			for (u32 i = 0; i < CELL_GEM_MAX_NUM; i++)
			{
				const auto& pad = ::at32(handler->GetPads(), pad_num(i));
				const bool connected = pad && pad->is_connected() && !pad->is_copilot() && i < attribute.max_connect;
				const bool is_real_move = g_cfg.io.move != move_handler::real || (pad && pad->m_pad_handler == pad_handler::move);

				update_connection(i, connected && is_real_move);
			}
			break;
		}
		case move_handler::mouse:
		case move_handler::raw_mouse:
		{
			auto& handler = *ensure(g_fxo->try_get<MouseHandlerBase>());
			std::lock_guard mouse_lock(handler.mutex);

			// Pedal build: make sure the mouse handler is initialized. After loading a savestate of a game that only
			// set it up through cellGem (The Shoot), nothing else does it: the guns stayed "not connected" forever.
			if (attribute.max_connect)
			{
				handler.Init(std::min<u32>(attribute.max_connect, CELL_GEM_MAX_NUM));
			}

			const MouseInfo& info = handler.GetInfo();

			for (u32 i = 0; i < CELL_GEM_MAX_NUM; i++)
			{
				update_connection(i, i < attribute.max_connect && info.status[i] == CELL_MOUSE_STATUS_CONNECTED);
			}
			break;
		}
#ifdef HAVE_LIBEVDEV
		case move_handler::gun:
		{
			gun_thread& gun = *ensure(g_fxo->try_get<gun_thread>());
			std::scoped_lock lock(gun.handler.mutex);
			gun.num_devices = gun.handler.init() ? gun.handler.get_num_guns() : 0;

			for (u32 i = 0; i < CELL_GEM_MAX_NUM; i++)
			{
				update_connection(i, i < attribute.max_connect && i < gun.num_devices);
			}
			break;
		}
#endif
		case move_handler::null:
		{
			break;
		}
		}
	}

	void update_calibration_status()
	{
		for (u32 gem_num = 0; gem_num < CELL_GEM_MAX_NUM; gem_num++)
		{
			gem_controller& controller = controllers[gem_num];
			if (!controller.is_calibrating) continue;

			bool controller_calibrated = true;

			// Request controller calibration
			if (g_cfg.io.move == move_handler::real)
			{
				std::lock_guard pad_lock(pad::g_pad_mutex);
				const auto handler = pad::get_pad_thread();
				const auto& pad = ::at32(handler->GetPads(), pad_num(gem_num));
				if (pad && pad->m_pad_handler == pad_handler::move && !pad->is_copilot())
				{
					if (!pad->move_data.calibration_requested || !pad->move_data.calibration_succeeded)
					{
						pad->move_data.calibration_requested = true;
						controller_calibrated = false;
					}
				}
			}

			// The calibration takes ~0.5 seconds on real hardware
			if ((get_guest_system_time() - controller.calibration_start_us) < gem_controller::calibration_time_us) continue;

			if (!controller_calibrated)
			{
				cellGem.warning("Reached calibration timeout but ps move controller %d is still calibrating", gem_num);
			}

			controller.is_calibrating = false;
			controller.calibration_start_us = 0;
			controller.calibration_status_flags = CELL_GEM_FLAG_CALIBRATION_SUCCEEDED | CELL_GEM_FLAG_CALIBRATION_OCCURRED;
			controller.calibrated_magnetometer = true;
			controller.enabled_tracking = true;

			if (gem_v032_mouse())
			{
				controller.hue = 1;
				v032_status_flags = CELL_GEM_FLAG_CALIBRATION_SUCCEEDED | CELL_GEM_FLAG_CALIBRATION_OCCURRED;
			}

			// Reset controller calibration request
			if (g_cfg.io.move == move_handler::real)
			{
				std::lock_guard pad_lock(pad::g_pad_mutex);
				const auto handler = pad::get_pad_thread();
				const auto& pad = ::at32(handler->GetPads(), pad_num(gem_num));
				if (pad && pad->m_pad_handler == pad_handler::move && !pad->is_copilot())
				{
					pad->move_data.calibration_requested = false;
					pad->move_data.calibration_succeeded = false;
				}
			}
		}
	}

	void reset_controller(u32 gem_num)
	{
		if (gem_num >= CELL_GEM_MAX_NUM)
		{
			return;
		}

		gem_controller& controller = ::at32(controllers, gem_num);
		controller = {};
		controller.sphere_rgb = gem_color::get_default_color(gem_num);

		bool is_connected = false;

		switch (g_cfg.io.move)
		{
		case move_handler::real:
		{
			connected_controllers = 0;
			std::lock_guard lock(pad::g_pad_mutex);
			const auto handler = pad::get_pad_thread();
			for (u32 i = 0; i < std::min<u32>(attribute.max_connect, CELL_GEM_MAX_NUM); i++)
			{
				const auto& pad = ::at32(handler->GetPads(), pad_num(i));
				if (pad && pad->m_pad_handler == pad_handler::move && pad->is_connected() && !pad->is_copilot())
				{
					connected_controllers++;

					if (gem_num == i)
					{
						pad->move_data.magnetometer_enabled = controller.enabled_magnetometer;
						is_connected = true;
					}
				}
			}
			break;
		}
		case move_handler::fake:
		{
			connected_controllers = 0;
			std::lock_guard lock(pad::g_pad_mutex);
			const auto handler = pad::get_pad_thread();
			for (u32 i = 0; i < std::min<u32>(attribute.max_connect, CELL_GEM_MAX_NUM); i++)
			{
				const auto& pad = ::at32(handler->GetPads(), pad_num(i));
				if (pad && pad->is_connected() && !pad->is_copilot())
				{
					connected_controllers++;

					if (gem_num == i)
					{
						is_connected = true;
					}
				}
			}
			break;
		}
		case move_handler::mouse:
		case move_handler::raw_mouse:
		{
			auto& handler = *ensure(g_fxo->try_get<MouseHandlerBase>());
			std::lock_guard mouse_lock(handler.mutex);

			// Make sure that the mouse handler is initialized
			handler.Init(std::min<u32>(attribute.max_connect, CELL_GEM_MAX_NUM));

			const MouseInfo& info = handler.GetInfo();
			connected_controllers = std::min<u32>({ info.now_connect, attribute.max_connect, CELL_GEM_MAX_NUM });
			if (gem_num < connected_controllers)
			{
				is_connected = true;
			}
			break;
		}
#ifdef HAVE_LIBEVDEV
		case move_handler::gun:
		{
			gun_thread& gun = *ensure(g_fxo->try_get<gun_thread>());
			std::scoped_lock lock(gun.handler.mutex);
			gun.num_devices = gun.handler.init() ? gun.handler.get_num_guns() : 0;
			connected_controllers = std::min<u32>(std::min<u32>(attribute.max_connect, CELL_GEM_MAX_NUM), gun.num_devices);

			if (gem_num < connected_controllers)
			{
				is_connected = true;
			}
			break;
		}
#endif
		case move_handler::null:
			break;
		}

		// Assign status and port number
		if (is_connected)
		{
			controller.status = CELL_GEM_STATUS_READY;
			controller.port = port_num(gem_num);
		}
	}

	void paint_spheres(CellGemVideoConvertFormatEnum output_format, u32 width, u32 height, u8* video_data_out, u32 video_data_out_size);

	gem_config_data()
	{
		load_configs();
	};

	SAVESTATE_INIT_POS(16.1); // Depends on cellCamera

	void save(utils::serial& ar)
	{
		ar(state);

		if (!state)
		{
			return;
		}

		const s32 version = GET_OR_USE_SERIALIZATION_VERSION(ar.is_writing(), cellGem);

		ar(attribute, vc_attribute, runtime_status_flags, enable_pitch_correction, inertial_counter);

		for (gem_controller& c : controllers)
		{
			ar(c.status, c.ext_status, c.ext_id, c.port, c.enabled_magnetometer, c.calibrated_magnetometer, c.enabled_filtering, c.enabled_tracking, c.enabled_LED, c.hue_set, c.rumble);

			// We need to add padding because we used bitwise serialization in version 1
			if (version < 2)
			{
				ar.add_padding(&gem_controller::rumble, &gem_controller::sphere_rgb);
			}

			ar(c.sphere_rgb, c.hue, c.distance_mm, c.radius, c.radius_valid, c.is_calibrating);

			if (version < 2)
			{
				ar.add_padding(&gem_controller::is_calibrating, &gem_controller::calibration_start_us);
			}

			ar(c.calibration_start_us);

			if (ar.is_writing() || version >= 2)
			{
				ar(c.calibration_status_flags);
			}
		}

		ar(connected_controllers, updating, camera_frame, memory_ptr, start_timestamp_us);

		if (ar.is_writing() || version >= 3)
		{
			ar(video_conversion_in_progress, video_data_out_size);
		}
	}

	gem_config_data(utils::serial& ar)
	{
		save(ar);
		load_configs();

		// Pedal build: the v0.0.32 global status flags are not in the savestate - rebuild them from the saved per-gun flags
		v032_status_flags = 0;
		for (const auto& c : controllers)
		{
			v032_status_flags |= c.calibration_status_flags;
		}
	}

	static void load_configs()
	{
		if (!g_cfg_gem_real.load())
		{
			cellGem.notice("Could not load real gem config. Using defaults.");
		}

		if (!g_cfg_gem_fake.load())
		{
			cellGem.notice("Could not load fake gem config. Using defaults.");
		}

		if (!g_cfg_gem_mouse.load())
		{
			cellGem.notice("Could not load mouse gem config. Using defaults.");
		}

		cellGem.notice("Real gem config=%s", g_cfg_gem_real.to_string());
		cellGem.notice("Fake gem config=%s", g_cfg_gem_fake.to_string());
		cellGem.notice("Mouse gem config=%s", g_cfg_gem_mouse.to_string());
	}
};

extern std::pair<u32, u32> get_video_resolution(const CellCameraInfoEx& info);
extern u32 get_buffer_size_by_format(s32 format, s32 width, s32 height);

static inline s32 cellGemGetVideoConvertSize(s32 output_format)
{
	switch (output_format)
	{
	case CELL_GEM_RGBA_320x240: // RGBA output; 320*240*4-byte output buffer required
		return 320 * 240 * 4;
	case CELL_GEM_RGBA_640x480: // RGBA output; 640*480*4-byte output buffer required
		return 640 * 480 * 4;
	case CELL_GEM_YUV_640x480: // YUV output; 640*480+640*480+640*480-byte output buffer required (contiguous)
		return 640 * 480 + 640 * 480 + 640 * 480;
	case CELL_GEM_YUV422_640x480: // YUV output; 640*480+320*480+320*480-byte output buffer required (contiguous)
		return 640 * 480 + 320 * 480 + 320 * 480;
	case CELL_GEM_YUV411_640x480: // YUV411 output; 640*480+320*240+320*240-byte output buffer required (contiguous)
		return 640 * 480 + 320 * 240 + 320 * 240;
	case CELL_GEM_BAYER_RESTORED: // Bayer pattern output, 640x480, gamma and white balance applied, output buffer required
	case CELL_GEM_BAYER_RESTORED_RGGB: // Restored Bayer output, 2x2 pixels rearranged into 320x240 RG1G2B
	case CELL_GEM_BAYER_RESTORED_RASTERIZED: // Restored Bayer output, R,G1,G2,B rearranged into 4 contiguous 320x240 1-channel rasters
		return 640 * 480;
	case CELL_GEM_NO_VIDEO_OUTPUT: // Disable video output
		return 0;
	default:
		return -1;
	}
}

namespace gem
{
	struct gem_position
	{
	public:
		void set_position(f32 x, f32 y)
		{
			std::lock_guard lock(m_mutex);
			m_x = x;
			m_y = y;
		}
		void get_position(f32& x, f32& y)
		{
			std::lock_guard lock(m_mutex);
			x = m_x;
			y = m_y;
		}
	private:
		std::mutex m_mutex;
		f32 m_x = 0.0f;
		f32 m_y = 0.0f;
	};

	std::array<gem_position, CELL_GEM_MAX_NUM> positions {};

	struct YUV
	{
		u8 y = 0;
		u8 u = 0;
		u8 v = 0;

		YUV(u8 r, u8 g, u8 b)
			: y(Y(r, g, b))
			, u(U(r, g, b))
			, v(V(r, g, b))
		{
		}

		YUV(const u8 rgb[3])
		{
			const u8 r = rgb[0];
			const u8 g = rgb[1];
			const u8 b = rgb[2];
			y = Y(r, g, b);
			u = U(r, g, b);
			v = V(r, g, b);
		}

		static inline u8 Y(u8 r, u8 g, u8 b) { return static_cast<u8>(std::clamp(0.299f * r + 0.587f * g + 0.114f * b, 0.0f, 255.0f)); }
		static inline u8 U(u8 r, u8 g, u8 b) { return static_cast<u8>(std::clamp(-0.169f * r - 0.331f * g + 0.499f * b + 128, 0.0f, 255.0f)); }
		static inline u8 V(u8 r, u8 g, u8 b) { return static_cast<u8>(std::clamp(0.499f * r - 0.460f * g - 0.040f * b + 128, 0.0f, 255.0f)); }
	};

	template <bool use_gain>
	static inline void debayer_raw8_impl(const u8* src, u8* dst, u8 alpha, f32 gain_r, f32 gain_g, f32 gain_b)
	{
		constexpr u32 in_pitch = 640;
		constexpr u32 out_pitch = 640 * 4;

		// Hamilton–Adams demosaicing
		for (s32 y = 0; y < 480; y++)
		{
			const bool is_even_y = (y % 2) == 0;
			const u8* srcc = src + y * in_pitch;
			const u8* srcu = src + std::max(0, y - 1) * in_pitch;
			const u8* srcd = src + std::min(480 - 1, y + 1) * in_pitch;

			u8* dst0 = dst + y * out_pitch;

			// Split loops (roughly twice the performance by removing one condition)
			if (is_even_y)
			{
				for (s32 x = 0; x < 640; x++, dst0 += 4)
				{
					const bool is_even_x = (x % 2) == 0;
					const int xl = std::max(0, x - 1);
					const int xr = std::min(640 - 1, x + 1);

					u8 r, b, g;

					if (is_even_x)
					{
						// Blue pixel
						const u8 up = srcu[x];
						const u8 down = srcd[x];
						const u8 left = srcc[xl];
						const u8 right = srcc[xr];
						const int dh = std::abs(int(left) - int(right));
						const int dv = std::abs(int(up)   - int(down));

						r = (srcu[xl] + srcu[xr] + srcd[xl] + srcd[xr]) / 4;
						if (dh < dv)
							g = (left + right) / 2;
						else if (dv < dh)
							g = (up + down) / 2;
						else
							g = (up + down + left + right) / 4;
						b = srcc[x];
					}
					else
					{
						// Green (on blue row)
						r = (srcu[x] + srcd[x]) / 2;
						g = srcc[x];
						b = (srcc[xl] + srcc[xr]) / 2;
					}

					if constexpr (use_gain)
					{
						dst0[0] = static_cast<u8>(std::clamp(r * gain_r, 0.0f, 255.0f));
						dst0[1] = static_cast<u8>(std::clamp(g * gain_g, 0.0f, 255.0f));
						dst0[2] = static_cast<u8>(std::clamp(b * gain_b, 0.0f, 255.0f));
					}
					else
					{
						dst0[0] = r;
						dst0[1] = g;
						dst0[2] = b;
					}
					dst0[3] = alpha;
				}
			}
			else
			{
				for (s32 x = 0; x < 640; x++, dst0 += 4)
				{
					const bool is_even_x = (x % 2) == 0;
					const int xl = std::max(0, x - 1);
					const int xr = std::min(640 - 1, x + 1);

					u8 r, b, g;

					if (is_even_x)
					{
						// Green (on red row)
						r = (srcc[xl] + srcc[xr]) / 2;
						g = srcc[x];
						b = (srcu[x] + srcd[x]) / 2;
					}
					else
					{
						// Red pixel
						const u8 up = srcu[x];
						const u8 down = srcd[x];
						const u8 left = srcc[xl];
						const u8 right = srcc[xr];
						const int dh = std::abs(int(left) - int(right));
						const int dv = std::abs(int(up)   - int(down));

						r = srcc[x];
						if (dh < dv)
							g = (left + right) / 2;
						else if (dv < dh)
							g = (up + down) / 2;
						else
							g = (up + down + left + right) / 4;
						b = (srcu[xl] + srcu[xr] + srcd[xl] + srcd[xr]) / 4;
					}

					if constexpr (use_gain)
					{
						dst0[0] = static_cast<u8>(std::clamp(r * gain_r, 0.0f, 255.0f));
						dst0[1] = static_cast<u8>(std::clamp(g * gain_g, 0.0f, 255.0f));
						dst0[2] = static_cast<u8>(std::clamp(b * gain_b, 0.0f, 255.0f));
					}
					else
					{
						dst0[0] = r;
						dst0[1] = g;
						dst0[2] = b;
					}
					dst0[3] = alpha;
				}
			}
		}
	}

	static void debayer_raw8(const u8* src, u8* dst, u8 alpha, f32 gain_r, f32 gain_g, f32 gain_b)
	{
		if (gain_r != 1.0f || gain_g != 1.0f || gain_b != 1.0f)
			debayer_raw8_impl<true>(src, dst, alpha, gain_r, gain_g, gain_b);
		else
			debayer_raw8_impl<false>(src, dst, alpha, gain_r, gain_g, gain_b);
	}

	template <bool use_gain>
	static inline void debayer_raw8_downscale_impl(const u8* src, u8* dst, u8 alpha, f32 gain_r, f32 gain_g, f32 gain_b)
	{
		constexpr u32 in_pitch = 640;
		constexpr u32 out_pitch = 320 * 4;

		// Simple debayer
		for (s32 y = 0; y < 240; y++)
		{
			const u8* src0 = src + y * 2 * in_pitch;
			const u8* src1 = src0 + in_pitch;

			u8* dst0 = dst + y * out_pitch;

			for (s32 x = 0; x < 320; x++, dst0 += 4, src0 += 2, src1 += 2)
			{
				const u8 b  = src0[0];
				const u8 g0 = src0[1];
				const u8 g1 = src1[0];
				const u8 r  = src1[1];
				const u8 g  = (g0 + g1) >> 1;

				if constexpr (use_gain)
				{
					dst0[0] = static_cast<u8>(std::clamp(r * gain_r, 0.0f, 255.0f));
					dst0[1] = static_cast<u8>(std::clamp(g * gain_g, 0.0f, 255.0f));
					dst0[2] = static_cast<u8>(std::clamp(b * gain_b, 0.0f, 255.0f));
				}
				else
				{
					dst0[0] = r;
					dst0[1] = g;
					dst0[2] = b;
				}
				dst0[3] = alpha;
			}
		}
	}

	static void debayer_raw8_downscale(const u8* src, u8* dst, u8 alpha, f32 gain_r, f32 gain_g, f32 gain_b)
	{
		if (gain_r != 1.0f || gain_g != 1.0f || gain_b != 1.0f)
			debayer_raw8_downscale_impl<true>(src, dst, alpha, gain_r, gain_g, gain_b);
		else
			debayer_raw8_downscale_impl<false>(src, dst, alpha, gain_r, gain_g, gain_b);
	}

	bool convert_image_format(CellCameraFormat input_format, const CellGemVideoConvertAttribute& vc,
	                          const std::vector<u8>& video_data_in, u32 width, u32 height,
	                          u8* video_data_out, u32 video_data_out_size, u8* buffer_memory,
	                          std::string_view caller)
	{
		if (vc.output_format != CELL_GEM_NO_VIDEO_OUTPUT && !video_data_out)
		{
			return false;
		}

		const u32 required_in_size = get_buffer_size_by_format(static_cast<s32>(input_format), width, height);
		const s32 required_out_size = cellGemGetVideoConvertSize(vc.output_format);

		if (video_data_in.size() != required_in_size)
		{
			cellGem.error("convert: in_size mismatch: required=%d, actual=%d (called from %s)", required_in_size, video_data_in.size(), caller);
			return false;
		}

		if (required_out_size < 0 || video_data_out_size != static_cast<u32>(required_out_size))
		{
			cellGem.error("convert: out_size unknown: required=%d, actual=%d, format %d (called from %s)", required_out_size, video_data_out_size, vc.output_format, caller);
			return false;
		}

		if (required_out_size == 0)
		{
			return false;
		}

		thread_local std::vector<u8> corrected_buffer;
		thread_local std::vector<u8> combined_buffer;
		thread_local std::vector<u8> conversion_buffer;

		const u8* src_data = video_data_in.data();
		const u8 alpha = vc.alpha;
		const f32 gain_r = vc.gain * vc.red_gain;
		const f32 gain_g = vc.gain * vc.green_gain;
		const f32 gain_b = vc.gain * vc.blue_gain;

		// Only RAW8 should be relevant for cellGem unless I'm mistaken
		if (input_format == CELL_CAMERA_RAW8)
		{
			// TODO: CELL_GEM_AUTO_WHITE_BALANCE
			// TODO: CELL_GEM_GAMMA_BOOST

			// Correct outliers
			if (vc.conversion_flags & CELL_GEM_FILTER_OUTLIER_PIXELS)
			{
				corrected_buffer.resize(width * height);

				for (u32 y = 0; y < height; y++)
				{
					const u8* src = src_data + y * 640;
					u8* dst = &corrected_buffer[y * 640];

					for (u32 x = 0; x < width; x++, src++)
					{
						// Let's just say these 2 are outliers
						if (const u8 val = *src; val > 0 && val < 255)
						{
							*dst++ = val;
							continue;
						}

						// Just take the 4 neighbours for now
						s32 sum = 0;
						if (y >= 2) sum += *(src - (2 * 640));
						if (x >= 2) sum += *(src - 2);
						if (x < 638) sum += *(src + 2);
						if (y < 478) sum += *(src + (2 * 640));

						*dst++ = sum / 4; // Ignore count. It will only be less than 4 on the edges
					}
				}

				src_data = corrected_buffer.data();
			}

			// Combine with previous frame
			if (buffer_memory && (vc.conversion_flags & CELL_GEM_COMBINE_PREVIOUS_INPUT_FRAME))
			{
				combined_buffer.resize(width * height);

				for (u32 i = 0; i < combined_buffer.size(); i++)
				{
					const u8 val = src_data[i];
					u8& old = buffer_memory[i];
					combined_buffer[i] = (old + val) / 2;
					old = val;
				}

				src_data = combined_buffer.data();
			}

			switch (vc.output_format)
			{
			case CELL_GEM_YUV_640x480:
			case CELL_GEM_YUV422_640x480:
			case CELL_GEM_YUV411_640x480:
			{
				// Let's debayer the image first for YUV formats
				conversion_buffer.resize(cellGemGetVideoConvertSize(CELL_GEM_RGBA_640x480));

				debayer_raw8(src_data, conversion_buffer.data(), alpha, gain_r, gain_g, gain_b);

				src_data = conversion_buffer.data();
				input_format = CELL_CAMERA_RGBA;
				width = 640;
				height = 480;
				break;
			}
			case CELL_GEM_BAYER_RESTORED:
			case CELL_GEM_BAYER_RESTORED_RGGB:
			case CELL_GEM_BAYER_RESTORED_RASTERIZED:
			{
				// Let's apply gain
				if (gain_r != 1.0f || gain_g != 1.0f || gain_b != 1.0f)
				{
					conversion_buffer.resize(cellGemGetVideoConvertSize(CELL_GEM_RGBA_640x480));

					const f32 bggr_gains[2][2] = {{gain_b, gain_g}, {gain_g, gain_r}};
					const u8* src = src_data;
					u8* dst = conversion_buffer.data();

					for (u32 y = 0; y < 480; y++)
					{
						const f32* gains = bggr_gains[y % 2];

						for (u32 x = 0; x < 640; x++)
						{
							*dst++ = static_cast<u8>(std::clamp(*src++ * gains[x % 2], 0.0f, 255.0f));
						}
					}

					src_data = conversion_buffer.data();
				}
				break;
			}
			default:
				break;
			}
		}

		switch (vc.output_format)
		{
		case CELL_GEM_RGBA_640x480: // RGBA output; 640*480*4-byte output buffer required
		{
			switch (input_format)
			{
			case CELL_CAMERA_RAW8:
			{
				debayer_raw8(src_data, video_data_out, alpha, gain_r, gain_g, gain_b);
				break;
			}
			case CELL_CAMERA_RGBA:
			{
				std::memcpy(video_data_out, src_data, std::min<usz>(required_in_size, required_out_size));
				break;
			}
			default:
			{
				cellGem.error("Unimplemented: Converting %s to %s (called from %s)", input_format, vc.output_format, caller);
				std::memcpy(video_data_out, src_data, std::min<usz>(required_in_size, required_out_size));
				return false;
			}
			}
			break;
		}
		case CELL_GEM_BAYER_RESTORED: // Bayer pattern output, 640x480, gamma and white balance applied, output buffer required
		{
			if (input_format == CELL_CAMERA_RAW8)
			{
				std::memcpy(video_data_out, src_data, std::min<usz>(required_in_size, required_out_size));
			}
			else
			{
				cellGem.error("Unimplemented: Converting %s to %s (called from %s)", input_format, vc.output_format, caller);
				return false;
			}
			break;
		}
		case CELL_GEM_YUV_640x480: // YUV output; 640*480+640*480+640*480-byte output buffer required (contiguous)
		{
			// YUV 4:4:4 planar. 1 value each per pixel
			const u32 yuv_pitch = width;

			u8* dst_y = video_data_out;
			u8* dst_u = dst_y + yuv_pitch * height;
			u8* dst_v = dst_u + yuv_pitch * height;

			switch (input_format)
			{
			case CELL_CAMERA_RAW8:
			{
				fmt::throw_exception("Unreachable: should already be debayered");
				break;
			}
			case CELL_CAMERA_RGBA:
			{
				const u32 in_pitch = width * 4;

				for (u32 y = 0; y < height; y++)
				{
					const u8* src = src_data + y * in_pitch;

					for (u32 x = 0; x < width; x++, src += 4)
					{
						// Convert RGBA to YUV
						const YUV yuv = YUV(src);

						*dst_y++ = yuv.y;
						*dst_u++ = yuv.u;
						*dst_v++ = yuv.v;
					}
				}
				break;
			}
			default:
			{
				cellGem.error("Unimplemented: Converting %s to %s (called from %s)", input_format, vc.output_format, caller);
				std::memcpy(video_data_out, src_data, std::min<usz>(required_in_size, required_out_size));
				return false;
			}
			}
			break;
		}
		case CELL_GEM_YUV422_640x480: // YUV output; 640*480+320*480+320*480-byte output buffer required (contiguous)
		{
			// YUV 4:2:2 planar. 1 Y value per pixel, 1 U/V value per 2 horizontal pixels
			const u32 y_pitch = width;
			const u32 uv_pitch = width / 2;

			u8* dst_y = video_data_out;
			u8* dst_u = dst_y + y_pitch * height;
			u8* dst_v = dst_u + uv_pitch * height;

			switch (input_format)
			{
			case CELL_CAMERA_RAW8:
			{
				fmt::throw_exception("Unreachable: should already be debayered");
				break;
			}
			case CELL_CAMERA_RGBA:
			{
				const u32 in_pitch = width * 4;

				for (u32 y = 0; y < height; y++)
				{
					const u8* src = src_data + y * in_pitch;

					for (u32 x = 0; x < width - 1; x += 2, src += 8, dst_y += 2)
					{
						// Convert RGBA to YUV
						const YUV yuv_0 = YUV(src);
						const YUV yuv_1 = YUV(src + 4);

						dst_y[0] = yuv_0.y;
						dst_y[1] = yuv_1.y;

						// Average U/V from 2 horizontal pixels
						*dst_u++ = (yuv_0.u + yuv_1.u) / 2;
						*dst_v++ = (yuv_0.v + yuv_1.v) / 2;
					}
				}
				break;
			}
			default:
			{
				cellGem.error("Unimplemented: Converting %s to %s (called from %s)", input_format, vc.output_format, caller);
				std::memcpy(video_data_out, src_data, std::min<usz>(required_in_size, required_out_size));
				return false;
			}
			}
			break;
		}
		case CELL_GEM_YUV411_640x480: // YUV411 output; 640*480+320*240+320*240-byte output buffer required (contiguous)
		{
			// YUV 4:1:1 planar. 1 Y value per pixel, 1 U/V value per 2x2 pixel block
			u8* dst_y = video_data_out;
			u8* dst_u = dst_y + 640 * 480;
			u8* dst_v = dst_u + 320 * 240;

			switch (input_format)
			{
			case CELL_CAMERA_RAW8:
			{
				fmt::throw_exception("Unreachable: should already be debayered");
				break;
			}
			case CELL_CAMERA_RGBA:
			{
				const u32 in_pitch = width * 4;

				// 2 rows at a time to get a 2x2 pixel block
				for (u32 y = 0; y < height - 1; y += 2)
				{
					const u8* src = src_data + y * in_pitch;
					const u8* src2 = src + in_pitch;
					u8* dst_y1 = dst_y + y * 640;
					u8* dst_y2 = dst_y1 + 640;

					for (u32 x = 0; x < width - 1; x += 2, src += 8, src2 += 8, dst_y1 += 2, dst_y2 += 2)
					{
						// Convert RGBA to YUV
						const YUV yuv_0 = YUV(src);
						const YUV yuv_1 = YUV(src + 4);
						const YUV yuv_2 = YUV(src2);
						const YUV yuv_3 = YUV(src2 + 4);

						dst_y1[0] = yuv_0.y;
						dst_y1[1] = yuv_1.y;
						dst_y2[0] = yuv_2.y;
						dst_y2[1] = yuv_3.y;

						// Average U/V from 2x2 pixel block
						*dst_u++ = (yuv_0.u + yuv_1.u + yuv_2.u + yuv_3.u) / 4;
						*dst_v++ = (yuv_0.v + yuv_1.v + yuv_2.v + yuv_3.v) / 4;
					}
				}
				break;
			}
			default:
			{
				cellGem.error("Unimplemented: Converting %s to %s (called from %s)", input_format, vc.output_format, caller);
				std::memcpy(video_data_out, src_data, std::min<usz>(required_in_size, required_out_size));
				return false;
			}
			}
			break;
		}
		case CELL_GEM_RGBA_320x240: // RGBA output; 320*240*4-byte output buffer required
		{
			switch (input_format)
			{
			case CELL_CAMERA_RAW8:
			{
				debayer_raw8_downscale(src_data, video_data_out, alpha, gain_r, gain_g, gain_b);
				break;
			}
			case CELL_CAMERA_RGBA:
			{
				const u32 in_pitch = width * 4;
				const u32 out_pitch = width * 4 / 2;

				for (u32 y = 0; y < height / 2; y++)
				{
					const u8* src = src_data + y * 2 * in_pitch;
					u8* dst = video_data_out + y * out_pitch;

					for (u32 x = 0; x < width / 2; x++, src += 4 * 2, dst += 4)
					{
						std::memcpy(dst, src, 4);
					}
				}
				break;
			}
			default:
			{
				cellGem.error("Unimplemented: Converting %s to %s (called from %s)", input_format, vc.output_format, caller);
				std::memcpy(video_data_out, src_data, std::min<usz>(required_in_size, required_out_size));
				return false;
			}
			}
			break;
		}
		case CELL_GEM_BAYER_RESTORED_RGGB: // Restored Bayer output, 2x2 pixels rearranged into 320x240 RG1G2B
		{
			if (input_format == CELL_CAMERA_RAW8)
			{
				const u32 dst_w = std::min(320u, width / 2);
				const u32 dst_h = std::min(240u, height / 2);
				const u32 in_pitch = width;
				constexpr u32 out_pitch = 320 * 4;

				for (u32 y = 0; y < dst_h; y++)
				{
					const u8* src0 = src_data + y * 2 * in_pitch;
					const u8* src1 = src0 + in_pitch;

					u8* dst = video_data_out + y * out_pitch;

					for (u32 x = 0; x < dst_w; x++, src0 += 2, src1 += 2, dst += 4)
					{
						const u8 b  = src0[0];
						const u8 g0 = src0[1];
						const u8 g1 = src1[0];
						const u8 r  = src1[1];

						dst[0] = r;
						dst[1] = g0;
						dst[2] = g1;
						dst[3] = b;
					}
				}
			}
			else
			{
				cellGem.error("Unimplemented: Converting %s to %s (called from %s)", input_format, vc.output_format, caller);
				std::memcpy(video_data_out, src_data, std::min<usz>(required_in_size, required_out_size));
				return false;
			}
			break;
		}
		case CELL_GEM_BAYER_RESTORED_RASTERIZED: // Restored Bayer output, R,G1,G2,B rearranged into 4 contiguous 320x240 1-channel rasters
		{
			if (input_format == CELL_CAMERA_RAW8)
			{
				const u32 dst_w = std::min(320u, width / 2);
				const u32 dst_h = std::min(240u, height / 2);
				const u32 in_pitch = width;
				constexpr u32 out_plane = 320 * 240;
				constexpr u32 out_pitch = 320;

				u8* dst_plane_r = video_data_out;
				u8* dst_plane_g1 = video_data_out + out_plane;
				u8* dst_plane_g2 = video_data_out + out_plane * 2;
				u8* dst_plane_b = video_data_out + out_plane * 3;

				for (u32 y = 0; y < dst_h; y++)
				{
					const u8* src0 = src_data + y * 2 * in_pitch;
					const u8* src1 = src0 + in_pitch;

					u8* dst_r = dst_plane_r + y * out_pitch;
					u8* dst_g1 = dst_plane_g1 + y * out_pitch;
					u8* dst_g2 = dst_plane_g2 + y * out_pitch;
					u8* dst_b = dst_plane_b + y * out_pitch;

					for (u32 x = 0; x < dst_w; x++, src0 += 2, src1 += 2)
					{
						const u8 b  = src0[0];
						const u8 g0 = src0[1];
						const u8 g1 = src1[0];
						const u8 r  = src1[1];

						dst_r[x] = r;
						dst_g1[x] = g0;
						dst_g2[x] = g1;
						dst_b[x] = b;
					}
				}
			}
			else
			{
				cellGem.error("Unimplemented: Converting %s to %s (called from %s)", input_format, vc.output_format, caller);
				std::memcpy(video_data_out, src_data, std::min<usz>(required_in_size, required_out_size));
				return false;
			}
			break;
		}
		case CELL_GEM_NO_VIDEO_OUTPUT: // Disable video output
		{
			cellGem.trace("Ignoring frame conversion for CELL_GEM_NO_VIDEO_OUTPUT (called from %s)", caller);
			break;
		}
		default:
		{
			cellGem.error("Trying to convert %s to %s (called from %s)", input_format, vc.output_format, caller);
			return false;
		}
		}

		return true;
	}
}

void gem_config_data::paint_spheres(CellGemVideoConvertFormatEnum output_format, u32 width, u32 height, u8* video_data_out, u32 video_data_out_size)
{
	if (!width || !height || !video_data_out || !video_data_out_size)
	{
		return;
	}

	struct sphere_information
	{
		f32 radius = 0.0f;
		s16 x = 0;
		s16 y = 0;
		u8 r = 0;
		u8 g = 0;
		u8 b = 0;
	};

	std::vector<sphere_information> sphere_info;
	{
		reader_lock lock(mtx);

		for (u32 gem_num = 0; gem_num < CELL_GEM_MAX_NUM; gem_num++)
		{
			const gem_config_data::gem_controller& controller = controllers[gem_num];
			if (!controller.radius_valid || controller.radius <= 0.0f) continue;

			f32 x, y;
			::at32(gem::positions, gem_num).get_position(x, y);

			const u8 r = static_cast<u8>(std::clamp(controller.sphere_rgb.r * 255.0f, 0.0f, 255.0f));
			const u8 g = static_cast<u8>(std::clamp(controller.sphere_rgb.g * 255.0f, 0.0f, 255.0f));
			const u8 b = static_cast<u8>(std::clamp(controller.sphere_rgb.b * 255.0f, 0.0f, 255.0f));

			sphere_info.push_back({ controller.radius, static_cast<s16>(x), static_cast<s16>(y), r, g, b });
		}
	}

	switch (output_format)
	{
	case CELL_GEM_RGBA_640x480: // RGBA output; 640*480*4-byte output buffer required
	{
		cellGem.trace("Painting spheres for CELL_GEM_RGBA_640x480");

		const u32 out_pitch = width * 4;

		for (const sphere_information& info : sphere_info)
		{
			const s32 x_begin = std::max(0, static_cast<s32>(std::floor(info.x - info.radius)));
			const s32 x_end = std::min<s32>(width, static_cast<s32>(std::ceil(info.x + info.radius)));
			const s32 y_begin = std::max(0, static_cast<s32>(std::floor(info.y - info.radius)));
			const s32 y_end = std::min<s32>(height, static_cast<s32>(std::ceil(info.y + info.radius)));

			for (s32 y = y_begin; y < y_end; y++)
			{
				u8* dst = video_data_out + y * out_pitch + x_begin * 4;

				for (s32 x = x_begin; x < x_end; x++, dst += 4)
				{
					const f32 distance = static_cast<f32>(std::sqrt(std::pow(info.x - x, 2) + std::pow(info.y - y, 2)));
					if (distance > info.radius) continue;

					dst[0] = info.r;
					dst[1] = info.g;
					dst[2] = info.b;
					dst[3] = 255;
				}
			}
		}

		break;
	}
	case CELL_GEM_BAYER_RESTORED: // Bayer pattern output, 640x480, gamma and white balance applied, output buffer required
	case CELL_GEM_RGBA_320x240: // RGBA output; 320*240*4-byte output buffer required
	case CELL_GEM_YUV_640x480: // YUV output; 640*480+640*480+640*480-byte output buffer required (contiguous)
	case CELL_GEM_YUV422_640x480: // YUV output; 640*480+320*480+320*480-byte output buffer required (contiguous)
	case CELL_GEM_YUV411_640x480: // YUV411 output; 640*480+320*240+320*240-byte output buffer required (contiguous)
	case CELL_GEM_BAYER_RESTORED_RGGB: // Restored Bayer output, 2x2 pixels rearranged into 320x240 RG1G2B
	case CELL_GEM_BAYER_RESTORED_RASTERIZED: // Restored Bayer output, R,G1,G2,B rearranged into 4 contiguous 320x240 1-channel rasters
	{
		cellGem.trace("Unimplemented: painting spheres for %s", output_format);
		break;
	}
	case CELL_GEM_NO_VIDEO_OUTPUT: // Disable video output
	{
		cellGem.trace("Ignoring painting spheres for CELL_GEM_NO_VIDEO_OUTPUT");
		break;
	}
	default:
	{
		cellGem.trace("Ignoring painting spheres for %d", static_cast<u32>(output_format));
		break;
	}
	}
}

void gem_config_data::operator()()
{
	cellGem.notice("Starting thread");

	u64 last_update_us = 0;

	// Handle initial state after loading a savestate
	if (state && video_conversion_in_progress)
	{
		// Wait for cellGemConvertVideoFinish. The initial savestate loading may take a while.
		m_done = 2; // Use special value 2 for this case
		thread_ctrl::wait_on(m_done, 2, 5'000'000);

		// Just mark this conversion as complete (there's no real downside to this, except for a black image)
		video_conversion_in_progress = false;
		done();
	}

	while (thread_ctrl::state() != thread_state::aborting && !Emu.IsStopped())
	{
		u64 timeout = umax;

		if (state && !video_conversion_in_progress)
		{
			constexpr u64 update_timeout_us = 100'000; // Update controllers at 10Hz
			const u64 now_us = get_system_time();
			const u64 elapsed_us = now_us - last_update_us;

			if (elapsed_us < update_timeout_us)
			{
				timeout = update_timeout_us - elapsed_us;
			}
			else
			{
				timeout = update_timeout_us;
				last_update_us = now_us;

				std::scoped_lock lock(mtx);
				// Pedal build: v0.0.32 only refreshed mouse/raw mouse connections when the game called cellGemGetInfo.
				// Polling here makes savestates see the guns as unplugged for a moment (TC4 then pauses and clears its controllers).
				if (!gem_v032_mouse())
				{
					update_connections();
				}
				update_calibration_status();
			}
		}

		if (!m_wake_up)
		{
			thread_ctrl::wait_on(m_wake_up, 0, timeout);
		}

		m_wake_up = 0;

		if (!video_conversion_in_progress)
		{
			continue;
		}

		if (thread_ctrl::state() == thread_state::aborting || Emu.IsStopped())
		{
			return;
		}

		CellGemVideoConvertAttribute vc;
		{
			std::scoped_lock lock(mtx);
			vc = vc_attribute;
		}

		switch (g_cfg.io.camera)
		{
#ifdef HAVE_SDL3
		case camera_handler::sdl:
#endif
		case camera_handler::qt:
			break;
		case camera_handler::fake:
		case camera_handler::null:
			video_conversion_in_progress = false;
			done();
			continue;
		}

		const auto& shared_data = g_fxo->get<gem_camera_shared>();

		if (gem::convert_image_format(shared_data.format, vc, video_data_in, shared_data.width, shared_data.height,
			vc.video_data_out ? vc.video_data_out.get_ptr() : nullptr, video_data_out_size,
			vc.buffer_memory ? vc.buffer_memory.get_ptr() : nullptr, "cellGem"))
		{
			cellGem.trace("Converted video frame of format %s to %s", shared_data.format.load(), vc.output_format.get());

			if (g_cfg.io.paint_move_spheres)
			{
				paint_spheres(vc.output_format, shared_data.width, shared_data.height, vc.video_data_out ? vc.video_data_out.get_ptr() : nullptr, video_data_out_size);
			}
		}

		video_conversion_in_progress = false;
		done();
	}
}

using gem_config = named_thread<gem_config_data>;

class gem_tracker
{
public:
	gem_tracker()
	{
	}

	bool is_busy()
	{
		return m_busy;
	}

	void wake_up_tracker()
	{
		m_wake_up_tracker.release(1);
		m_wake_up_tracker.notify_one();
	}

	void tracker_done()
	{
		m_tracker_done.release(1);
		m_tracker_done.notify_one();
	}

	bool wait_for_tracker_result(ppu_thread& ppu)
	{
		if (g_cfg.io.move != move_handler::real)
		{
			m_tracker_done = 0;
			return true;
		}

		while (!m_tracker_done && !ppu.is_stopped())
		{
			thread_ctrl::wait_on(m_tracker_done, 0);
		}

		if (ppu.is_stopped())
		{
			ppu.state += cpu_flag::again;
			return false;
		}

		m_tracker_done = 0;
		return true;
	}

	bool set_image(vm::cptr<void> buf)
	{
		if (!buf)
			return false;

		auto& g_camera = g_fxo->get<camera_thread>();
		std::lock_guard lock(g_camera.mutex);
		m_camera_info = g_camera.info;

		if (m_camera_info.buffer.addr() != buf.addr() && m_camera_info.pbuf[0].addr() != buf.addr() && m_camera_info.pbuf[1].addr() != buf.addr())
		{
			// The game passes its own buffer. Let's just ignore this and log a warning. We use our own camera buffer for tracking for now anyway.
			cellGem.warning("gem_tracker: unexpected image address: addr=0x%x, expected one of: 0x%x, 0x%x, 0x%x", buf.addr(), m_camera_info.buffer.addr(), m_camera_info.pbuf[0].addr(), m_camera_info.pbuf[1].addr());
		}

		if (!m_camera_info.buffer)
		{
			cellGem.error("gem_tracker: camer buffer not set");
			return false;
		}

		// Copy image data for further processing

		const auto& [width, height] = get_video_resolution(m_camera_info);
		const u32 expected_size = get_buffer_size_by_format(m_camera_info.format, width, height);

		if (!m_camera_info.bytesize || static_cast<u32>(m_camera_info.bytesize) != expected_size)
		{
			cellGem.error("gem_tracker: unexpected image size: size=%d, expected=%d", m_camera_info.bytesize, expected_size);
			return false;
		}

		m_tracker.set_image_data(m_camera_info.buffer.get_ptr(), m_camera_info.bytesize, m_camera_info.width, m_camera_info.height, m_camera_info.format);
		m_framenumber++; // using framenumber instead of timestamp since the timestamp could be identical
		return true;
	}

	bool hue_is_trackable(u32 hue)
	{
		if (g_cfg.io.move != move_handler::real)
		{
			return true; // potentially true if less than 20 pixels have the hue
		}

		return hue < m_hues.size() && m_hues[hue] < 20; // potentially true if less than 20 pixels have the hue
	}

	ps_move_info get_info(u32 gem_num)
	{
		std::lock_guard lock(mutex);
		return ::at32(m_info, gem_num);
	}

	void operator()()
	{
		if (g_cfg.io.move != move_handler::real)
		{
			return;
		}

		if (!g_cfg_move.load())
		{
			cellGem.notice("Could not load PS Move config. Using defaults.");
		}

		auto& gem = g_fxo->get<gem_config>();
		u64 last_framenumber = 0;

		while (thread_ctrl::state() != thread_state::aborting)
		{
			// Check if we have a new frame
			if (!m_wake_up_tracker)
			{
				thread_ctrl::wait_on(m_wake_up_tracker, 0);
				m_wake_up_tracker.release(0);

				if (thread_ctrl::state() == thread_state::aborting)
				{
					break;
				}
			}

			if (std::exchange(last_framenumber, m_framenumber.load()) == last_framenumber)
			{
				cellGem.warning("Tracker woke up without new frame. Skipping processing (framenumber=%d)", last_framenumber);
				tracker_done();
				continue;
			}

			m_busy.release(true);

			// Update PS Move LED colors
			{
				std::lock_guard lock(pad::g_pad_mutex);
				const auto handler = pad::get_pad_thread();
				auto& handlers = handler->get_handlers();
				if (auto it = handlers.find(pad_handler::move); it != handlers.end() && it->second)
				{
					for (auto& binding : it->second->bindings())
					{
						if (!binding.device) continue;

						// last 4 out of 7 ports (6,5,4,3). index starts at 0
						const s32 gem_num = std::abs(binding.device->player_id - CELL_PAD_MAX_PORT_NUM) - 1;

						if (gem_num < 0 || gem_num >= CELL_GEM_MAX_NUM) continue;

						binding.device->color_override_active = true;

						if (g_cfg.io.allow_move_hue_set_by_game)
						{
							const auto& controller = gem.controllers[gem_num];
							binding.device->color_override.r = static_cast<u8>(std::clamp(controller.sphere_rgb.r * 255.0f, 0.0f, 255.0f));
							binding.device->color_override.g = static_cast<u8>(std::clamp(controller.sphere_rgb.g * 255.0f, 0.0f, 255.0f));
							binding.device->color_override.b = static_cast<u8>(std::clamp(controller.sphere_rgb.b * 255.0f, 0.0f, 255.0f));
						}
						else
						{
							const cfg_ps_move* config = ::at32(g_cfg_move.move, gem_num);
							binding.device->color_override.r = config->r.get();
							binding.device->color_override.g = config->g.get();
							binding.device->color_override.b = config->b.get();
						}
					}
				}
			}

			// Update tracker config
			for (u32 gem_num = 0; gem_num < CELL_GEM_MAX_NUM; gem_num++)
			{
				const auto& controller = gem.controllers[gem_num];
				const cfg_ps_move* config = g_cfg_move.move[gem_num];

				m_tracker.set_active(gem_num, controller.enabled_tracking && controller.status == CELL_GEM_STATUS_READY);
				// The calibrated hue is only valid for the configured color
				const u32 configured_hue = config->hue_calibrated ? config->calibrated_hue.get() : config->hue.get();

				m_tracker.set_hue(gem_num, g_cfg.io.allow_move_hue_set_by_game ? controller.hue : configured_hue);
				m_tracker.set_hue_threshold(gem_num, static_cast<u8>(config->hue_threshold.get()));
				m_tracker.set_saturation_threshold(gem_num, static_cast<u8>(config->saturation_threshold.get()));
				m_tracker.set_value_threshold(gem_num, static_cast<u8>(config->value_threshold.get()));
			}

			m_tracker.set_min_radius(static_cast<f32>(g_cfg_move.min_radius) / 100.0f);
			m_tracker.set_max_radius(static_cast<f32>(g_cfg_move.max_radius) / 100.0f);

			// Process camera image
			m_tracker.process_image();

			// Update cellGem with results
			{
				std::lock_guard lock(mutex);
				m_hues = m_tracker.hues();
				m_info = m_tracker.info();

				for (u32 gem_num = 0; gem_num < CELL_GEM_MAX_NUM; gem_num++)
				{
					const ps_move_info& info = m_info[gem_num];
					auto& controller = gem.controllers[gem_num];
					controller.radius_valid = info.valid;

					if (info.valid)
					{
						// Only set new radius and distance if the radius is valid
						controller.radius = info.radius;
						controller.distance_mm = info.distance_mm;
					}
				}
			}

			// Notify that we are finished with this frame
			tracker_done();

			m_busy.release(false);
		}
	}

	static constexpr auto thread_name = "GemUpdateThread"sv;

	shared_mutex mutex;

	gem_tracker& operator=(thread_state) noexcept
	{
		wake_up_tracker();
		return *this;
	}

private:
	atomic_t<u32> m_wake_up_tracker = 0;
	atomic_t<u32> m_tracker_done = 0;
	atomic_t<u64> m_framenumber = 0;
	atomic_t<bool> m_busy = false;
	ps_move_tracker<false> m_tracker{};
	CellCameraInfoEx m_camera_info{};
	std::array<u32, 360> m_hues{};
	std::array<ps_move_info, CELL_GEM_MAX_NUM> m_info{};
};

/**
 * \brief Verifies that a Move controller id is valid
 * \param gem_num Move controler ID to verify
 * \return True if the ID is valid, false otherwise
 */
static bool check_gem_num(u32 gem_num)
{
	return gem_num < CELL_GEM_MAX_NUM;
}

static inline void draw_overlay_cursor(u32 gem_num, const gem_config::gem_controller&, f32 x_pos, f32 y_pos)
{
	const s16 x = static_cast<s16>(x_pos * rsx::overlays::overlay::virtual_width);
	const s16 y = static_cast<s16>(y_pos * rsx::overlays::overlay::virtual_height);

	// Note: We shouldn't use sphere_rgb here. The game will set it to black in many cases.
	const gem_config_data::gem_color& rgb = gem_config_data::gem_color::get_default_color(gem_num);
	const color4f color = { rgb.r, rgb.g, rgb.b, 0.85f };

	rsx::overlays::set_cursor(rsx::overlays::cursor_offset::cell_gem + gem_num, x, y, color, 2'000'000, false);
}

static inline void pos_to_gem_image_state(u32 gem_num, gem_config::gem_controller& controller, vm::ptr<CellGemImageState>& gem_image_state, f32 x_pos, f32 y_pos)
{
	const auto& shared_data = g_fxo->get<gem_camera_shared>();

	// Move the cursor out of the screen if we're at the screen border (Time Crisis 4 needs this)
	if (x_pos <= 0) x_pos -= 0.1f; else if (x_pos >= 1.0f) x_pos += 0.1f;
	if (y_pos <= 0) y_pos -= 0.1f; else if (y_pos >= 1.0f) y_pos += 0.1f;

	// Image coordinates in pixels
	const f32 image_x = x_pos * shared_data.width;
	const f32 image_y = y_pos * shared_data.height;

	// Centered image coordinates in pixels
	const f32 centered_x = image_x - (shared_data.width / 2.f);
	const f32 centered_y = (shared_data.height / 2.f) - image_y; // Image coordinates increase downwards, so we have to invert this

	// Camera coordinates in mm (centered, so it's the same as world coordinates)
	const f32 mmPerPixel = controller.radius <= 0.0f ? 0.0f : (CELL_GEM_SPHERE_RADIUS_MM / controller.radius);
	const f32 camera_x = centered_x * mmPerPixel;
	const f32 camera_y = centered_y * mmPerPixel;

	// Image coordinates in pixels
	gem_image_state->u = image_x;
	gem_image_state->v = image_y;

	// Projected camera coordinates in mm
	gem_image_state->projectionx = camera_x / controller.distance_mm;
	gem_image_state->projectiony = camera_y / controller.distance_mm;

	// Update visibility for fake handlers
	if (g_cfg.io.move != move_handler::real)
	{
		// Let's say the sphere is not visible if the position is at the edge of the screen
		controller.radius_valid = x_pos > 0.0f && x_pos < 1.0f && y_pos > 0.0f && y_pos < 1.0f;
	}

	if (g_cfg.io.show_move_cursor)
	{
		draw_overlay_cursor(gem_num, controller, x_pos, y_pos);
	}

	if (g_cfg.io.paint_move_spheres)
	{
		::at32(gem::positions, gem_num).set_position(image_x, image_y);
	}
}

// MLGB lightgun fork: The Shoot gesture switches (per player, from profiles/The Shoot.yml "Raw Mouse")
static bool shoot_raw_flag(u32 player, cfg::_bool raw_mouse_config::* flag)
{
	if (g_cfg.io.move != move_handler::raw_mouse || Emu.GetTitleID() != "BCUS98222" || player >= g_cfg_raw_mouse.players.size()) return false;
	reader_lock config_lock(g_cfg_raw_mouse.m_mutex);
	return (g_cfg_raw_mouse.players[player].get()->*flag).get();
}

static bool shoot_spin_enabled(u32 player) { return shoot_raw_flag(player, &raw_mouse_config::shoot_spin_360); }
static bool shoot_point_down_enabled(u32 player) { return shoot_raw_flag(player, &raw_mouse_config::shoot_point_down); }
static bool shoot_point_up_enabled(u32 player) { return shoot_raw_flag(player, &raw_mouse_config::shoot_point_up_offscreen); }
// Pedal build: bowling / bat only inside their Challenges (pedal_outputs finds the Challenge's own Lua values)
static bool shoot_bowl_enabled(u32 player) { return pedal_outputs::shoot_mode_bowling() && shoot_raw_flag(player, &raw_mouse_config::shoot_bowl); }
static bool shoot_bat_enabled(u32 player) { return pedal_outputs::shoot_mode_bat() && shoot_raw_flag(player, &raw_mouse_config::shoot_bat); }
static bool shoot_offscreen_middle_enabled(u32 player)
{
	return shoot_point_down_enabled(player) && shoot_raw_flag(player, &raw_mouse_config::shoot_offscreen_middle);
}

// MLGB lightgun fork: per-game aim scale (Move Aim Scale Horizontal / Vertical), raw mouse only
static std::pair<f32, f32> mlgb_aim(u32 mouse_no, f32 x, f32 y)
{
	if (g_cfg.io.move != move_handler::raw_mouse || mouse_no >= g_cfg_raw_mouse.players.size()) return {x, y};
	reader_lock config_lock(g_cfg_raw_mouse.m_mutex);
	const auto& config = g_cfg_raw_mouse.players[mouse_no];
	return {lightgun_scale_axis(x, static_cast<float>(config->move_aim_scale_h.get())),
		lightgun_scale_axis(y, static_cast<float>(config->move_aim_scale_v.get()))};
}

static inline void pos_to_gem_state(u32 gem_num, gem_config::gem_controller& controller, vm::ptr<CellGemState>& gem_state, f32 x_pos, f32 y_pos, ps_move_data& move_data)
{
	const auto& shared_data = g_fxo->get<gem_camera_shared>();

	// Move the cursor out of the screen if we're at the screen border (Time Crisis 4 needs this)
	if (x_pos <= 0.0f) x_pos -= 0.1f; else if (x_pos >= 1.0f) x_pos += 0.1f;
	if (y_pos <= 0.0f) y_pos -= 0.1f; else if (y_pos >= 1.0f) y_pos += 0.1f;

	// Image coordinates in pixels
	const f32 image_x = x_pos * shared_data.width;
	const f32 image_y = y_pos * shared_data.height;

	// Half of the camera image
	const f32 half_width = shared_data.width / 2.f;
	const f32 half_height = shared_data.height / 2.f;

	// Centered image coordinates in pixels
	const f32 centered_x = image_x - half_width;
	const f32 centered_y = half_height - image_y; // Image coordinates increase downwards, so we have to invert this

	// Camera coordinates in mm (centered, so it's the same as world coordinates)
	const f32 mmPerPixel = controller.radius <= 0.0f ? 0.0f : (CELL_GEM_SPHERE_RADIUS_MM / controller.radius);
	const f32 camera_x = centered_x * mmPerPixel;
	const f32 camera_y = centered_y * mmPerPixel;

	// World coordinates in mm
	gem_state->pos[0] = camera_x;
	gem_state->pos[1] = camera_y;
	gem_state->pos[2] = controller.distance_mm;
	gem_state->pos[3] = 0.f;

	// Calculate orientation
	ps_move_data::vect<4> quat = move_data.quaternion;

	if (g_cfg.io.move != move_handler::real && !(g_cfg.io.move == move_handler::fake && move_data.orientation_enabled))
	{
		const f32 max_angle_per_side_h = g_cfg.io.fake_move_rotation_cone_h / 2.0f;
		const f32 max_angle_per_side_v = g_cfg.io.fake_move_rotation_cone_v / 2.0f;
		f32 roll = -PadHandlerBase::degree_to_rad((image_y - half_height) / half_height * max_angle_per_side_v); // This is actually the pitch
		f32 pitch = -PadHandlerBase::degree_to_rad((image_x - half_width) / half_width * max_angle_per_side_h); // This is actually the yaw
		// Pedal build: The Shoot
		if (gem_mlgb_title() && gem_num < CELL_GEM_MAX_NUM)
		{
			// Deep Strike: aim sets the bat's height (magnified), the gun's stick (arrow keys = Select) swings it across
			if (shoot_bat_enabled(gem_num) && gem_num < g_cfg_raw_mouse.players.size() && max_angle_per_side_v > 0.f)
			{
				f32 turn = 180.f, height = 90.f;
				u32 ms = 250;
				{
					reader_lock config_lock(g_cfg_raw_mouse.m_mutex);
					const auto& c = g_cfg_raw_mouse.players[gem_num];
					turn = static_cast<f32>(c->shoot_bat_turn.get());
					height = static_cast<f32>(c->shoot_bat_height.get());
					ms = static_cast<u32>(c->shoot_bat_time.get());
				}
				auto& bat = g_fxo->get<gem_config>().mouse_bat[gem_num];
				bat.swing_us = u64{ms} * 1000;
				const bool was_swinging = bat.swinging;
				const f32 a = bat.update(get_guest_system_time(), (gem_state->pad.digitalbuttons & CELL_GEM_CTRL_SELECT) != 0);
				if (bat.swinging && !was_swinging) cellGem.notice("The Shoot P%d: bat swing (%d degrees, %d ms)", gem_num + 1, static_cast<s32>(turn), ms);
				roll *= height / (2.f * max_angle_per_side_v);
				pitch += PadHandlerBase::degree_to_rad(a * turn / 2.f);
			}
			// bowling: the ball is held at lane height wherever the gun aims (aim only steers left / right)
			if (g_fxo->get<gem_config>().mouse_bowl[gem_num].shaping) roll = 0.f;
		}
		const f32 yaw = PadHandlerBase::degree_to_rad(0.0f);
		const f32 cr = std::cos(roll * 0.5f);
		const f32 sr = std::sin(roll * 0.5f);
		const f32 cp = std::cos(pitch * 0.5f);
		const f32 sp = std::sin(pitch * 0.5f);
		const f32 cy = std::cos(yaw * 0.5f);
		const f32 sy = std::sin(yaw * 0.5f);

		quat.x() = sr * cp * cy - cr * sp * sy;
		quat.y() = cr * sp * cy + sr * cp * sy;
		quat.z() = cr * cp * sy - sr * sp * cy;
		quat.w() = cr * cp * cy + sr * sp * sy;
	}

	// MLGB lightgun fork: supply a coherent inertial stream for supported mouse-backed Move games.
	// A changing quaternion with an always-zero gyro describes conflicting motion.
	if (gem_mlgb_title())
	{
		auto& gem = g_fxo->get<gem_config>();
		std::lock_guard history_lock(gem.mouse_history_mutex);
		const u64 now = get_guest_system_time();
		auto& spin = gem.mouse_spin[gem_num];
		const bool was_spinning = spin.active;
		auto& middle = gem.mouse_middle_gesture[gem_num];
		middle.update((gem_state->pad.digitalbuttons & CELL_GEM_CTRL_SQUARE) != 0,
			x_pos <= 0.f || x_pos >= 1.f || y_pos <= 0.f || y_pos >= 1.f, shoot_offscreen_middle_enabled(gem_num));
		// Pedal build: right click pressed after holding the trigger half a second is the bowling button, not a spin
		auto& bowl = gem.mouse_bowl[gem_num];
		const bool bowl_on = shoot_bowl_enabled(gem_num);
		if (bowl_on) bowl.update_spin_button(now, (gem_state->pad.digitalbuttons & CELL_GEM_CTRL_SQUARE) != 0, (gem_state->pad.digitalbuttons & CELL_GEM_CTRL_T) != 0);
		// Pedal build: Deep Strike - the bat follows the aim (set above); right click does not spin there
		const bool bat_on = shoot_bat_enabled(gem_num);
		const float angle = spin.update(now, shoot_spin_enabled(gem_num) && !bat_on && middle.held && !middle.point_down && !(bowl_on && bowl.spin_button_latched));
		auto& origin = gem.mouse_spin_origin[gem_num];
		if (spin.active && !was_spinning)
		{
			origin = ps_move_data::vect<3>({gem_state->pos[0], gem_state->pos[1], gem_state->pos[2]});
			cellGem.notice("The Shoot P%d: started Showtime turn (yaw + 600 mm orbit)", gem_num + 1);
		}
		if (spin.active)
		{
			// The game's roundhouse detector checks horizontal facing directions
			// AND sphere travel. Simulate the arm orbit during a body turn.
			const float sy = std::sin(angle * .5f), cy = std::cos(angle * .5f);
			const auto base = quat;
			quat.x() = cy*base.x() + sy*base.z();
			quat.y() = cy*base.y() + sy*base.w();
			quat.z() = cy*base.z() - sy*base.x();
			quat.w() = cy*base.w() - sy*base.y();
			const auto offset = lightgun_spin::orbit(angle);
			gem_state->pos[0] = origin.x() + offset[0];
			gem_state->pos[1] = origin.y();
			gem_state->pos[2] = origin.z() + offset[1];
		}

		const bool point_down = shoot_point_down_enabled(gem_num) &&
			((gem_state->pad.digitalbuttons & CELL_GEM_CTRL_TRIANGLE) || middle.point_down);
		const bool offscreen = x_pos <= 0.f || x_pos >= 1.f || y_pos <= 0.f || y_pos >= 1.f;
		const int vertical = lightgun_vertical_direction(spin.active, point_down, offscreen, shoot_point_up_enabled(gem_num));

		// Pedal build: bowling swing (hold Circle + the trigger, let go of the trigger). Only with Circle held,
		// so no other mode is affected (Circle is only "back" on menus).
		const bool bowl_was_active = bowl.active;
		const bool bowl_button = bowl_on && ((gem_state->pad.digitalbuttons & CELL_GEM_CTRL_CIRCLE) != 0 || bowl.spin_button_latched);
		const float bowl_p = bowl_on && !spin.active ?
			bowl.update(now, (gem_state->pad.digitalbuttons & CELL_GEM_CTRL_T) != 0, bowl_button) : -1.f;
		float pitch_deg = 90.f * vertical;
		if (bowl_on)
		{
			// Circle is the bowling button in The Shoot, not a game button (the menus use Triangle for back)
			gem_state->pad.digitalbuttons = static_cast<u16>(+gem_state->pad.digitalbuttons & ~CELL_GEM_CTRL_CIRCLE);
		}
		// the swing, from the profile (tunable without a new build)
		struct bowl_params { f32 low, back, fwd, up, p0, p1; u32 ms; f32 steer; u32 follow_ms; };
		const bowl_params bp = [&]() -> bowl_params
		{
			if (!bowl_on || gem_num >= g_cfg_raw_mouse.players.size()) return {350.f, 250.f, 900.f, 550.f, -90.f, 30.f, 450, 800.f, 150};
			reader_lock config_lock(g_cfg_raw_mouse.m_mutex);
			const auto& c = g_cfg_raw_mouse.players[gem_num];
			return {static_cast<f32>(c->shoot_bowl_low.get()), static_cast<f32>(c->shoot_bowl_back.get()),
				static_cast<f32>(c->shoot_bowl_forward.get()), static_cast<f32>(c->shoot_bowl_up.get()),
				static_cast<f32>(c->shoot_bowl_pitch_start.get()), static_cast<f32>(c->shoot_bowl_pitch_end.get()),
				static_cast<u32>(c->shoot_bowl_time.get()), static_cast<f32>(c->shoot_bowl_steer.get()),
				static_cast<u32>(c->shoot_bowl_follow.get())};
		}();
		bowl.duration_us = u64{bp.ms} * 1000;
		bowl.follow_us = u64{bp.follow_ms} * 1000;
		bowl.shaping = false;
		if (bowl_on && bowl.t_released_for_regrab(now))
		{
			// held low first, then a fresh T press
			gem_state->pad.digitalbuttons = static_cast<u16>(+gem_state->pad.digitalbuttons & ~CELL_GEM_CTRL_T);
			gem_state->pad.analog_T = 0;
		}
		if ((bowl_button || bowl.armed) && bowl_p < 0.f)
		{
			// holding the ball: arm hanging down, pointing down
			pitch_deg = bp.p0;
			bowl.shaping = true;
			bowl.dy = -bp.low;
			bowl.dz = bp.back;
			gem_state->pos[1] = bowl.dy;   // lane height, not where the gun aims
			gem_state->pos[2] += bowl.dz;
		}
		if (bowl_p >= 0.f)
		{
			auto& bowl_origin = gem.mouse_bowl_origin[gem_num];
			if (!bowl_was_active)
			{
				bowl_origin = ps_move_data::vect<3>({gem_state->pos[0], 0.f, gem_state->pos[2]});
				cellGem.notice("The Shoot P%d: bowling swing (%d ms, forward %d mm, up %d mm, pitch %d to %d)", gem_num + 1,
					bp.ms, static_cast<s32>(bp.fwd), static_cast<s32>(bp.up), static_cast<s32>(bp.p0), static_cast<s32>(bp.p1));
			}
			const float e = lightgun_bowl::ease(bowl_p);
			// T stays held until the arm has swung through; then the release, while the arm keeps going
			if (bowl_p < 1.f)
			{
				gem_state->pad.digitalbuttons = static_cast<u16>(+gem_state->pad.digitalbuttons | CELL_GEM_CTRL_T);
				gem_state->pad.analog_T = 255;
			}
			// arm: from hanging down and behind (pointing down) forward along the lane, speeding up to the release.
			// Sideways: where the gun aims (left / right of the screen centre) is where the ball goes.
			// the arm keeps turning through the release - the game takes the ball speed from how fast it turns
			const float e_pitch = std::min(e, 1.f);
			pitch_deg = bp.p0 + (bp.p1 - bp.p0) * e;
			bowl.shaping = true;
			bowl.dy = -bp.low + bp.up * e_pitch;
			bowl.dz = bp.back - bp.fwd * e;
			const f32 side = (std::clamp(x_pos, 0.f, 1.f) - 0.5f) * bp.steer;
			gem_state->pos[0] = bowl_origin.x() + side * e;
			gem_state->pos[1] = bowl_origin.y() + bowl.dy;
			gem_state->pos[2] = bowl_origin.z() + bowl.dz;
		}

		if (pitch_deg != 0.f)
		{
			// Off-screen prepares the upward pose; the player's real trigger supplies
			// the new-press edge required by the game's shoot-in-air gesture.
			const float half = pitch_deg * 3.14159265358979f / 360.f;
			const float c = std::cos(half);
			const float s = std::sin(half);
			const auto base = quat;
			quat.x() = c*base.x() + s*base.w();
			quat.y() = c*base.y() - s*base.z();
			quat.z() = c*base.z() + s*base.y();
			quat.w() = c*base.w() - s*base.x();
		}

		if (shoot_point_down_enabled(gem_num)) gem_state->pad.digitalbuttons &= ~CELL_GEM_CTRL_TRIANGLE;
		// The virtual Square slot carries this gesture rather than a game button.
		if (shoot_spin_enabled(gem_num)) gem_state->pad.digitalbuttons &= ~CELL_GEM_CTRL_SQUARE;
		// the stick (arrow keys) is the bat swing, never the game's Select
		if (shoot_raw_flag(gem_num, &raw_mouse_config::shoot_bat)) gem_state->pad.digitalbuttons &= ~CELL_GEM_CTRL_SELECT;
		u64& previous_time = gem.mouse_orientation_time[gem_num];
		auto& previous = gem.mouse_previous_orientation[gem_num];
		if (previous_time == 0 || now < previous_time || now - previous_time >= 1000)
		{
			move_data.gyro = {};
			if (previous_time != 0 && now > previous_time && now - previous_time <= 100'000)
			{
				// conjugate(previous) * current: rotation in the controller's frame.
				const f32 dx = previous.w()*quat.x() - previous.x()*quat.w() - previous.y()*quat.z() + previous.z()*quat.y();
				const f32 dy = previous.w()*quat.y() + previous.x()*quat.z() - previous.y()*quat.w() - previous.z()*quat.x();
				const f32 dz = previous.w()*quat.z() - previous.x()*quat.y() + previous.y()*quat.x() - previous.z()*quat.w();
				const f32 dw = previous.w()*quat.w() + previous.x()*quat.x() + previous.y()*quat.y() + previous.z()*quat.z();
				const f32 length = std::sqrt(dx*dx + dy*dy + dz*dz);
				if (length > 0.000001f)
				{
					const f32 scale = 2.0f * std::atan2(length, std::abs(dw)) / length * (1'000'000.0f / (now - previous_time)) * (dw < 0.0f ? -1.0f : 1.0f);
					move_data.gyro = ps_move_data::vect<3>({dx*scale, dy*scale, dz*scale});
				}
			}
			previous = quat;
			previous_time = now;
		}
		const ps_move_data::vect<4> inverse({-quat.x(), -quat.y(), -quat.z(), quat.w()});
		move_data.accelerometer = ps_move_data::rotate_vector(inverse, ps_move_data::vect<3>({0.f, 0.f, 1.f}));
		move_data.angvel_world = ps_move_data::rotate_vector(quat, move_data.gyro);
		for (u32 i = 0; i < 3; i++) gem_state->angvel[i] = move_data.angvel_world[i];
	}

	gem_state->quat[0] = quat.x();
	gem_state->quat[1] = quat.y();
	gem_state->quat[2] = quat.z();
	gem_state->quat[3] = quat.w();

	// Calculate handle position based on our world coordinate and the current orientation
	constexpr ps_move_data::vect<3> offset_local_mm({0.f, 0.f, 45.f}); // handle is ~45 mm below sphere
	const ps_move_data::vect<3> offset_world = ps_move_data::rotate_vector(quat, offset_local_mm);

	gem_state->handle_pos[0] = gem_state->pos[0] - offset_world.x(); // Flip x offset
	gem_state->handle_pos[1] = gem_state->pos[1] - offset_world.y(); // Flip y offset
	gem_state->handle_pos[2] = gem_state->pos[2] + offset_world.z();
	gem_state->handle_pos[3] = 0.f;

	// Calculate velocity
	if constexpr (!ps_move_data::use_imu_for_velocity)
	{
		move_data.update_velocity(shared_data.frame_timestamp_us, gem_state->pos);

		for (u32 i = 0; i < 3; i++)
		{
			gem_state->vel[i] = move_data.vel_world[i];
			gem_state->accel[i] = move_data.accel_world[i];

			// TODO: maybe this also needs to be adjusted depending on the orientation
			gem_state->handle_vel[i] = gem_state->vel[i];
			gem_state->handle_accel[i] = gem_state->accel[i];
		}
	}

	// Update visibility for fake handlers
	if (g_cfg.io.move != move_handler::real)
	{
		// Let's say the sphere is not visible if the position is at the edge of the screen
		controller.radius_valid = x_pos > 0.0f && x_pos < 1.0f && y_pos > 0.0f && y_pos < 1.0f;
	}
	// Pedal build: the camera keeps seeing the sphere while a bowling swing is held / played (the gun may point at the floor)
	if (gem_mlgb_title() && gem_num < CELL_GEM_MAX_NUM && (g_fxo->get<gem_config>().mouse_bowl[gem_num].shaping || shoot_bat_enabled(gem_num)))
	{
		controller.radius_valid = true;
	}

	if (g_cfg.io.show_move_cursor)
	{
		draw_overlay_cursor(gem_num, controller, x_pos, y_pos);
	}

	if (g_cfg.io.paint_move_spheres)
	{
		::at32(gem::positions, gem_num).set_position(image_x, image_y);
	}
}

extern bool is_input_allowed();

/**
 * \brief Maps Move controller data (digital buttons, and analog Trigger data) to DS3 pad input.
 *        Unavoidably buttons conflict with DS3 mappings, which is problematic for some games.
 * \param gem_num gem index to use
 * \param digital_buttons Bitmask filled with CELL_GEM_CTRL_* values
 * \param analog_t Analog value of Move's Trigger.
 * \return true on success, false if controller is disconnected
 */
static void ds3_input_to_pad(const u32 gem_num, be_t<u16>& digital_buttons, be_t<u16>& analog_t)
{
	digital_buttons = 0;
	analog_t = 0;

	if (!is_input_allowed() || input::g_pads_intercepted) // Let's intercept the PS Move just like a pad
	{
		return;
	}

	std::lock_guard lock(pad::g_pad_mutex);

	const auto handler = pad::get_pad_thread();
	const auto& pad = ::at32(handler->GetPads(), pad_num(gem_num));

	if (!pad->is_connected() || pad->is_copilot())
	{
		return;
	}

	const auto handle_input = [&](const emulated_pad_config<gem_btn>::input_value& value, bool& /*abort*/)
	{
		if (!value.pressed)
			return;

		switch (value.btn)
		{
		case gem_btn::start:
			digital_buttons |= CELL_GEM_CTRL_START;
			break;
		case gem_btn::select:
			digital_buttons |= CELL_GEM_CTRL_SELECT;
			break;
		case gem_btn::square:
			digital_buttons |= CELL_GEM_CTRL_SQUARE;
			break;
		case gem_btn::cross:
			digital_buttons |= CELL_GEM_CTRL_CROSS;
			break;
		case gem_btn::circle:
			digital_buttons |= CELL_GEM_CTRL_CIRCLE;
			break;
		case gem_btn::triangle:
			digital_buttons |= CELL_GEM_CTRL_TRIANGLE;
			break;
		case gem_btn::move:
			digital_buttons |= CELL_GEM_CTRL_MOVE;
			break;
		case gem_btn::t:
			digital_buttons |= CELL_GEM_CTRL_T;
			analog_t = std::max<u16>(analog_t, value.value);
			break;
		default:
			break;
		}
	};

	if (g_cfg.io.move == move_handler::real)
	{
		::at32(g_cfg_gem_real.players, gem_num)->handle_input(pad, true, handle_input);
	}
	else
	{
		::at32(g_cfg_gem_fake.players, gem_num)->handle_input(pad, true, handle_input);
	}
}

static inline void ds3_get_stick_values(u32 gem_num, const std::shared_ptr<Pad>& pad, f32& x_pos, f32& y_pos)
{
	x_pos = 0.0f;
	y_pos = 0.0f;

	const auto& cfg = ::at32(g_cfg_gem_fake.players, gem_num);
	cfg->handle_input(pad, true, [&](const auto& value, bool& /*abort*/)
	{
		if (!value.pressed)
			return;

		switch (value.btn)
		{
		case gem_btn::x_axis: x_pos = value.value / 255.0f; break;
		case gem_btn::y_axis: y_pos = value.value / 255.0f; break;
		default: break;
		}
	});
}

template <typename T>
static void ds3_pos_to_gem_state(u32 gem_num, gem_config::gem_controller& controller, T& gem_state)
{
	if (!gem_state || !is_input_allowed() || input::g_pads_intercepted) // Let's intercept the PS Move just like a pad
	{
		return;
	}

	std::lock_guard lock(pad::g_pad_mutex);

	const auto handler = pad::get_pad_thread();
	const auto& pad = ::at32(handler->GetPads(), pad_num(gem_num));

	if (!pad->is_connected() || pad->is_copilot())
	{
		return;
	}

	f32 ds3_pos_x, ds3_pos_y;
	ds3_get_stick_values(gem_num, pad, ds3_pos_x, ds3_pos_y);

	if constexpr (std::is_same_v<T, vm::ptr<CellGemState>>)
	{
		pos_to_gem_state(gem_num, controller, gem_state, ds3_pos_x, ds3_pos_y, pad->move_data);
	}
	else if constexpr (std::is_same_v<T, vm::ptr<CellGemImageState>>)
	{
		pos_to_gem_image_state(gem_num, controller, gem_state, ds3_pos_x, ds3_pos_y);
	}
}

template <typename T>
static void ps_move_pos_to_gem_state(u32 gem_num, gem_config::gem_controller& controller, T& gem_state)
{
	if (!gem_state || !is_input_allowed() || input::g_pads_intercepted) // Let's intercept the PS Move just like a pad
	{
		return;
	}

	std::lock_guard lock(pad::g_pad_mutex);

	const auto handler = pad::get_pad_thread();
	const auto& pad = ::at32(handler->GetPads(), pad_num(gem_num));

	if (pad->m_pad_handler != pad_handler::move || !pad->is_connected() || pad->is_copilot())
	{
		return;
	}

	auto& tracker = g_fxo->get<named_thread<gem_tracker>>();
	const ps_move_info info = tracker.get_info(gem_num);

	if constexpr (std::is_same_v<T, vm::ptr<CellGemState>>)
	{
		gem_state->temperature = pad->move_data.temperature;

		for (u32 i = 0; i < 3; i++)
		{
			if constexpr (ps_move_data::use_imu_for_velocity)
			{
				gem_state->vel[i] = pad->move_data.vel_world[i];
				gem_state->accel[i] = pad->move_data.accel_world[i];
			}
			gem_state->angvel[i] = pad->move_data.angvel_world[i];
			gem_state->angaccel[i] = pad->move_data.angaccel_world[i];
		}

		pos_to_gem_state(gem_num, controller, gem_state, info.x_pos, info.y_pos, pad->move_data);
	}
	else if constexpr (std::is_same_v<T, vm::ptr<CellGemImageState>>)
	{
		pos_to_gem_image_state(gem_num, controller, gem_state, info.x_pos, info.y_pos);
	}
}

static const std::unordered_map<gem_btn, u16> ext_btn_map =
{
	{ gem_btn::sharpshooter_firing_mode_1, button_flags::ss_firing_mode_1 },
	{ gem_btn::sharpshooter_firing_mode_2, button_flags::ss_firing_mode_2 },
	{ gem_btn::sharpshooter_firing_mode_3, button_flags::ss_firing_mode_3 },
	{ gem_btn::sharpshooter_trigger, button_flags::ss_trigger },
	{ gem_btn::sharpshooter_reload, button_flags::ss_reload },
	{ gem_btn::racing_wheel_d_pad_up, CELL_PAD_CTRL_UP },
	{ gem_btn::racing_wheel_d_pad_right, CELL_PAD_CTRL_RIGHT },
	{ gem_btn::racing_wheel_d_pad_down, CELL_PAD_CTRL_DOWN },
	{ gem_btn::racing_wheel_d_pad_left, CELL_PAD_CTRL_LEFT },
	{ gem_btn::racing_wheel_throttle, 0 },
	{ gem_btn::racing_wheel_l1, CELL_PAD_CTRL_L1 },
	{ gem_btn::racing_wheel_r1, CELL_PAD_CTRL_R1 },
	{ gem_btn::racing_wheel_l2, 0 },
	{ gem_btn::racing_wheel_r2, 0 },
	{ gem_btn::racing_wheel_paddle_l, button_flags::rw_paddle_l },
	{ gem_btn::racing_wheel_paddle_r, button_flags::rw_paddle_r },
	{ gem_btn::combo_sharpshooter_firing_mode_1, button_flags::ss_firing_mode_1 },
	{ gem_btn::combo_sharpshooter_firing_mode_2, button_flags::ss_firing_mode_2 },
	{ gem_btn::combo_sharpshooter_firing_mode_3, button_flags::ss_firing_mode_3 },
	{ gem_btn::combo_sharpshooter_trigger, button_flags::ss_trigger },
	{ gem_btn::combo_sharpshooter_reload, button_flags::ss_reload },
	{ gem_btn::combo_racing_wheel_d_pad_up, CELL_PAD_CTRL_UP },
	{ gem_btn::combo_racing_wheel_d_pad_right, CELL_PAD_CTRL_RIGHT },
	{ gem_btn::combo_racing_wheel_d_pad_down, CELL_PAD_CTRL_DOWN },
	{ gem_btn::combo_racing_wheel_d_pad_left, CELL_PAD_CTRL_LEFT },
	{ gem_btn::combo_racing_wheel_throttle, 0 },
	{ gem_btn::combo_racing_wheel_l1, CELL_PAD_CTRL_L1 },
	{ gem_btn::combo_racing_wheel_r1, CELL_PAD_CTRL_R1 },
	{ gem_btn::combo_racing_wheel_l2, 0 },
	{ gem_btn::combo_racing_wheel_r2, 0 },
	{ gem_btn::combo_racing_wheel_paddle_l, button_flags::rw_paddle_l },
	{ gem_btn::combo_racing_wheel_paddle_r, button_flags::rw_paddle_r },
};

static const std::unordered_map<gem_btn, u16> ext_btn_indices =
{
	{ gem_btn::racing_wheel_throttle, 0 },
	{ gem_btn::racing_wheel_l2, 1 },
	{ gem_btn::racing_wheel_r2, 2 },
	{ gem_btn::racing_wheel_paddle_l, 3 },
	{ gem_btn::racing_wheel_paddle_r, 3 },
	{ gem_btn::combo_racing_wheel_throttle, 0 },
	{ gem_btn::combo_racing_wheel_l2, 1 },
	{ gem_btn::combo_racing_wheel_r2, 2 },
	{ gem_btn::combo_racing_wheel_paddle_l, 3 },
	{ gem_btn::combo_racing_wheel_paddle_r, 3 },
};

static u32 ext_device_id(gem_ext_id id)
{
	switch (id)
	{
		case gem_ext_id::disconnected: return 0;
		case gem_ext_id::sharpshooter: return SHARP_SHOOTER_DEVICE_ID;
		case gem_ext_id::racing_wheel: return RACING_WHEEL_DEVICE_ID;
	}
	fmt::throw_exception("Mo ID found for id = %d", static_cast<s32>(id));
}

template <bool has_combo, bool is_combo>
static void input_to_ext(u32 external_device_id, CellGemExtPortData& ext, std::set<pad_button>& combos, const emulated_pad_config<gem_btn>::input_value& value)
{
	if (!value.pressed)
		return;

	const auto set_firing_mode = [&ext](gem_btn btn)
	{
		// The firing mode is exclusive
		ext.custom[0] &= ~button_flags::ss_firing_mode_mask;
		ext.custom[0] |= ::at32(ext_btn_map, btn);
	};

	if constexpr (has_combo && is_combo)
	{
		if (external_device_id == SHARP_SHOOTER_DEVICE_ID)
		{
			switch (value.btn)
			{
			case gem_btn::combo_sharpshooter_firing_mode_1:
			case gem_btn::combo_sharpshooter_firing_mode_2:
			case gem_btn::combo_sharpshooter_firing_mode_3:
				set_firing_mode(value.btn);
				combos.insert(value.pad_btn);
				break;
			case gem_btn::combo_sharpshooter_trigger:
			case gem_btn::combo_sharpshooter_reload:
				ext.custom[0] |= ::at32(ext_btn_map, value.btn);
				combos.insert(value.pad_btn);
				break;
			default:
				break;
			}
		}
		else if (external_device_id == RACING_WHEEL_DEVICE_ID)
		{
			switch (value.btn)
			{
			case gem_btn::combo_racing_wheel_throttle:
			case gem_btn::combo_racing_wheel_l2:
			case gem_btn::combo_racing_wheel_r2:
				ext.custom[::at32(ext_btn_indices, value.btn)] = static_cast<u8>(value.value);
				combos.insert(value.pad_btn);
				break;
			case gem_btn::combo_racing_wheel_paddle_l:
			case gem_btn::combo_racing_wheel_paddle_r:
				ext.custom[::at32(ext_btn_indices, value.btn)] |= ::at32(ext_btn_map, value.btn);
				combos.insert(value.pad_btn);
				break;
			case gem_btn::combo_racing_wheel_d_pad_up:
			case gem_btn::combo_racing_wheel_d_pad_right:
			case gem_btn::combo_racing_wheel_d_pad_down:
			case gem_btn::combo_racing_wheel_d_pad_left:
				ext.digital1 |= ::at32(ext_btn_map, value.btn);
				combos.insert(value.pad_btn);
				break;
			case gem_btn::combo_racing_wheel_l1:
			case gem_btn::combo_racing_wheel_r1:
				ext.digital2 |= ::at32(ext_btn_map, value.btn);
				combos.insert(value.pad_btn);
				break;
			default:
				break;
			}
		}
	}
	else
	{
		if constexpr (has_combo)
		{
			if (combos.contains(value.pad_btn))
			{
				return;
			}
		}

		if (external_device_id == SHARP_SHOOTER_DEVICE_ID)
		{
			switch (value.btn)
			{
			case gem_btn::sharpshooter_firing_mode_1:
			case gem_btn::sharpshooter_firing_mode_2:
			case gem_btn::sharpshooter_firing_mode_3:
				set_firing_mode(value.btn);
				break;
			case gem_btn::sharpshooter_trigger:
			case gem_btn::sharpshooter_reload:
				ext.custom[0] |= ::at32(ext_btn_map, value.btn);
				break;
			default:
				break;
			}
		}
		else if (external_device_id == RACING_WHEEL_DEVICE_ID)
		{
			switch (value.btn)
			{
			case gem_btn::racing_wheel_throttle:
			case gem_btn::racing_wheel_l2:
			case gem_btn::racing_wheel_r2:
				ext.custom[::at32(ext_btn_indices, value.btn)] = static_cast<u8>(value.value);
				break;
			case gem_btn::racing_wheel_paddle_l:
			case gem_btn::racing_wheel_paddle_r:
				ext.custom[::at32(ext_btn_indices, value.btn)] |= ::at32(ext_btn_map, value.btn);
				break;
			case gem_btn::racing_wheel_d_pad_up:
			case gem_btn::racing_wheel_d_pad_right:
			case gem_btn::racing_wheel_d_pad_down:
			case gem_btn::racing_wheel_d_pad_left:
				ext.digital1 |= ::at32(ext_btn_map, value.btn);
				break;
			case gem_btn::racing_wheel_l1:
			case gem_btn::racing_wheel_r1:
				ext.digital2 |= ::at32(ext_btn_map, value.btn);
				break;
			default:
				break;
			}
		}
	}
}

static void real_input_to_ext(u32 gem_num, gem_config::gem_controller& controller, CellGemExtPortData& ext)
{
	std::lock_guard lock(pad::g_pad_mutex);

	const auto handler = pad::get_pad_thread();
	const auto& pad = ::at32(handler->GetPads(), pad_num(gem_num));

	if (!pad->is_connected() || pad->is_copilot())
	{
		controller.ext_status = 0;
		controller.ext_id = 0;
		return;
	}

	ps_move_data& move_data = pad->move_data;

	controller.ext_status = move_data.external_device_connected ? CELL_GEM_EXT_CONNECTED : 0; // TODO: | CELL_GEM_EXT_EXT0 | CELL_GEM_EXT_EXT1
	controller.ext_id = move_data.external_device_connected ? move_data.external_device_id : 0;

	if (!move_data.external_device_connected)
	{
		return;
	}

	// TODO:
	// ext.analog_left_x
	// ext.analog_left_y
	// ext.analog_right_x
	// ext.analog_right_y
	// ext.digital1
	// ext.digital2

	ext.status = controller.ext_status;
	std::memcpy(ext.custom, move_data.external_device_data.data(), 5);
}

static void fake_input_to_ext(u32 gem_num, gem_config::gem_controller& controller, CellGemExtPortData& ext)
{
	std::lock_guard lock(pad::g_pad_mutex);

	const auto handler = pad::get_pad_thread();
	const auto& pad = ::at32(handler->GetPads(), pad_num(gem_num));

	if (!pad->is_connected() || pad->is_copilot())
	{
		controller.ext_status = 0;
		controller.ext_id = 0;
		return;
	}

	ps_move_data& move_data = pad->move_data;
	const auto& cfg = ::at32(g_cfg_gem_fake.players, gem_num);

	move_data.external_device_id = ext_device_id(cfg->external_device);
	move_data.external_device_connected = move_data.external_device_id != 0;
	move_data.external_device_data = {};

	controller.ext_status = move_data.external_device_connected ? CELL_GEM_EXT_CONNECTED : 0; // TODO: | CELL_GEM_EXT_EXT0 | CELL_GEM_EXT_EXT1
	controller.ext_id = move_data.external_device_connected ? move_data.external_device_id : 0;

	if (!move_data.external_device_connected)
	{
		return;
	}

	// Restore firing mode
	ext.custom[0] = controller.firing_mode;

	cfg->handle_input(pad, true, [&move_data, &ext](const auto& value, bool& /*abort*/)
	{
		static std::set<pad_button> s_combos = {};
		input_to_ext<false, false>(move_data.external_device_id, ext, s_combos, value);
	});

	ext.status = controller.ext_status;

	// Save firing mode
	controller.firing_mode = ext.custom[0] & button_flags::ss_firing_mode_mask;
}

static void mouse_input_to_ext(u32 mouse_no, gem_config::gem_controller& controller, CellGemExtPortData& ext)
{
	auto& handler = g_fxo->get<MouseHandlerBase>();

	std::scoped_lock lock(handler.mutex);

	// Make sure that the mouse handler is initialized
	handler.Init(std::min<u32>(g_fxo->get<gem_config>().attribute.max_connect, CELL_GEM_MAX_NUM));

	if (mouse_no >= handler.GetMice().size())
	{
		controller.ext_status = 0;
		controller.ext_id = 0;
		return;
	}

	if (gem_v032_mouse())
	{
		// v0.0.32: no external device
		controller.ext_status = 0;
		controller.ext_id = 0;
		return;
	}

	const Mouse& mouse_data = ::at32(handler.GetMice(), mouse_no);
	const auto& cfg = ::at32(g_cfg_gem_mouse.players, mouse_no);

	const u32 external_device_id = ext_device_id(cfg->external_device);
	const bool external_device_connected = external_device_id != 0;

	controller.ext_status = external_device_connected ? CELL_GEM_EXT_CONNECTED : 0; // TODO: | CELL_GEM_EXT_EXT0 | CELL_GEM_EXT_EXT1
	controller.ext_id = external_device_connected ? external_device_id : 0;

	if (!external_device_connected)
	{
		return;
	}

	// Restore firing mode
	ext.custom[0] = controller.firing_mode;

	bool combo_active = false;
	std::set<pad_button> combos;

	// Check combo button first
	cfg->handle_input(mouse_data, [&combo_active](const auto& value, bool& abort)
	{
		if (value.pressed && value.btn == gem_btn::combo)
		{
			combo_active = true;
			abort = true;
		}
	});

	// Check combos
	if (combo_active)
	{
		cfg->handle_input(mouse_data, [external_device_id, &ext, &combos](const auto& value, bool& /*abort*/)
		{
			input_to_ext<true, true>(external_device_id, ext, combos, value);
		});
	}

	// Check normal buttons
	cfg->handle_input(mouse_data, [external_device_id, &ext, &combos](const auto& value, bool& /*abort*/)
	{
		input_to_ext<true, false>(external_device_id, ext, combos, value);
	});

	ext.status = controller.ext_status;

	// Save firing mode
	controller.firing_mode = ext.custom[0] & button_flags::ss_firing_mode_mask;
}

/**
 * \brief Maps external Move controller data to DS3 input. (This can be input from any physical pad, not just the DS3)
 *        Implementation detail: CellGemExtPortData's digital/analog fields map the same way as
 *        libPad, so no translation is needed.
 * \param gem_num gem index to use
 * \param ext External data to modify
 * \return true on success, false if controller is disconnected
 */
static void get_external_device_input(u32 gem_num, gem_config::gem_controller& controller, CellGemExtPortData& ext)
{
	ext = {};

	if (!is_input_allowed() || input::g_pads_intercepted) // Let's intercept the PS Move just like a pad
	{
		return;
	}

	// The sharpshooter only sets the custom bytes as follows:
	// custom[0] (0x01): Firing mode selector is in position 1.
	// custom[0] (0x02): Firing mode selector is in position 2.
	// custom[0] (0x04): Firing mode selector is in position 3.
	// custom[0] (0x40): T button trigger is pressed.
	// custom[0] (0x80): RL reload button is pressed.

	// The racing wheel sets the custom bytes as follows:
	// custom[0] 0-255:  Throttle
	// custom[1] 0-255:  L2
	// custom[2] 0-255:  R2
	// custom[3] (0x01): Left paddle
	// custom[3] (0x02): Right paddle

	switch (g_cfg.io.move)
	{
	case move_handler::real:
		real_input_to_ext(gem_num, controller, ext);
		break;
	case move_handler::fake:
		fake_input_to_ext(gem_num, controller, ext);
		break;
	case move_handler::mouse:
	case move_handler::raw_mouse:
		mouse_input_to_ext(gem_num, controller, ext);
		break;
	default:
		break;
	}
}

/**
 * \brief Maps Move controller data (digital buttons, and analog Trigger data) to mouse input.
 * \param mouse_no Mouse index number to use
 * \param digital_buttons Bitmask filled with CELL_GEM_CTRL_* values
 * \param analog_t Analog value of Move's Trigger.
 * \return true on success, false if mouse_no is invalid
 */
static bool mouse_input_to_pad(u32 mouse_no, be_t<u16>& digital_buttons, be_t<u16>& analog_t)
{
	digital_buttons = 0;
	analog_t = 0;

	if (!is_input_allowed() || input::g_pads_intercepted) // Let's intercept the PS Move just like a pad
	{
		return false;
	}

	auto& handler = g_fxo->get<MouseHandlerBase>();

	std::scoped_lock lock(handler.mutex);

	// Make sure that the mouse handler is initialized
	handler.Init(std::min<u32>(g_fxo->get<gem_config>().attribute.max_connect, CELL_GEM_MAX_NUM));

	if (mouse_no >= handler.GetMice().size())
	{
		return false;
	}

	const Mouse& mouse_data = ::at32(handler.GetMice(), mouse_no);

	if (gem_v032_buttons())
	{
		// v0.0.32 layout and combos
		const auto is_pressed = [&mouse_data](MouseButtonCodes button) -> bool { return !!(mouse_data.buttons & button); };

		if (is_pressed(CELL_MOUSE_BUTTON_1))
			digital_buttons |= CELL_GEM_CTRL_T;

		if (is_pressed(CELL_MOUSE_BUTTON_2))
			digital_buttons |= CELL_GEM_CTRL_MOVE;

		if (is_pressed(CELL_MOUSE_BUTTON_3))
			digital_buttons |= CELL_GEM_CTRL_CROSS;

		if (is_pressed(CELL_MOUSE_BUTTON_4))
			digital_buttons |= CELL_GEM_CTRL_CIRCLE;

		if (is_pressed(CELL_MOUSE_BUTTON_5))
			digital_buttons |= CELL_GEM_CTRL_SQUARE;

		if (is_pressed(CELL_MOUSE_BUTTON_6) || (is_pressed(CELL_MOUSE_BUTTON_1) && is_pressed(CELL_MOUSE_BUTTON_2)))
			digital_buttons |= CELL_GEM_CTRL_SELECT;

		if (is_pressed(CELL_MOUSE_BUTTON_7) || (is_pressed(CELL_MOUSE_BUTTON_1) && is_pressed(CELL_MOUSE_BUTTON_3)))
			digital_buttons |= CELL_GEM_CTRL_START;

		if (is_pressed(CELL_MOUSE_BUTTON_8) || (is_pressed(CELL_MOUSE_BUTTON_2) && is_pressed(CELL_MOUSE_BUTTON_3)))
			digital_buttons |= CELL_GEM_CTRL_TRIANGLE;

		analog_t = (mouse_data.buttons & CELL_MOUSE_BUTTON_1) ? 0xFFFF : 0;

		return true;
	}

	const auto& cfg = ::at32(g_cfg_gem_mouse.players, mouse_no);

	bool combo_active = false;
	std::set<pad_button> combos;

	static const std::unordered_map<gem_btn, u16> btn_map =
	{
		{ gem_btn::start, CELL_GEM_CTRL_START },
		{ gem_btn::select, CELL_GEM_CTRL_SELECT },
		{ gem_btn::triangle, CELL_GEM_CTRL_TRIANGLE },
		{ gem_btn::circle, CELL_GEM_CTRL_CIRCLE },
		{ gem_btn::cross, CELL_GEM_CTRL_CROSS },
		{ gem_btn::square, CELL_GEM_CTRL_SQUARE },
		{ gem_btn::move, CELL_GEM_CTRL_MOVE },
		{ gem_btn::t, CELL_GEM_CTRL_T },
		{ gem_btn::combo_start, CELL_GEM_CTRL_START },
		{ gem_btn::combo_select, CELL_GEM_CTRL_SELECT },
		{ gem_btn::combo_triangle, CELL_GEM_CTRL_TRIANGLE },
		{ gem_btn::combo_circle, CELL_GEM_CTRL_CIRCLE },
		{ gem_btn::combo_cross, CELL_GEM_CTRL_CROSS },
		{ gem_btn::combo_square, CELL_GEM_CTRL_SQUARE },
		{ gem_btn::combo_move, CELL_GEM_CTRL_MOVE },
		{ gem_btn::combo_t, CELL_GEM_CTRL_T },
	};

	// Check combo button first
	cfg->handle_input(mouse_data, [&combo_active](const auto& value, bool& abort)
	{
		if (value.pressed && value.btn == gem_btn::combo)
		{
			combo_active = true;
			abort = true;
		}
	});

	// Check combos
	if (combo_active)
	{
		cfg->handle_input(mouse_data, [&digital_buttons, &combos](const auto& value, bool& /*abort*/)
		{
			if (!value.pressed)
				return;

			switch (value.btn)
			{
			case gem_btn::combo_start:
			case gem_btn::combo_select:
			case gem_btn::combo_triangle:
			case gem_btn::combo_circle:
			case gem_btn::combo_cross:
			case gem_btn::combo_square:
			case gem_btn::combo_move:
			case gem_btn::combo_t:
				digital_buttons |= ::at32(btn_map, value.btn);
				combos.insert(value.pad_btn);
				break;
			default:
				break;
			}
		});
	}

	// Check normal buttons
	cfg->handle_input(mouse_data, [&digital_buttons, &combos](const auto& value, bool& /*abort*/)
	{
		if (!value.pressed)
			return;

		switch (value.btn)
		{
		case gem_btn::start:
		case gem_btn::select:
		case gem_btn::square:
		case gem_btn::cross:
		case gem_btn::circle:
		case gem_btn::triangle:
		case gem_btn::move:
		case gem_btn::t:
			// Ignore this gem_btn if the same pad_button was already used in a combo
			if (!combos.contains(value.pad_btn))
			{
				digital_buttons |= ::at32(btn_map, value.btn);
			}
			break;
		default:
			break;
		}
	});

	analog_t = (digital_buttons & CELL_GEM_CTRL_T) ? 255 : 0;

	return true;
}

template <typename T>
static void mouse_pos_to_gem_state(u32 mouse_no, gem_config::gem_controller& controller, T& gem_state)
{
	if (!gem_state || !is_input_allowed() || input::g_pads_intercepted) // Let's intercept the PS Move just like a pad
	{
		return;
	}

	auto& handler = g_fxo->get<MouseHandlerBase>();

	std::scoped_lock lock(handler.mutex);

	// Make sure that the mouse handler is initialized
	handler.Init(std::min<u32>(g_fxo->get<gem_config>().attribute.max_connect, CELL_GEM_MAX_NUM));

	if (mouse_no >= handler.GetMice().size())
	{
		return;
	}

	const auto& mouse = ::at32(handler.GetMice(), mouse_no);

	if constexpr (std::is_same_v<T, vm::ptr<CellGemState>>)
	{
		if (gem_v032_mouse())
		{
			// v0.0.32 math (pixel position in the game window, radius 10, distance 1500 mm)
			const auto& shared_data = g_fxo->get<gem_camera_shared>();
			const f32 width = static_cast<f32>(shared_data.width);
			const f32 height = static_cast<f32>(shared_data.height);
			const f32 x_max = mouse.x_max > 0 ? static_cast<f32>(mouse.x_max) : width;
			const f32 y_max = mouse.y_max > 0 ? static_cast<f32>(mouse.y_max) : height;
			const f32 x_pix = mouse.x_max > 0 ? static_cast<f32>(mouse.x_pos_pixels) : mouse.x_pos * width;
			const f32 y_pix = mouse.y_max > 0 ? static_cast<f32>(mouse.y_pos_pixels) : mouse.y_pos * height;
			const f32 scaling_width = x_max / width;
			const f32 scaling_height = y_max / height;
			constexpr f32 old_radius = 10.0f;
			constexpr f32 old_distance = 1500.0f;
			const f32 mmPerPixel = CELL_GEM_SPHERE_RADIUS_MM / old_radius;

			const f32 image_x = x_pix / scaling_width;
			const f32 image_y = y_pix / scaling_height;
			const f32 camera_x = (image_x - (width / 2.f)) * mmPerPixel;
			const f32 camera_y = ((height / 2.f) - image_y) * mmPerPixel;

			gem_state->pos[0] = camera_x;
			gem_state->pos[1] = camera_y;
			gem_state->pos[2] = old_distance;
			gem_state->pos[3] = 0.f;

			gem_state->quat[0] = 320.f - image_x;
			gem_state->quat[1] = (y_pix / scaling_width) - 180.f;
			gem_state->quat[2] = 1200.f;
			gem_state->quat[3] = 1.f;

			gem_state->handle_pos[0] = camera_x;
			gem_state->handle_pos[1] = camera_y;
			gem_state->handle_pos[2] = old_distance + 10.f;
			gem_state->handle_pos[3] = 0.f;

			if (g_cfg.io.show_move_cursor)
			{
				draw_overlay_cursor(mouse_no, controller, mouse.x_pos, mouse.y_pos);
			}
			return;
		}

		ps_move_data& move_data = ::at32(g_fxo->get<gem_config>().fake_move_data, mouse_no);
		const auto [aim_x, aim_y] = mlgb_aim(mouse_no, mouse.x_pos, mouse.y_pos);
		pos_to_gem_state(mouse_no, controller, gem_state, aim_x, aim_y, move_data);
	}
	else if constexpr (std::is_same_v<T, vm::ptr<CellGemImageState>>)
	{
		const auto [aim_x, aim_y] = mlgb_aim(mouse_no, mouse.x_pos, mouse.y_pos);
		pos_to_gem_image_state(mouse_no, controller, gem_state, aim_x, aim_y);
	}
}

#ifdef HAVE_LIBEVDEV
static bool gun_input_to_pad(u32 gem_no, be_t<u16>& digital_buttons, be_t<u16>& analog_t)
{
	digital_buttons = 0;
	analog_t = 0;

	if (!is_input_allowed())
		return false;

	gun_thread& gun = g_fxo->get<gun_thread>();
	std::scoped_lock lock(gun.handler.mutex);

	if (gun.handler.get_button(gem_no, gun_button::btn_left) == 1)
		digital_buttons |= CELL_GEM_CTRL_T;

	if (gun.handler.get_button(gem_no, gun_button::btn_right) == 1)
		digital_buttons |= CELL_GEM_CTRL_MOVE;

	if (gun.handler.get_button(gem_no, gun_button::btn_middle) == 1)
		digital_buttons |= CELL_GEM_CTRL_START;

	if (gun.handler.get_button(gem_no, gun_button::btn_1) == 1)
		digital_buttons |= CELL_GEM_CTRL_CROSS;

	if (gun.handler.get_button(gem_no, gun_button::btn_2) == 1)
		digital_buttons |= CELL_GEM_CTRL_CIRCLE;

	if (gun.handler.get_button(gem_no, gun_button::btn_3) == 1)
		digital_buttons |= CELL_GEM_CTRL_SELECT;

	if (gun.handler.get_button(gem_no, gun_button::btn_5) == 1)
		digital_buttons |= CELL_GEM_CTRL_TRIANGLE;

	if (gun.handler.get_button(gem_no, gun_button::btn_6) == 1)
		digital_buttons |= CELL_GEM_CTRL_SQUARE;

	analog_t = gun.handler.get_button(gem_no, gun_button::btn_left) ? 255 : 0;

	return true;
}

template <typename T>
static void gun_pos_to_gem_state(u32 gem_no, gem_config::gem_controller& controller, T& gem_state)
{
	if (!gem_state || !is_input_allowed())
		return;

	f32 x_pos, y_pos;
	{
		gun_thread& gun = g_fxo->get<gun_thread>();
		std::scoped_lock lock(gun.handler.mutex);

		x_pos = gun.handler.get_axis_x(gem_no);
		y_pos = gun.handler.get_axis_y(gem_no);
	}

	if constexpr (std::is_same_v<T, vm::ptr<CellGemState>>)
	{
		ps_move_data& move_data = ::at32(g_fxo->get<gem_config>().fake_move_data, gem_no);
		pos_to_gem_state(gem_no, controller, gem_state, x_pos, y_pos, move_data);
	}
	else if constexpr (std::is_same_v<T, vm::ptr<CellGemImageState>>)
	{
		pos_to_gem_image_state(gem_no, controller, gem_state, x_pos, y_pos);
	}
}
#endif

// *********************
// * cellGem functions *
// *********************

error_code cellGemCalibrate(u32 gem_num)
{
	cellGem.todo("cellGemCalibrate(gem_num=%d)", gem_num);

	auto& gem = g_fxo->get<gem_config>();

	std::scoped_lock lock(gem.mtx);

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!check_gem_num(gem_num))
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	auto& controller = gem.controllers[gem_num];

	if (controller.is_calibrating)
	{
		return CELL_EBUSY;
	}

	controller.is_calibrating = true;
	controller.calibration_start_us = get_guest_system_time();

	return CELL_OK;
}

error_code cellGemClearStatusFlags(u32 gem_num, u64 mask)
{
	cellGem.todo("cellGemClearStatusFlags(gem_num=%d, mask=0x%x)", gem_num, mask);

	auto& gem = g_fxo->get<gem_config>();

	std::scoped_lock lock(gem.mtx);

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!check_gem_num(gem_num))
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	gem.controllers[gem_num].calibration_status_flags &= ~mask;
	gem.v032_status_flags &= ~mask;

	return CELL_OK;
}

error_code cellGemConvertVideoFinish(ppu_thread& ppu)
{
	ppu.state += cpu_flag::wait;

	cellGem.warning("cellGemConvertVideoFinish()");

	auto& gem = g_fxo->get<gem_config>();

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!gem.video_conversion_in_progress)
	{
		return CELL_GEM_ERROR_CONVERT_NOT_STARTED;
	}

	if (!gem.wait_for_result(ppu))
	{
		return {};
	}

	return CELL_OK;
}

error_code cellGemConvertVideoStart(ppu_thread& ppu, vm::cptr<void> video_frame)
{
	ppu.state += cpu_flag::wait;

	cellGem.warning("cellGemConvertVideoStart(video_frame=*0x%x)", video_frame);

	auto& gem = g_fxo->get<gem_config>();

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!video_frame)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	if (!video_frame.aligned(128))
	{
		return CELL_GEM_ERROR_INVALID_ALIGNMENT;
	}

	if (gem.video_conversion_in_progress)
	{
		return CELL_GEM_ERROR_CONVERT_NOT_FINISHED;
	}

	const auto& shared_data = g_fxo->get<gem_camera_shared>();
	gem.video_data_in.resize(shared_data.size);
	std::memcpy(gem.video_data_in.data(), video_frame.get_ptr(), gem.video_data_in.size());

	gem.video_conversion_in_progress = true;
	gem.wake_up();

	return CELL_OK;
}

error_code cellGemEnableCameraPitchAngleCorrection(u32 enable_flag)
{
	cellGem.todo("cellGemEnableCameraPitchAngleCorrection(enable_flag=%d)", enable_flag);

	auto& gem = g_fxo->get<gem_config>();

	std::scoped_lock lock(gem.mtx);

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	gem.enable_pitch_correction = !!enable_flag;

	return CELL_OK;
}

error_code cellGemEnableMagnetometer(u32 gem_num, u32 enable)
{
	cellGem.todo("cellGemEnableMagnetometer(gem_num=%d, enable=0x%x)", gem_num, enable);

	auto& gem = g_fxo->get<gem_config>();

	std::scoped_lock lock(gem.mtx);

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!check_gem_num(gem_num))
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	if (!gem.is_controller_ready(gem_num))
	{
		return CELL_GEM_NOT_CONNECTED;
	}

	auto& controller = gem.controllers[gem_num];

	// NOTE: RE doesn't show this check but it is mentioned in the docs, so I'll leave it here for now.
	//if (!controller.calibrated_magnetometer)
	//{
	//	return CELL_GEM_NOT_CALIBRATED;
	//}

	controller.enabled_magnetometer = !!enable;

	if (g_cfg.io.move == move_handler::real)
	{
		std::lock_guard lock(pad::g_pad_mutex);

		const auto handler = pad::get_pad_thread();
		const auto& pad = ::at32(handler->GetPads(), pad_num(gem_num));

		if (pad && pad->m_pad_handler == pad_handler::move && !pad->is_copilot())
		{
			pad->move_data.magnetometer_enabled = controller.enabled_magnetometer;
		}
	}

	return CELL_OK;
}

error_code cellGemEnableMagnetometer2(u32 gem_num, u32 enable)
{
	cellGem.trace("cellGemEnableMagnetometer2(gem_num=%d, enable=0x%x)", gem_num, enable);

	auto& gem = g_fxo->get<gem_config>();

	std::scoped_lock lock(gem.mtx);

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!check_gem_num(gem_num))
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	if (!gem.is_controller_ready(gem_num))
	{
		return CELL_GEM_NOT_CONNECTED;
	}

	auto& controller = gem.controllers[gem_num];

	if (!controller.calibrated_magnetometer)
	{
		return CELL_GEM_NOT_CALIBRATED;
	}

	controller.enabled_magnetometer = !!enable;

	if (g_cfg.io.move == move_handler::real)
	{
		std::lock_guard lock(pad::g_pad_mutex);

		const auto handler = pad::get_pad_thread();
		const auto& pad = ::at32(handler->GetPads(), pad_num(gem_num));

		if (pad && pad->m_pad_handler == pad_handler::move && !pad->is_copilot())
		{
			pad->move_data.magnetometer_enabled = controller.enabled_magnetometer;
		}
	}

	return CELL_OK;
}

error_code cellGemEnd(ppu_thread& ppu)
{
	ppu.state += cpu_flag::wait;

	cellGem.warning("cellGemEnd()");

	auto& gem = g_fxo->get<gem_config>();

	std::unique_lock lock(gem.mtx);

	if (gem.state.compare_and_swap_test(1, 0))
	{
		if (u32 addr = std::exchange(gem.memory_ptr, 0))
		{
			sys_memory_free(ppu, addr);
		}

		return CELL_OK;
	}

	lock.unlock();

	auto& tracker = g_fxo->get<named_thread<gem_tracker>>();
	if (!tracker.wait_for_tracker_result(ppu))
	{
		return {};
	}

	gem.updating = false;

	return CELL_GEM_ERROR_UNINITIALIZED;
}

error_code cellGemFilterState(u32 gem_num, u32 enable)
{
	cellGem.warning("cellGemFilterState(gem_num=%d, enable=%d)", gem_num, enable);

	auto& gem = g_fxo->get<gem_config>();

	std::scoped_lock lock(gem.mtx);

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!check_gem_num(gem_num))
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	gem.controllers[gem_num].enabled_filtering = !!enable;

	return CELL_OK;
}

error_code cellGemForceRGB(u32 gem_num, f32 r, f32 g, f32 b)
{
	cellGem.warning("cellGemForceRGB(gem_num=%d, r=%f, g=%f, b=%f)", gem_num, r, g, b);

	auto& gem = g_fxo->get<gem_config>();

	std::scoped_lock lock(gem.mtx);

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!check_gem_num(gem_num))
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	// TODO: Adjust brightness
	//if (const f32 sum = r + g + b; sum > 2.f)
	//{
	//	color = color * (2.f / sum)
	//}

	auto& controller = gem.controllers[gem_num];

	controller.sphere_rgb = gem_config::gem_color(r, g, b);
	controller.enabled_tracking = false;

	const auto [h, s, v] = ps_move_tracker<false>::rgb_to_hsv(r, g, b);
	controller.hue = h;

	return CELL_OK;
}

error_code cellGemGetAccelerometerPositionInDevice(u32 gem_num, vm::ptr<f32> pos)
{
	cellGem.todo("cellGemGetAccelerometerPositionInDevice(gem_num=%d, pos=*0x%x)", gem_num, pos);

	auto& gem = g_fxo->get<gem_config>();

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!check_gem_num(gem_num) || !pos)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	// TODO
	pos[0] = 0.0f;
	pos[1] = 0.0f;
	pos[2] = 0.0f;
	pos[3] = 0.0f;

	return CELL_OK;
}

error_code cellGemGetAllTrackableHues(vm::ptr<u8> hues)
{
	cellGem.todo("cellGemGetAllTrackableHues(hues=*0x%x)", hues);

	auto& gem = g_fxo->get<gem_config>();

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!hues)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	auto& tracker = g_fxo->get<named_thread<gem_tracker>>();
	std::lock_guard lock(tracker.mutex);

	for (u32 hue = 0; hue < 360; hue++)
	{
		hues[hue] = tracker.hue_is_trackable(hue);
	}

	return CELL_OK;
}

error_code cellGemGetCameraState(vm::ptr<CellGemCameraState> camera_state)
{
	cellGem.todo("cellGemGetCameraState(camera_state=0x%x)", camera_state);

	[[maybe_unused]] auto& gem = g_fxo->get<gem_config>();

	if (!camera_state)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	// TODO: use correct camera settings
	camera_state->exposure = 0;
	camera_state->exposure_time = 1.0f / 60.0f;
	camera_state->gain = 1.0f;
	camera_state->pitch_angle = 0.0f;
	camera_state->pitch_angle_estimate = 0.0f;

	return CELL_OK;
}

error_code cellGemGetEnvironmentLightingColor(vm::ptr<f32> r, vm::ptr<f32> g, vm::ptr<f32> b)
{
	cellGem.todo("cellGemGetEnvironmentLightingColor(r=*0x%x, g=*0x%x, b=*0x%x)", r, g, b);

	auto& gem = g_fxo->get<gem_config>();

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!r || !g || !b)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	// default to 128
	*r = 128;
	*g = 128;
	*b = 128;

	// NOTE: RE doesn't show this check but it is mentioned in the docs, so I'll leave it here for now.
	//if (!gem.controllers[gem_num].calibrated_magnetometer)
	//{
	//	return CELL_GEM_ERROR_LIGHTING_NOT_CALIBRATED; // This error doesn't really seem to be a real thing.
	//}

	return CELL_OK;
}

error_code cellGemGetHuePixels(vm::cptr<void> camera_frame, u32 hue, vm::ptr<u8> pixels)
{
	cellGem.todo("cellGemGetHuePixels(camera_frame=*0x%x, hue=%d, pixels=*0x%x)", camera_frame, hue, pixels);

	auto& gem = g_fxo->get<gem_config>();

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!camera_frame || !pixels || hue > 359)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	std::memset(pixels.get_ptr(), 0, 640 * 480 * sizeof(u8));

	if (g_cfg.io.move == move_handler::real)
	{
		// TODO: get pixels from tracker
	}

	return CELL_OK;
}

error_code cellGemGetImageState(u32 gem_num, vm::ptr<CellGemImageState> gem_image_state)
{
	cellGem.warning("cellGemGetImageState(gem_num=%d, image_state=&0x%x)", gem_num, gem_image_state);

	auto& gem = g_fxo->get<gem_config>();
	std::scoped_lock lock(gem.mtx);

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!check_gem_num(gem_num) || !gem_image_state)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	*gem_image_state = {};

	if (g_cfg.io.move != move_handler::null)
	{
		const auto& shared_data = g_fxo->get<gem_camera_shared>();
		auto& controller = gem.controllers[gem_num];

		gem_image_state->frame_timestamp = shared_data.frame_timestamp_us.load();
		gem_image_state->timestamp = gem_image_state->frame_timestamp + 10;

		switch (g_cfg.io.move)
		{
		case move_handler::real:
			ps_move_pos_to_gem_state(gem_num, controller, gem_image_state);
			break;
		case move_handler::fake:
			ds3_pos_to_gem_state(gem_num, controller, gem_image_state);
			break;
		case move_handler::mouse:
		case move_handler::raw_mouse:
			mouse_pos_to_gem_state(gem_num, controller, gem_image_state);
			break;
#ifdef HAVE_LIBEVDEV
		case move_handler::gun:
			gun_pos_to_gem_state(gem_num, controller, gem_image_state);
			break;
#endif
		case move_handler::null:
			fmt::throw_exception("Unreachable");
		}

		gem_image_state->r = controller.radius; // Radius in camera pixels
		gem_image_state->distance = controller.distance_mm;
		gem_image_state->visible = controller.radius_valid && gem.is_controller_ready(gem_num);
		gem_image_state->r_valid = controller.radius_valid;

		// Pedal build: The Shoot bowling swing - the camera sees the sphere drop low, then swing up and come closer
		if (gem_mlgb_title() && (g_cfg.io.move == move_handler::raw_mouse) && gem.mouse_bowl[gem_num].shaping)
		{
			const auto& b = gem.mouse_bowl[gem_num];
			const f32 d0 = controller.distance_mm > 1.f ? controller.distance_mm : 1500.f;
			const f32 d = std::max(200.f, d0 + b.dz);
			const f32 r0 = controller.radius;
			const f32 mm_per_px = r0 > 0.f ? CELL_GEM_SPHERE_RADIUS_MM / r0 : 1.f;
			gem_image_state->v = g_fxo->get<gem_camera_shared>().height / 2.f - b.dy / mm_per_px; // lane height, not the aim
			gem_image_state->r = r0 * d0 / d;
			gem_image_state->distance = d;
			gem_image_state->visible = 1;
			gem_image_state->r_valid = 1;
		}
	}

	return CELL_OK;
}

error_code cellGemGetInertialState(u32 gem_num, u32 state_flag, u64 timestamp, vm::ptr<CellGemInertialState> inertial_state)
{
	cellGem.trace("cellGemGetInertialState(gem_num=%d, state_flag=%d, timestamp=0x%x, inertial_state=0x%x)", gem_num, state_flag, timestamp, inertial_state);

	auto& gem = g_fxo->get<gem_config>();

	std::scoped_lock lock(gem.mtx);

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!check_gem_num(gem_num) || !inertial_state || !gem.is_controller_ready(gem_num))
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	if (false) // TODO
	{
		return CELL_GEM_TIME_OUT_OF_RANGE;
	}

	*inertial_state = {};

	if (g_cfg.io.move != move_handler::null)
	{
		get_external_device_input(gem_num, gem.controllers[gem_num], inertial_state->ext);

		inertial_state->timestamp = (get_guest_system_time() - gem.start_timestamp_us);
		inertial_state->counter = gem.inertial_counter++;
		inertial_state->accelerometer[2] = 1.0f; // Current gravity in G units (9.81 == 1 unit)

		if (gem_v032_mouse())
		{
			// v0.0.32 value
			inertial_state->accelerometer[0] = 10;
			inertial_state->accelerometer[2] = 0;
		}

		switch (g_cfg.io.move)
		{
		case move_handler::real:
		case move_handler::fake:
		{
			// Get temperature and sensor data
			{
				std::lock_guard lock(pad::g_pad_mutex);

				const auto handler = pad::get_pad_thread();
				const auto& pad = ::at32(handler->GetPads(), pad_num(gem_num));

				if (pad && pad->is_connected() && !pad->is_copilot())
				{
					inertial_state->temperature = pad->move_data.temperature;

					for (u32 i = 0; i < 3; i++)
					{
						inertial_state->accelerometer[i] = pad->move_data.accelerometer[i];
						inertial_state->gyro[i] = pad->move_data.gyro[i];
					}
				}
			}

			ds3_input_to_pad(gem_num, inertial_state->pad.digitalbuttons, inertial_state->pad.analog_T);
			break;
		}
		case move_handler::mouse:
		case move_handler::raw_mouse:
			mouse_input_to_pad(gem_num, inertial_state->pad.digitalbuttons, inertial_state->pad.analog_T);
			// Pedal build: TriggerStart (outputs file) - on a continue screen a trigger pull presses Start (one short press)
			if (pedal_outputs::trigger_is_start(gem_num))
			{
				inertial_state->pad.digitalbuttons = static_cast<u16>(+inertial_state->pad.digitalbuttons | CELL_GEM_CTRL_START);
			}
			// MLGB lightgun fork: The Shoot reads gravity and gyro from the inertial state
			if (gem_mlgb_title() && Emu.GetTitleID() == "BCUS98222")
			{
				std::lock_guard history_lock(gem.mouse_history_mutex);
				const auto& data = gem.fake_move_data[gem_num];
				if (shoot_spin_enabled(gem_num)) inertial_state->pad.digitalbuttons &= ~CELL_GEM_CTRL_SQUARE;
				if (shoot_raw_flag(gem_num, &raw_mouse_config::shoot_bat)) inertial_state->pad.digitalbuttons &= ~CELL_GEM_CTRL_SELECT;
				if (shoot_point_down_enabled(gem_num)) inertial_state->pad.digitalbuttons &= ~CELL_GEM_CTRL_TRIANGLE;
				// Pedal build: T stays held while a bowling swing plays out; Circle is the bowling button then
				if (shoot_bowl_enabled(gem_num))
				{
					inertial_state->pad.digitalbuttons = static_cast<u16>(+inertial_state->pad.digitalbuttons & ~CELL_GEM_CTRL_CIRCLE);
				}
				if (gem.mouse_bowl[gem_num].t_released_for_regrab(get_guest_system_time()))
				{
					inertial_state->pad.digitalbuttons = static_cast<u16>(+inertial_state->pad.digitalbuttons & ~CELL_GEM_CTRL_T);
					inertial_state->pad.analog_T = 0;
				}
				if (gem.mouse_bowl[gem_num].swinging(get_guest_system_time()))
				{
					inertial_state->pad.digitalbuttons = static_cast<u16>(+inertial_state->pad.digitalbuttons | CELL_GEM_CTRL_T);
					inertial_state->pad.analog_T = 255;
				}
				for (u32 i = 0; i < 3; i++)
				{
					inertial_state->accelerometer[i] = data.accelerometer[i];
					inertial_state->gyro[i] = get_guest_system_time() - gem.mouse_orientation_time[gem_num] <= 100'000 ? data.gyro[i] : 0.f;
				}
			}
			break;
#ifdef HAVE_LIBEVDEV
		case move_handler::gun:
			gun_input_to_pad(gem_num, inertial_state->pad.digitalbuttons, inertial_state->pad.analog_T);
			break;
#endif
		case move_handler::null:
			fmt::throw_exception("Unreachable");
		}
	}

	return CELL_OK;
}

static error_code cellGemGetInfo_inner(vm::ptr<CellGemInfo> info)
{
	cellGem.trace("cellGemGetInfo(info=*0x%x)", info);

	auto& gem = g_fxo->get<gem_config>();

	std::scoped_lock lock(gem.mtx); // pedal build: writes the connection status (v0.0.32)

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!info)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	if (gem_v032_mouse())
	{
		// v0.0.32: refresh the gun connections only here, when the game asks
		auto& handler = g_fxo->get<MouseHandlerBase>();
		std::lock_guard mouse_lock(handler.mutex);
		handler.Init(std::min<u32>(gem.attribute.max_connect, CELL_GEM_MAX_NUM));

		const MouseInfo& minfo = handler.GetInfo();

		// A moment with no guns at all (handler still starting, e.g. right after a savestate) is not an unplug:
		// keep the last known status, or the game drops the guns' calibration.
		const bool handler_not_ready = minfo.now_connect == 0 && gem.connected_controllers > 0;

		for (u32 i = 0; i < CELL_GEM_MAX_NUM && !handler_not_ready; i++)
		{
			if (i == 0) gem.connected_controllers = 0;

			if (i < gem.attribute.max_connect && minfo.status[i] == CELL_MOUSE_STATUS_CONNECTED)
			{
				gem.connected_controllers++;
				gem.controllers[i].status = CELL_GEM_STATUS_READY;
				gem.controllers[i].port = port_num(i);
			}
			else
			{
				gem.controllers[i].status = CELL_GEM_STATUS_DISCONNECTED;
				gem.controllers[i].port = 0;
			}
		}
	}

	info->max_connect = gem.attribute.max_connect;
	info->now_connect = gem.connected_controllers;

	for (int i = 0; i < CELL_GEM_MAX_NUM; i++)
	{
		info->status[i] = gem.controllers[i].status;
		info->port[i] = gem.controllers[i].port;
	}

	return CELL_OK;
}

u32 GemGetMemorySize(s32 max_connect)
{
	return max_connect <= 2 ? 0x120000 : 0x140000;
}

error_code cellGemGetMemorySize(s32 max_connect)
{
	cellGem.warning("cellGemGetMemorySize(max_connect=%d)", max_connect);

	if (max_connect > CELL_GEM_MAX_NUM || max_connect <= 0)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	return not_an_error(GemGetMemorySize(max_connect));
}

error_code cellGemGetRGB(u32 gem_num, vm::ptr<f32> r, vm::ptr<f32> g, vm::ptr<f32> b)
{
	cellGem.todo("cellGemGetRGB(gem_num=%d, r=*0x%x, g=*0x%x, b=*0x%x)", gem_num, r, g, b);

	auto& gem = g_fxo->get<gem_config>();

	reader_lock lock(gem.mtx);

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!check_gem_num(gem_num) || !r || !g || !b)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	const gem_config_data::gem_color& sphere_color = gem.controllers[gem_num].sphere_rgb;
	*r = sphere_color.r;
	*g = sphere_color.g;
	*b = sphere_color.b;

	return CELL_OK;
}

error_code cellGemGetRumble(u32 gem_num, vm::ptr<u8> rumble)
{
	cellGem.todo("cellGemGetRumble(gem_num=%d, rumble=*0x%x)", gem_num, rumble);

	auto& gem = g_fxo->get<gem_config>();

	reader_lock lock(gem.mtx);

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!check_gem_num(gem_num) || !rumble)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	*rumble = gem.controllers[gem_num].rumble;

	return CELL_OK;
}

static error_code cellGemGetState_inner(u32 gem_num, u32 flag, u64 time_parameter, vm::ptr<CellGemState> gem_state)
{
	cellGem.trace("cellGemGetState(gem_num=%d, flag=0x%x, time=0x%llx, gem_state=*0x%x)", gem_num, flag, time_parameter, gem_state);

	auto& gem = g_fxo->get<gem_config>();

	reader_lock lock(gem.mtx);

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!check_gem_num(gem_num) || flag > CELL_GEM_STATE_FLAG_TIMESTAMP || !gem_state)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	if (!gem.is_controller_ready(gem_num))
	{
		return not_an_error(CELL_GEM_NOT_CONNECTED);
	}

	// TODO: Get the gem state at the specified time
	//if (flag == CELL_GEM_STATE_FLAG_CURRENT_TIME)
	//{
	//	// now + time_parameter (time_parameter in microseconds). Positive values actually allow predictions for the future state.
	//}
	//else if (flag == CELL_GEM_STATE_FLAG_LATEST_IMAGE_TIME)
	//{
	//	// When the sphere was registered during the last camera frame (time_parameter may also have an impact)
	//}
	//else // CELL_GEM_STATE_FLAG_TIMESTAMP
	//{
	//	// As specified by time_parameter.
	//}

	if (false) // TODO: check if there is data for the specified time_parameter and flag
	{
		return CELL_GEM_TIME_OUT_OF_RANGE;
	}

	auto& controller = gem.controllers[gem_num];

	*gem_state = {};

	if (g_cfg.io.move != move_handler::null)
	{
		get_external_device_input(gem_num, controller, gem_state->ext);

		if (gem_v032_mouse())
		{
			// v0.0.32: always visible
			gem_state->tracking_flags |= CELL_GEM_TRACKING_FLAG_VISIBLE;
			if (controller.enabled_tracking)
				gem_state->tracking_flags |= CELL_GEM_TRACKING_FLAG_POSITION_TRACKED;
		}
		else if (controller.enabled_tracking)
		{
			gem_state->tracking_flags |= CELL_GEM_TRACKING_FLAG_POSITION_TRACKED;
			gem_state->tracking_flags |= CELL_GEM_TRACKING_FLAG_VISIBLE;
		}

		gem_state->timestamp = (get_guest_system_time() - gem.start_timestamp_us);
		gem_state->camera_pitch_angle = 0.f;

		switch (g_cfg.io.move)
		{
		case move_handler::real:
		{
			auto& tracker = g_fxo->get<named_thread<gem_tracker>>();
			const ps_move_info info = tracker.get_info(gem_num);

			ds3_input_to_pad(gem_num, gem_state->pad.digitalbuttons, gem_state->pad.analog_T);
			ps_move_pos_to_gem_state(gem_num, controller, gem_state);

			if (info.valid)
				gem_state->tracking_flags |= CELL_GEM_TRACKING_FLAG_VISIBLE;
			else
				gem_state->tracking_flags &= ~CELL_GEM_TRACKING_FLAG_VISIBLE;

			break;
		}
		case move_handler::fake:
			ds3_input_to_pad(gem_num, gem_state->pad.digitalbuttons, gem_state->pad.analog_T);
			ds3_pos_to_gem_state(gem_num, controller, gem_state);
			break;
		case move_handler::mouse:
		case move_handler::raw_mouse:
			mouse_input_to_pad(gem_num, gem_state->pad.digitalbuttons, gem_state->pad.analog_T);
			// Pedal build: TriggerStart (outputs file) - on a continue screen a trigger pull presses Start (one short press)
			if (pedal_outputs::trigger_is_start(gem_num))
			{
				gem_state->pad.digitalbuttons = static_cast<u16>(+gem_state->pad.digitalbuttons | CELL_GEM_CTRL_START);
			}
			mouse_pos_to_gem_state(gem_num, controller, gem_state);
			break;
#ifdef HAVE_LIBEVDEV
		case move_handler::gun:
			gun_input_to_pad(gem_num, gem_state->pad.digitalbuttons, gem_state->pad.analog_T);
			gun_pos_to_gem_state(gem_num, controller, gem_state);
			break;
#endif
		case move_handler::null:
			fmt::throw_exception("Unreachable");
		}
	}

	if (false) // TODO: check if we are computing colors
	{
		return CELL_GEM_COMPUTING_AVAILABLE_COLORS;
	}

	if (controller.is_calibrating)
	{
		return CELL_GEM_SPHERE_CALIBRATING;
	}

	if (!controller.calibrated_magnetometer)
	{
		return CELL_GEM_SPHERE_NOT_CALIBRATED;
	}

	if (!controller.hue_set)
	{
		return CELL_GEM_HUE_NOT_SET;
	}

	// MLGB lightgun fork: The Shoot compares timestamped samples. Returning the live state for every
	// historical request discards motion and button transitions between samples.
	if (gem_mlgb_title() && Emu.GetTitleID() == "BCUS98222")
	{
		std::lock_guard history_lock(gem.mouse_history_mutex);
		if (gem.mouse_history_epoch != gem.start_timestamp_us)
		{
			for (auto& samples : gem.mouse_history) samples.clear();
			gem.mouse_history_epoch = gem.start_timestamp_us;
		}

		auto& samples = gem.mouse_history[gem_num];
		const u64 now = gem_state->timestamp;
		if (!samples.empty() && now < static_cast<u64>(samples.back().timestamp)) samples.clear();
		if (samples.empty() || now - static_cast<u64>(samples.back().timestamp) >= 1000)
		{
			samples.push_back(*gem_state);
			if (samples.size() > 512) samples.pop_front();
		}

		if (flag == CELL_GEM_STATE_FLAG_TIMESTAMP ||
			(flag == CELL_GEM_STATE_FLAG_CURRENT_TIME && static_cast<s64>(time_parameter) < 0))
		{
			const s64 requested = flag == CELL_GEM_STATE_FLAG_TIMESTAMP
				? static_cast<s64>(time_parameter) : static_cast<s64>(now) + static_cast<s64>(time_parameter);
			if (requested < 0 || samples.empty() || static_cast<u64>(requested) < static_cast<u64>(samples.front().timestamp))
				return CELL_GEM_TIME_OUT_OF_RANGE;

			for (auto sample = samples.rbegin(); sample != samples.rend(); ++sample)
			{
				if (static_cast<u64>(sample->timestamp) <= static_cast<u64>(requested))
				{
					*gem_state = *sample;
					break;
				}
			}
		}
	}

	return CELL_OK;
}

static error_code cellGemGetStatusFlags_inner(u32 gem_num, vm::ptr<u64> flags)
{
	cellGem.trace("cellGemGetStatusFlags(gem_num=%d, flags=*0x%x)", gem_num, flags);

	auto& gem = g_fxo->get<gem_config>();

	reader_lock lock(gem.mtx);

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!check_gem_num(gem_num) || !flags)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	*flags = gem_v032_mouse() ? (gem.v032_status_flags | gem.controllers[gem_num].calibration_status_flags) : (gem.runtime_status_flags | gem.controllers[gem_num].calibration_status_flags);

	return CELL_OK;
}

error_code cellGemGetTrackerHue(u32 gem_num, vm::ptr<u32> hue)
{
	cellGem.warning("cellGemGetTrackerHue(gem_num=%d, hue=*0x%x)", gem_num, hue);

	auto& gem = g_fxo->get<gem_config>();

	reader_lock lock(gem.mtx);

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!check_gem_num(gem_num) || !hue)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	const auto& controller = gem.controllers[gem_num];

	if (!controller.enabled_tracking || controller.hue > 359)
	{
		return { CELL_GEM_ERROR_NOT_A_HUE, controller.hue };
	}

	*hue = controller.hue;

	return CELL_OK;
}

error_code cellGemHSVtoRGB(f32 h, f32 s, f32 v, vm::ptr<f32> r, vm::ptr<f32> g, vm::ptr<f32> b)
{
	cellGem.warning("cellGemHSVtoRGB(h=%f, s=%f, v=%f, r=*0x%x, g=*0x%x, b=*0x%x)", h, s, v, r, g, b);

	if (!r || !g || !b)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	h = std::fmod(h, 360.0f);
	s = std::clamp(s, 0.0f, 1.0f);
	v = std::clamp(v, 0.0f, 1.0f);

	if (h < 0.0)
	{
		h += 360.0f;
	}

	const f32 c = v * s;
	const f32 x = c * (1.0f - fabs(fmod(h / 60.0f, 2.0f) - 1.0f));
	const f32 m = v - c;

	f32 r_tmp{};
	f32 g_tmp{};
	f32 b_tmp{};

	if (h < 60.0f)
	{
		r_tmp = c;
		g_tmp = x;
	}
	else if (h < 120.0f)
	{
		r_tmp = x;
		g_tmp = c;
	}
	else if (h < 180.0f)
	{
		g_tmp = c;
		b_tmp = x;
	}
	else if (h < 240.0f)
	{
		g_tmp = x;
		b_tmp = c;
	}
	else if (h < 300.0f)
	{
		r_tmp = x;
		b_tmp = c;
	}
	else
	{
		r_tmp = c;
		b_tmp = x;
	}

	*r = std::clamp(r_tmp + m, 0.0f, 1.0f);
	*g = std::clamp(g_tmp + m, 0.0f, 1.0f);
	*b = std::clamp(b_tmp + m, 0.0f, 1.0f);

	return CELL_OK;
}

error_code cellGemInit(ppu_thread& ppu, vm::cptr<CellGemAttribute> attribute)
{
	cellGem.warning("cellGemInit(attribute=*0x%x)", attribute);

	auto& gem = g_fxo->get<gem_config>();

	if (!attribute || !attribute->spurs_addr || !attribute->max_connect || attribute->max_connect > CELL_GEM_MAX_NUM)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	std::scoped_lock lock(gem.mtx);

	if (!gem.state.compare_and_swap_test(0, 1))
	{
		return CELL_GEM_ERROR_ALREADY_INITIALIZED;
	}

	if (!attribute->memory_ptr)
	{
		vm::var<u32> addr(0);

		// Decrease memory stats
		if (sys_memory_allocate(ppu, GemGetMemorySize(attribute->max_connect), SYS_MEMORY_PAGE_SIZE_64K, +addr) != CELL_OK)
		{
			return CELL_GEM_ERROR_RESOURCE_ALLOCATION_FAILED;
		}

		gem.memory_ptr = *addr;
	}
	else
	{
		gem.memory_ptr = 0;
	}

	gem.updating = false;
	gem.camera_frame = 0;
	gem.runtime_status_flags = 0;
	gem.v032_status_flags = 0;
	gem.attribute = *attribute;

	for (int gem_num = 0; gem_num < CELL_GEM_MAX_NUM; gem_num++)
	{
		gem.reset_controller(gem_num);
	}

	// TODO: is this correct?
	gem.start_timestamp_us = get_guest_system_time();

	gem.wake_up();

	return CELL_OK;
}

error_code cellGemInvalidateCalibration(s32 gem_num)
{
	cellGem.todo("cellGemInvalidateCalibration(gem_num=%d)", gem_num);

	auto& gem = g_fxo->get<gem_config>();

	std::scoped_lock lock(gem.mtx);

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!check_gem_num(gem_num))
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	auto& controller = gem.controllers[gem_num];

	// TODO: does this really stop an ongoing calibration ?
	controller.calibrated_magnetometer = false;
	controller.is_calibrating = false;
	controller.calibration_start_us = 0;

	if (gem_v032_mouse())
	{
		// v0.0.32 only reset the calibration itself
		return CELL_OK;
	}

	controller.calibration_status_flags = 0;
	controller.hue_set = false;
	controller.enabled_tracking = false;

	return CELL_OK;
}

s32 cellGemIsTrackableHue(u32 hue)
{
	cellGem.todo("cellGemIsTrackableHue(hue=%d)", hue);

	auto& gem = g_fxo->get<gem_config>();

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (hue > 359)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	auto& tracker = g_fxo->get<named_thread<gem_tracker>>();
	std::lock_guard lock(tracker.mutex);

	return tracker.hue_is_trackable(hue);
}

error_code cellGemPrepareCamera(s32 max_exposure, f32 image_quality)
{
	cellGem.todo("cellGemPrepareCamera(max_exposure=%d, image_quality=%f)", max_exposure, image_quality);

	auto& gem = g_fxo->get<gem_config>();

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (false) // TODO: Check if the camera is currently being prepared.
	{
		return CELL_EBUSY;
	}

	max_exposure = std::clamp(max_exposure, static_cast<s32>(CELL_GEM_MIN_CAMERA_EXPOSURE), static_cast<s32>(CELL_GEM_MAX_CAMERA_EXPOSURE));
	image_quality = std::clamp(image_quality, 0.0f, 1.0f);

	// TODO: prepare camera properly

	extern error_code cellCameraGetAttribute(s32 dev_num, s32 attrib, vm::ptr<u32> arg1, vm::ptr<u32> arg2);
	extern error_code cellCameraSetAttribute(s32 dev_num, s32 attrib, u32 arg1, u32 arg2);
	extern error_code cellCameraGetBufferInfoEx(ppu_thread&, s32 dev_num, vm::ptr<CellCameraInfoEx> info);

	vm::var<CellCameraInfoEx> info = vm::make_var<CellCameraInfoEx>({});
	vm::var<u32> arg1 = vm::make_var<u32>({});
	vm::var<u32> arg2 = vm::make_var<u32>({});

	cellCameraGetAttribute(0, 0x3e6, arg1, arg2);
	cellCameraSetAttribute(0, 0x3e6, 0x3e, *arg2 | 0x80);
	cellCameraGetBufferInfoEx(*cpu_thread::get_current<ppu_thread>(), 0, info);

	if (info->width == 640)
	{
		// Disable some features
		cellCameraSetAttribute(0, CELL_CAMERA_AGC, 0, 0);
		cellCameraSetAttribute(0, CELL_CAMERA_AWB, 0, 0);
		cellCameraSetAttribute(0, CELL_CAMERA_AEC, 0, 0);
		cellCameraSetAttribute(0, CELL_CAMERA_GAMMA, 0, 0);
		cellCameraSetAttribute(0, CELL_CAMERA_PIXELOUTLIERFILTER, 0, 0);

		// Set new values for others
		cellCameraSetAttribute(0, CELL_CAMERA_GREENGAIN, 96, 0);
		cellCameraSetAttribute(0, CELL_CAMERA_REDBLUEGAIN, 64, 96);
		cellCameraSetAttribute(0, CELL_CAMERA_GAIN, 0, 0); // TODO
		cellCameraSetAttribute(0, CELL_CAMERA_EXPOSURE, 0, 0); // TODO
	}

	return CELL_OK;
}

error_code cellGemPrepareVideoConvert(vm::cptr<CellGemVideoConvertAttribute> vc_attribute)
{
	cellGem.warning("cellGemPrepareVideoConvert(vc_attribute=*0x%x)", vc_attribute);

	auto& gem = g_fxo->get<gem_config>();

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!vc_attribute)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	const CellGemVideoConvertAttribute vc = *vc_attribute;

	if (vc.version != CELL_GEM_VERSION)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	if (vc.output_format != CELL_GEM_NO_VIDEO_OUTPUT)
	{
		if (!vc.video_data_out)
		{
			return CELL_GEM_ERROR_INVALID_PARAMETER;
		}
	}

	if ((vc.conversion_flags & CELL_GEM_COMBINE_PREVIOUS_INPUT_FRAME) && !vc.buffer_memory)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	if (!vc.video_data_out.aligned(128) || !vc.buffer_memory.aligned(16))
	{
		return CELL_GEM_ERROR_INVALID_ALIGNMENT;
	}

	gem.vc_attribute = vc;

	const s32 buffer_size = cellGemGetVideoConvertSize(vc.output_format);
	gem.video_data_out_size = buffer_size;

	return CELL_OK;
}

error_code cellGemReadExternalPortDeviceInfo(u32 gem_num, vm::ptr<u32> ext_id, vm::ptr<u8[CELL_GEM_EXTERNAL_PORT_DEVICE_INFO_SIZE]> ext_info)
{
	cellGem.warning("cellGemReadExternalPortDeviceInfo(gem_num=%d, ext_id=*0x%x, ext_info=%s)", gem_num, ext_id, ext_info);

	auto& gem = g_fxo->get<gem_config>();

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!check_gem_num(gem_num) || !ext_id)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	if (!gem.is_controller_ready(gem_num))
	{
		return CELL_GEM_NOT_CONNECTED;
	}

	const u64 start = get_system_time();

	auto& controller = gem.controllers[gem_num];

	if (g_cfg.io.move != move_handler::null)
	{
		// Get external device status
		CellGemExtPortData ext_port_data{};
		get_external_device_input(gem_num, controller, ext_port_data);
	}

	if (!(controller.ext_status & CELL_GEM_EXT_CONNECTED))
	{
		return CELL_GEM_NO_EXTERNAL_PORT_DEVICE;
	}

	*ext_id = controller.ext_id;

	if (ext_info && g_cfg.io.move == move_handler::real)
	{
		bool read_requested = false;

		while (true)
		{
			{
				std::lock_guard lock(pad::g_pad_mutex);

				const auto handler = pad::get_pad_thread();
				const auto& pad = ::at32(handler->GetPads(), pad_num(gem_num));

				if (pad->m_pad_handler != pad_handler::move || !pad->is_connected() || pad->is_copilot())
				{
					return CELL_GEM_NOT_CONNECTED;
				}

				if (!read_requested)
				{
					pad->move_data.external_device_read_requested = true;
					read_requested = true;
				}

				if (!pad->move_data.external_device_read_requested)
				{
					*ext_id = controller.ext_id = pad->move_data.external_device_id;
					std::memcpy(ext_info.get_ptr(), pad->move_data.external_device_read.data(), CELL_GEM_EXTERNAL_PORT_DEVICE_INFO_SIZE);
					break;
				}
			}

			// We wait for 300ms at most
			if (const u64 elapsed_us = get_system_time() - start; elapsed_us > 300'000)
			{
				cellGem.warning("cellGemReadExternalPortDeviceInfo(gem_num=%d): timeout", gem_num);
				break;
			}

			// TODO: sleep ?
		}
	}

	return CELL_OK;
}

error_code cellGemReset(u32 gem_num)
{
	cellGem.todo("cellGemReset(gem_num=%d)", gem_num);

	auto& gem = g_fxo->get<gem_config>();

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!check_gem_num(gem_num))
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	gem.reset_controller(gem_num);

	// TODO: is this correct?
	gem.start_timestamp_us = get_guest_system_time();

	return CELL_OK;
}

error_code cellGemSetRumble(u32 gem_num, u8 rumble)
{
	cellGem.trace("cellGemSetRumble(gem_num=%d, rumble=0x%x)", gem_num, rumble);

	auto& gem = g_fxo->get<gem_config>();

	std::scoped_lock lock(gem.mtx);

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!check_gem_num(gem_num))
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	gem.controllers[gem_num].rumble = rumble;
	pedal_outputs::gem_rumble(gem_num, rumble);

	// Set actual device rumble
	if (g_cfg.io.move == move_handler::real)
	{
		std::lock_guard pad_lock(pad::g_pad_mutex);
		const auto handler = pad::get_pad_thread();
		auto& handlers = handler->get_handlers();
		if (auto it = handlers.find(pad_handler::move); it != handlers.end() && it->second)
		{
			const u32 pad_index = pad_num(gem_num);
			for (const auto& binding : it->second->bindings())
			{
				if (!binding.device || binding.device->player_id != pad_index) continue;

				handler->SetRumble(pad_index, rumble, rumble);
				break;
			}
		}
	}

	return CELL_OK;
}

error_code cellGemSetYaw(u32 gem_num, v128 z_direction)
{
	// Unpack vector argument
	const f32 z_direction_x = z_direction.fr[0];
	const f32 z_direction_y = z_direction.fr[1];
	const f32 z_direction_z = z_direction.fr[2];
	const f32 z_direction_w = z_direction.fr[3];

	cellGem.warning("cellGemSetYaw(gem_num=%d, z_direction_x=%f, z_direction_y=%f, z_direction_z=%f, z_direction_w=%f)", gem_num, z_direction_x, z_direction_y, z_direction_z, z_direction_w);

	auto& gem = g_fxo->get<gem_config>();

	std::scoped_lock lock(gem.mtx);

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!check_gem_num(gem_num))
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	if (g_cfg.io.move != move_handler::real)
	{
		return CELL_OK;
	}

	std::lock_guard pad_lock(pad::g_pad_mutex);
	const auto handler = pad::get_pad_thread();
	const auto& pad = ::at32(handler->GetPads(), pad_num(gem_num));

	if (pad && pad->m_pad_handler == pad_handler::move && !pad->is_copilot())
	{
		// z_direction is the direction of the controller's z axis (sphere -> handle) in world coordinates.
		// This function is usually used when the game wants to re-orient the ps move towards the camera, e.g. during calibration or as a surrogate for it.
		// So far we've seen:
		// A: (0,0,1,0): The controller points straight at the camera along the camera axis. This is our default orientation.
		// B: The current sphere position (CellGemState.pos) while pointing at the camera.
		//    This is equal to A if the controller is on the camera axis. Otherwise the correct yaw would be atan2(x, z).
		// For now we simply reset the orientation (including pitch and roll) instead of only adjusting the yaw.
		pad->move_data.orientation_reset_requested = true;
	}

	return CELL_OK;
}

error_code cellGemTrackHues(vm::cptr<u32> req_hues, vm::ptr<u32> res_hues)
{
	cellGem.todo("cellGemTrackHues(req_hues=%s, res_hues=*0x%x)", req_hues ? fmt::format("*0x%x [%d, %d, %d, %d]", req_hues, req_hues[0], req_hues[1], req_hues[2], req_hues[3]) : "*0x0", res_hues);

	auto& gem = g_fxo->get<gem_config>();

	std::scoped_lock lock(gem.mtx);

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!req_hues)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	for (u32 i = 0; i < CELL_GEM_MAX_NUM; i++)
	{
		if (req_hues[i] == CELL_GEM_DONT_CARE_HUE)
		{
			gem.controllers[i].enabled_tracking = true;
			gem.controllers[i].enabled_LED = true;
			gem.controllers[i].hue_set = true;

			switch (i)
			{
			default:
			case 0:
				gem.controllers[i].hue = 240; // blue
				break;
			case 1:
				gem.controllers[i].hue = 0;   // red
				break;
			case 2:
				gem.controllers[i].hue = 120; // green
				break;
			case 3:
				gem.controllers[i].hue = 300; // purple
				break;
			}

			const auto [r, g, b] = ps_move_tracker<false>::hsv_to_rgb(gem.controllers[i].hue, 1.0f, 1.0f);
			gem.controllers[i].sphere_rgb = gem_config::gem_color(r / 255.0f, g / 255.0f, b / 255.0f);

			if (res_hues)
			{
				res_hues[i] = gem.controllers[i].hue;
			}
		}
		else if (req_hues[i] == CELL_GEM_DONT_TRACK_HUE)
		{
			gem.controllers[i].enabled_tracking = false;
			gem.controllers[i].enabled_LED = false;
			gem.controllers[i].hue_set = gem_v032_mouse(); // v0.0.32 set it for every controller

			if (res_hues)
			{
				res_hues[i] = CELL_GEM_DONT_TRACK_HUE;
			}
		}
		else
		{
			if (req_hues[i] > 359)
			{
				cellGem.warning("cellGemTrackHues: req_hues[%d]=%d -> this can lead to unexpected behavior", i, req_hues[i]);
			}

			gem.controllers[i].enabled_tracking = true;
			gem.controllers[i].enabled_LED = true;
			gem.controllers[i].hue_set = true;
			gem.controllers[i].hue = req_hues[i];

			const auto [r, g, b] = ps_move_tracker<false>::hsv_to_rgb(gem.controllers[i].hue, 1.0f, 1.0f);
			gem.controllers[i].sphere_rgb = gem_config::gem_color(r / 255.0f, g / 255.0f, b / 255.0f);

			if (res_hues)
			{
				res_hues[i] = gem.controllers[i].hue;
			}
		}
	}

	return CELL_OK;
}

error_code cellGemUpdateFinish(ppu_thread& ppu)
{
	ppu.state += cpu_flag::wait;

	cellGem.warning("cellGemUpdateFinish()");

	auto& gem = g_fxo->get<gem_config>();

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!gem.updating)
	{
		return CELL_GEM_ERROR_UPDATE_NOT_STARTED;
	}

	auto& tracker = g_fxo->get<named_thread<gem_tracker>>();
	if (!tracker.wait_for_tracker_result(ppu))
	{
		return {};
	}

	std::scoped_lock lock(gem.mtx);

	gem.updating = false;

	if (!gem.camera_frame)
	{
		return not_an_error(CELL_GEM_NO_VIDEO);
	}

	return CELL_OK;
}

error_code cellGemUpdateStart(vm::cptr<void> camera_frame, u64 timestamp)
{
	cellGem.warning("cellGemUpdateStart(camera_frame=*0x%x, timestamp=%d)", camera_frame, timestamp);

	auto& gem = g_fxo->get<gem_config>();

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	auto& tracker = g_fxo->get<named_thread<gem_tracker>>();

	if (tracker.is_busy())
	{
		return CELL_GEM_ERROR_UPDATE_NOT_FINISHED;
	}

	std::scoped_lock lock(gem.mtx);

	// Update is starting even when camera_frame is null
	if (gem.updating.exchange(true))
	{
		return CELL_GEM_ERROR_UPDATE_NOT_FINISHED;
	}

	if (!camera_frame.aligned(128))
	{
		return CELL_GEM_ERROR_INVALID_ALIGNMENT;
	}

	gem.camera_frame = camera_frame.addr();

	const bool image_set = tracker.set_image(camera_frame);

	tracker.wake_up_tracker();

	if (!image_set)
	{
		return not_an_error(CELL_GEM_NO_VIDEO);
	}

	return CELL_OK;
}

error_code cellGemWriteExternalPort(u32 gem_num, vm::ptr<u8[CELL_GEM_EXTERNAL_PORT_OUTPUT_SIZE]> data)
{
	cellGem.warning("cellGemWriteExternalPort(gem_num=%d, data=%s)", gem_num, data);

	auto& gem = g_fxo->get<gem_config>();

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!check_gem_num(gem_num))
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	if (!gem.is_controller_ready(gem_num))
	{
		return CELL_GEM_NOT_CONNECTED;
	}

	if (data && g_cfg.io.move == move_handler::real)
	{
		std::lock_guard lock(pad::g_pad_mutex);

		const auto handler = pad::get_pad_thread();
		const auto& pad = ::at32(handler->GetPads(), pad_num(gem_num));

		if (pad->m_pad_handler != pad_handler::move || !pad->is_connected() || pad->is_copilot())
		{
			return CELL_GEM_NOT_CONNECTED;
		}

		if (pad->move_data.external_device_write_requested)
		{
			return CELL_GEM_ERROR_WRITE_NOT_FINISHED;
		}

		std::memcpy(pad->move_data.external_device_write.data(), data.get_ptr(), CELL_GEM_EXTERNAL_PORT_OUTPUT_SIZE);
		pad->move_data.external_device_write_requested = true;
	}

	return CELL_OK;
}

// Pedal build: thin wrappers kept (the test logging that used to live here is removed)
error_code cellGemGetState(u32 gem_num, u32 flag, u64 time_parameter, vm::ptr<CellGemState> gem_state)
{
	return cellGemGetState_inner(gem_num, flag, time_parameter, gem_state);
}

error_code cellGemGetInfo(vm::ptr<CellGemInfo> info)
{
	return cellGemGetInfo_inner(info);
}

error_code cellGemGetStatusFlags(u32 gem_num, vm::ptr<u64> flags)
{
	return cellGemGetStatusFlags_inner(gem_num, flags);
}

DECLARE(ppu_module_manager::cellGem)("libgem", []()
{
	REG_FUNC(libgem, cellGemCalibrate);
	REG_FUNC(libgem, cellGemClearStatusFlags);
	REG_FUNC(libgem, cellGemConvertVideoFinish);
	REG_FUNC(libgem, cellGemConvertVideoStart);
	REG_FUNC(libgem, cellGemEnableCameraPitchAngleCorrection);
	REG_FUNC(libgem, cellGemEnableMagnetometer);
	REG_FUNC(libgem, cellGemEnableMagnetometer2);
	REG_FUNC(libgem, cellGemEnd);
	REG_FUNC(libgem, cellGemFilterState);
	REG_FUNC(libgem, cellGemForceRGB);
	REG_FUNC(libgem, cellGemGetAccelerometerPositionInDevice);
	REG_FUNC(libgem, cellGemGetAllTrackableHues);
	REG_FUNC(libgem, cellGemGetCameraState);
	REG_FUNC(libgem, cellGemGetEnvironmentLightingColor);
	REG_FUNC(libgem, cellGemGetHuePixels);
	REG_FUNC(libgem, cellGemGetImageState);
	REG_FUNC(libgem, cellGemGetInertialState);
	REG_FUNC(libgem, cellGemGetInfo);
	REG_FUNC(libgem, cellGemGetMemorySize);
	REG_FUNC(libgem, cellGemGetRGB);
	REG_FUNC(libgem, cellGemGetRumble);
	REG_FUNC(libgem, cellGemGetState);
	REG_FUNC(libgem, cellGemGetStatusFlags);
	REG_FUNC(libgem, cellGemGetTrackerHue);
	REG_FUNC(libgem, cellGemHSVtoRGB);
	REG_FUNC(libgem, cellGemInit);
	REG_FUNC(libgem, cellGemInvalidateCalibration);
	REG_FUNC(libgem, cellGemIsTrackableHue);
	REG_FUNC(libgem, cellGemPrepareCamera);
	REG_FUNC(libgem, cellGemPrepareVideoConvert);
	REG_FUNC(libgem, cellGemReadExternalPortDeviceInfo);
	REG_FUNC(libgem, cellGemReset);
	REG_FUNC(libgem, cellGemSetRumble);
	REG_FUNC(libgem, cellGemSetYaw);
	REG_FUNC(libgem, cellGemTrackHues);
	REG_FUNC(libgem, cellGemUpdateFinish);
	REG_FUNC(libgem, cellGemUpdateStart);
	REG_FUNC(libgem, cellGemWriteExternalPort);
});
