#include "stdafx.h"
#include "games_config.h"
#include "util/logs.hpp"
#include "util/yaml.hpp"
#include "Utilities/File.h"
#include "Utilities/StrUtil.h"

#include "Loader/ISO.h"

LOG_CHANNEL(cfg_log, "CFG");

// Pedal build: games.yml may hold paths without a drive letter, so the whole emulator folder can move
// between PCs / drive letters. "$(EmulatorDir)" (or a plain relative path) means "the folder rpcs3.exe is in".
// Paths inside the emulator folder are written back that way too.
namespace
{
	const std::string emu_dir_var = "$(EmulatorDir)";

	std::string slashes(std::string path)
	{
		for (char& c : path)
		{
			if (c == '\\') c = '/';
		}
		return path;
	}

	bool is_relative(const std::string& path)
	{
		if (path.empty()) return false;
		if (path.size() >= 2 && path[1] == ':') return false;    // C:/...
		if (path[0] == '/' || path[0] == '\\') return false;    // /... or \\server\...
		if (path.find(':') != umax) return false;                // devices / anything unusual
		return true;
	}

	std::string from_file(const std::string& path)
	{
		const std::string emu_dir = slashes(fs::get_executable_dir());

		if (path.starts_with(emu_dir_var))
		{
			return emu_dir + path.substr(emu_dir_var.size());
		}

		if (!emu_dir.empty() && is_relative(path))
		{
			return emu_dir + path;
		}

		return path;
	}

	std::string to_file(const std::string& path)
	{
		const std::string emu_dir = slashes(fs::get_executable_dir());
		const std::string p = slashes(path);

		if (!emu_dir.empty() && p.size() > emu_dir.size())
		{
			const std::string head = p.substr(0, emu_dir.size());
#ifdef _WIN32
			const bool same = fmt::to_lower(head) == fmt::to_lower(emu_dir);
#else
			const bool same = head == emu_dir;
#endif
			if (same)
			{
				return emu_dir_var + p.substr(emu_dir.size());
			}
		}

		return path;
	}
}

games_config::games_config()
{
	load();
}

games_config::~games_config()
{
	if (m_dirty)
	{
		save();
	}
}

const std::map<std::string, std::string> games_config::get_games() const
{
	std::lock_guard lock(m_mutex);
	return m_games;
}

std::string games_config::get_path(const std::string& title_id) const
{
	if (title_id.empty())
	{
		return {};
	}

	std::lock_guard lock(m_mutex);

	if (const auto it = m_games.find(title_id); it != m_games.cend())
	{
		return it->second;
	}

	return {};
}

games_config::result games_config::add_game(const std::string& key, const std::string& path)
{
	if (path == iso_device::virtual_device_name + "/")
	{
		const auto device = fs::get_virtual_device(iso_device::virtual_device_name + "/");
		if (!device) return result::failure;

		const auto iso_dev = dynamic_cast<const iso_device*>(device.get());
		return add_game(key, iso_dev->get_loaded_iso());
	}

	if (fs::is_drive_name(path))
	{
		// Restore the trailing delimiter trimmed by the caller if the game is on the root of a raw device (e.g. "E:/"),
		// which is also the form provided by "rpcs3::utils::get_games_dir()" and the one built by "GetBdvdDir()", so
		// that the same title is always registered with the very same path
		return add_game(key, path + '/');
	}

	std::lock_guard lock(m_mutex);

	// Access or create node if does not exist
	if (auto it = m_games.find(key); it != m_games.end())
	{
		if (it->second == path)
		{
			// Nothing to do
			return result::exists;
		}

		it->second = path;
	}
	else
	{
		m_games.emplace(key, path);
	}

	m_dirty = true;

	if (m_save_on_dirty && !save_nl())
	{
		return result::failure;
	}

	return result::success;
}

games_config::result games_config::add_external_hdd_game(const std::string& key, std::string& path)
{
	// Don't use the C00 subdirectory in our game list
	if (path.ends_with("/C00") || path.ends_with("\\C00"))
	{
		path = path.substr(0, path.size() - 4);
	}

	const result res = add_game(key, path);

	switch (res)
	{
	case result::failure:
		cfg_log.error("Failed to save HG game location of title '%s' (error=%s)", key, fs::g_tls_error);
		break;
	case result::success:
		cfg_log.notice("Registered HG game directory for title '%s': %s", key, path);
		break;
	case result::exists:
		break;
	}

	return res;
}

games_config::result games_config::remove_game(const std::string& key)
{
	std::lock_guard lock(m_mutex);

	// Remove node
	if (m_games.erase(key) == 0) // If node not found
	{
		// Nothing to do
		return result::success;
	}

	m_dirty = true;

	if (m_save_on_dirty && !save_nl())
	{
		return result::failure;
	}

	return result::success;
}

bool games_config::save_nl()
{
	std::map<std::string, std::string> games;
	for (const auto& [key, path] : m_games)
	{
		games.emplace(key, to_file(path));
	}

	YAML::Emitter out;
	out << games;

	fs::pending_file temp(fs::get_config_dir(true) + "games.yml");

	if (temp.file && temp.file.write(out.c_str(), out.size()) >= out.size() && temp.commit())
	{
		m_dirty = false;
		return true;
	}

	cfg_log.error("Failed to save games.yml: %s", fs::g_tls_error);
	return false;
}

bool games_config::save()
{
	std::lock_guard lock(m_mutex);
	return save_nl();
}

void games_config::load()
{
	std::lock_guard lock(m_mutex);

	m_games.clear();

	const std::string path = fs::get_config_dir(true) + "games.yml";

	// Move file from deprecated location to new location
#ifdef _WIN32
	const std::string old_path = fs::get_config_dir(false) + "games.yml";

	if (fs::is_file(old_path))
	{
		cfg_log.notice("Found deprecated games.yml file: '%s'", old_path);

		if (!fs::rename(old_path, path, false))
		{
			(fs::g_tls_error == fs::error::exist ? cfg_log.warning : cfg_log.error)("Failed to move '%s' to '%s' (error='%s')", old_path, path, fs::g_tls_error);
		}
	}
#endif

	if (fs::file f{path, fs::read + fs::create})
	{
		auto [result, error] = yaml_load(f.to_string());

		if (!error.empty())
		{
			cfg_log.error("Failed to load games.yml: %s", error);
		}

		if (!result.IsMap())
		{
			if (!result.IsNull())
			{
				cfg_log.error("Failed to load games.yml: type %d not a map", result.Type());
			}
			return;
		}

		for (const auto& entry : result)
		{
			if (!entry.first.Scalar().empty() && entry.second.IsScalar() && !entry.second.Scalar().empty())
			{
				m_games.emplace(entry.first.Scalar(), from_file(entry.second.Scalar()));
			}
		}
	}
}
