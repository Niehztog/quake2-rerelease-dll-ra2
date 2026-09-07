// Copyright (c) ZeniMax Media Inc.
// Licensed under the GNU General Public License 2.0.

// rocketarena2/maploop.cpp -- arena.cfg config parser, MOTD loader and map rotation.
#include "../g_local.h"
#include "arena.h"

#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// GetModuleHandleEx/GetModuleFileName and dladdr, for module_directory() below
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace
{

enum class definition_type_t
{
	plain,
	value_list,
	block
};

struct definition_t
{
	std::vector<std::string> names;
	definition_type_t type = definition_type_t::plain;
	std::vector<std::string> values;
	std::vector<definition_t> block;
};

struct tokenizer_t
{
	explicit tokenizer_t(std::string_view input) : input(input) {}

	[[nodiscard]] static bool is_ra_alnum(char ch)
	{
		const unsigned char uch = static_cast<unsigned char>(ch);
		return std::isalnum(uch) != 0;
	}

	[[nodiscard]] std::optional<std::string> next()
	{
		for (;;)
		{
			while (pos < input.size() && std::isspace(static_cast<unsigned char>(input[pos])))
				++pos;

			if (pos >= input.size())
				return std::nullopt;

			if (input[pos] == '/' && (pos + 1) < input.size() && input[pos + 1] == '/')
			{
				pos += 2;
				while (pos < input.size() && input[pos] != '\n')
					++pos;
				continue;
			}

			if (!is_ra_alnum(input[pos]))
				return std::string(1, input[pos++]);

			const size_t start = pos;
			while (pos < input.size() && is_ra_alnum(input[pos]))
				++pos;
			return std::string(input.substr(start, pos - start));
		}
	}

	std::string_view input;
	size_t pos = 0;
};

cvar_t *gamedir = nullptr;
cvar_t *arenacfg = nullptr;

std::vector<definition_t> definition_blocks;
const definition_t *map_loop = nullptr;
const definition_t *map_block = nullptr;
std::vector<const definition_t *> arena_blocks;

void clear_config_state()
{
	map_loop = nullptr;
	map_block = nullptr;
	arena_blocks.clear();
	definition_blocks.clear();
}

[[nodiscard]] const definition_t *find_key(std::string_view key, definition_type_t type, const std::vector<definition_t> &items)
{
	for (const definition_t &item : items)
	{
		if (item.type != type)
			continue;

		for (const std::string &name : item.names)
			if (name == key)
				return &item;
	}

	return nullptr;
}

[[nodiscard]] bool has_val(const definition_t &def, std::string_view key)
{
	for (const std::string &value : def.values)
		if (value == key)
			return true;

	return false;
}

[[nodiscard]] const char *get_val(const definition_t &def, size_t index)
{
	static constexpr char empty[] = "";
	return index < def.values.size() ? def.values[index].c_str() : empty;
}

[[nodiscard]] bool parse_block(tokenizer_t &tok, std::vector<definition_t> &out, bool nested)
{
	for (;;)
	{
		auto token = tok.next();
		if (!token)
			return !nested;

		if (*token == "}")
		{
			if (!nested)
			{
				gi.Com_Print("Error reading config file: unbalanced {}\n");
				return false;
			}

			return true;
		}

		definition_t def;
		def.names.push_back(*token);

		for (;;)
		{
			auto next = tok.next();
			if (!next)
			{
				if (nested)
				{
					gi.Com_Print("Error reading config file: unbalanced {}\n");
					return false;
				}

				out.push_back(std::move(def));
				return true;
			}

			if (*next == ":")
			{
				def.type = definition_type_t::value_list;
				for (;;)
				{
					auto value = tok.next();
					if (!value)
					{
						gi.Com_Print("Error reading config file: unbalanced {}\n");
						return false;
					}

					if (*value == ";")
						break;

					def.values.push_back(*value);
				}

				out.push_back(std::move(def));
				break;
			}

			if (*next == "{")
			{
				def.type = definition_type_t::block;
				if (!parse_block(tok, def.block, true))
					return false;
				out.push_back(std::move(def));
				break;
			}

			if (*next == "}")
			{
				if (!nested)
				{
					gi.Com_Print("Error reading config file: unbalanced {}\n");
					return false;
				}

				out.push_back(std::move(def));
				return true;
			}

			def.names.push_back(*next);
		}
	}
}

[[nodiscard]] std::optional<std::string> load_text_file(const std::filesystem::path &path)
{
	std::ifstream file(path, std::ios::in | std::ios::binary);
	if (!file)
		return std::nullopt;

	return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

// The engine loads the game library out of the mod's gamedir, but that gamedir is
// not necessarily reachable from the process's working directory: the remaster
// looks for mods in both the install dir and the user's writable data dir (on
// Windows "%USERPROFILE%\Saved Games\Nightdive Studios\Quake II") while running
// with the install dir as the working directory either way, so a bare
// "<game>/arena.cfg" can resolve to nothing. Ask the OS which directory this very
// library was loaded from instead -- arena.cfg and motd.txt ship next to it.
[[nodiscard]] std::filesystem::path module_directory()
{
#ifdef _WIN32
	HMODULE module = nullptr;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
							reinterpret_cast<LPCWSTR>(&module_directory), &module))
		return {};

	std::wstring buffer(MAX_PATH, L'\0');
	for (;;)
	{
		const DWORD length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
		if (!length)
			return {};

		if (length < buffer.size())
		{
			buffer.resize(length);
			break;
		}

		buffer.resize(buffer.size() * 2); // path was truncated, retry with more room
	}

	return std::filesystem::path(buffer).parent_path();
#else
	Dl_info info = {};
	if (!dladdr(reinterpret_cast<const void *>(&module_directory), &info) || !info.dli_fname)
		return {};

	return std::filesystem::path(info.dli_fname).parent_path();
#endif
}

void ensure_path_cvars()
{
	gamedir = gi.cvar("game", ".", CVAR_LATCH);
	arenacfg = gi.cvar("arenacfg", "arena.cfg", CVAR_NOFLAGS);
}

// Where to look for a gamedir-relative mod file, best candidate first.
[[nodiscard]] std::vector<std::filesystem::path> gamedir_candidates(const std::filesystem::path &relative)
{
	ensure_path_cvars();

	std::vector<std::filesystem::path> candidates;

	if (const std::string &dir = ra2_gamedir(); !dir.empty())
		candidates.push_back(std::filesystem::path(dir) / relative);

	// working-directory-relative, which is how RA2 has always found these files on
	// a server started from the base dir
	std::filesystem::path legacy = std::filesystem::path(gamedir->string) / relative;
	if (candidates.empty() || candidates.front() != legacy)
		candidates.push_back(std::move(legacy));

	return candidates;
}

struct text_file_t
{
	std::optional<std::string> text; // unset if none of the candidates could be read
	std::filesystem::path path;		 // the candidate that was read
	std::string tried;				 // every candidate, for the error message
};

[[nodiscard]] text_file_t load_gamedir_text_file(const std::filesystem::path &relative)
{
	text_file_t result;

	for (const std::filesystem::path &path : gamedir_candidates(relative))
	{
		if (!result.tried.empty())
			result.tried += " or ";
		result.tried += path.string();

		result.text = load_text_file(path);
		if (result.text)
		{
			result.path = path;
			break;
		}
	}

	return result;
}

[[nodiscard]] std::string join_words(const std::vector<std::string> &words)
{
	std::string out;
	for (size_t i = 0; i < words.size(); ++i)
	{
		if (i)
			out += ' ';
		out += words[i];
	}
	return out;
}

void apply_settings(const std::vector<definition_t> &items, arena_settings_t &settings, int32_t &max_teams, bool &pickup)
{
	if (const definition_t *key = find_key("weapons", definition_type_t::value_list, items))
	{
		int32_t mask = 0;
		for (size_t i = 0; i < weapon_vals.size(); ++i)
			if (has_val(*key, G_Fmt("{}", (i == 8) ? 0 : static_cast<int>(i) + 2)))
				mask |= weapon_vals[i];
		settings.weapons = mask;
	}

	if (const definition_t *key = find_key("armor", definition_type_t::value_list, items))
		settings.armor = atoi(get_val(*key, 0));
	if (const definition_t *key = find_key("health", definition_type_t::value_list, items))
		settings.health = atoi(get_val(*key, 0));
	if (const definition_t *key = find_key("minping", definition_type_t::value_list, items))
		settings.minping = atoi(get_val(*key, 0));
	if (const definition_t *key = find_key("maxping", definition_type_t::value_list, items))
		settings.maxping = atoi(get_val(*key, 0));
	if (const definition_t *key = find_key("playersperteam", definition_type_t::value_list, items))
		settings.playersperteam = atoi(get_val(*key, 0));
	if (const definition_t *key = find_key("rounds", definition_type_t::value_list, items))
		settings.rounds = atoi(get_val(*key, 0));
	if (const definition_t *key = find_key("maxteams", definition_type_t::value_list, items))
		max_teams = atoi(get_val(*key, 0));
	if (const definition_t *key = find_key("pickup", definition_type_t::value_list, items))
		pickup = atoi(get_val(*key, 0)) != 0;
	if (const definition_t *key = find_key("rocketspeed", definition_type_t::value_list, items))
		settings.rocket_speed = atoi(get_val(*key, 0));
	if (const definition_t *key = find_key("shells", definition_type_t::value_list, items))
		settings.shells = atoi(get_val(*key, 0));
	if (const definition_t *key = find_key("bullets", definition_type_t::value_list, items))
		settings.bullets = atoi(get_val(*key, 0));
	if (const definition_t *key = find_key("slugs", definition_type_t::value_list, items))
		settings.slugs = atoi(get_val(*key, 0));
	if (const definition_t *key = find_key("grenades", definition_type_t::value_list, items))
		settings.grenades = atoi(get_val(*key, 0));
	if (const definition_t *key = find_key("rockets", definition_type_t::value_list, items))
		settings.rockets = atoi(get_val(*key, 0));
	if (const definition_t *key = find_key("cells", definition_type_t::value_list, items))
		settings.cells = atoi(get_val(*key, 0));
	if (const definition_t *key = find_key("fastswitch", definition_type_t::value_list, items))
		settings.fastswitch = atoi(get_val(*key, 0));
	if (const definition_t *key = find_key("armorprotect", definition_type_t::value_list, items))
		settings.armorprotect = atoi(get_val(*key, 0));
	if (const definition_t *key = find_key("healthprotect", definition_type_t::value_list, items))
		settings.healthprotect = atoi(get_val(*key, 0));
	if (const definition_t *key = find_key("fallingdamage", definition_type_t::value_list, items))
		settings.fallingdamage = atoi(get_val(*key, 0)) != 0;

	if (const definition_t *key = find_key("allowvotingarmor", definition_type_t::value_list, items))
		settings.allow_voting_armor = atoi(get_val(*key, 0)) != 0;
	if (const definition_t *key = find_key("allowvotinghealth", definition_type_t::value_list, items))
		settings.allow_voting_health = atoi(get_val(*key, 0)) != 0;
	if (const definition_t *key = find_key("allowvotingminping", definition_type_t::value_list, items))
		settings.allow_voting_minping = atoi(get_val(*key, 0)) != 0;
	if (const definition_t *key = find_key("allowvotingmaxping", definition_type_t::value_list, items))
		settings.allow_voting_maxping = atoi(get_val(*key, 0)) != 0;
	if (const definition_t *key = find_key("allowvotingplayersperteam", definition_type_t::value_list, items))
		settings.allow_voting_playersperteam = atoi(get_val(*key, 0)) != 0;
	if (const definition_t *key = find_key("allowvotingrounds", definition_type_t::value_list, items))
		settings.allow_voting_rounds = atoi(get_val(*key, 0)) != 0;
	if (const definition_t *key = find_key("allowvotingmaxteams", definition_type_t::value_list, items))
		settings.allow_voting_maxteams = atoi(get_val(*key, 0)) != 0;
	if (const definition_t *key = find_key("allowvotingarmorprotect", definition_type_t::value_list, items))
		settings.allow_voting_armorprotect = atoi(get_val(*key, 0)) != 0;
	if (const definition_t *key = find_key("allowvotinghealthprotect", definition_type_t::value_list, items))
		settings.allow_voting_healthprotect = atoi(get_val(*key, 0)) != 0;
	if (const definition_t *key = find_key("allowvotingshotgun", definition_type_t::value_list, items))
		settings.allow_voting_shotgun = atoi(get_val(*key, 0)) != 0;
	if (const definition_t *key = find_key("allowvotingsupershotgun", definition_type_t::value_list, items))
		settings.allow_voting_supershotgun = atoi(get_val(*key, 0)) != 0;
	if (const definition_t *key = find_key("allowvotingmachinegun", definition_type_t::value_list, items))
		settings.allow_voting_machinegun = atoi(get_val(*key, 0)) != 0;
	if (const definition_t *key = find_key("allowvotingchaingun", definition_type_t::value_list, items))
		settings.allow_voting_chaingun = atoi(get_val(*key, 0)) != 0;
	if (const definition_t *key = find_key("allowvotinggrenadelauncher", definition_type_t::value_list, items))
		settings.allow_voting_grenadelauncher = atoi(get_val(*key, 0)) != 0;
	if (const definition_t *key = find_key("allowvotingrocketlauncher", definition_type_t::value_list, items))
		settings.allow_voting_rocketlauncher = atoi(get_val(*key, 0)) != 0;
	if (const definition_t *key = find_key("allowvotinghyperblaster", definition_type_t::value_list, items))
		settings.allow_voting_hyperblaster = atoi(get_val(*key, 0)) != 0;
	if (const definition_t *key = find_key("allowvotingrailgun", definition_type_t::value_list, items))
		settings.allow_voting_railgun = atoi(get_val(*key, 0)) != 0;
	if (const definition_t *key = find_key("allowvotingbfg", definition_type_t::value_list, items))
		settings.allow_voting_bfg = atoi(get_val(*key, 0)) != 0;
	if (const definition_t *key = find_key("allowvotingfallingdamage", definition_type_t::value_list, items))
		settings.allow_voting_fallingdamage = atoi(get_val(*key, 0)) != 0;

	if (const definition_t *key = find_key("lockarena", definition_type_t::value_list, items))
		settings.locked = atoi(get_val(*key, 0)) != 0;
	if (const definition_t *key = find_key("competitionmode", definition_type_t::value_list, items))
		settings.competition = atoi(get_val(*key, 0)) != 0;
	if (const definition_t *key = find_key("damagescoring", definition_type_t::value_list, items))
		settings.scorebydamage = atoi(get_val(*key, 0)) != 0;
}

[[nodiscard]] arena_settings_t default_settings_for_map()
{
	arena_settings_t settings;
	settings.playersperteam = 1;
	settings.rounds = idmap ? 9 : 1;
	settings.weapons = 0xff;
	settings.armor = 200;
	settings.health = 100;
	settings.minping = 0;
	settings.maxping = 1000;
	settings.rocket_speed = 650;
	settings.shells = 100;
	settings.bullets = 200;
	settings.slugs = 50;
	settings.grenades = 50;
	settings.rockets = 50;
	settings.cells = 150;
	settings.fastswitch = 1;
	settings.armorprotect = 2;
	settings.healthprotect = 1;
	settings.fallingdamage = true;
	settings.allow_voting_armor = true;
	settings.allow_voting_health = true;
	settings.allow_voting_minping = true;
	settings.allow_voting_maxping = true;
	settings.allow_voting_playersperteam = true;
	settings.allow_voting_rounds = true;
	settings.allow_voting_maxteams = true;
	settings.allow_voting_armorprotect = true;
	settings.allow_voting_healthprotect = true;
	settings.allow_voting_shotgun = true;
	settings.allow_voting_supershotgun = true;
	settings.allow_voting_machinegun = true;
	settings.allow_voting_chaingun = true;
	settings.allow_voting_grenadelauncher = true;
	settings.allow_voting_rocketlauncher = true;
	settings.allow_voting_hyperblaster = true;
	settings.allow_voting_railgun = true;
	settings.allow_voting_bfg = true;
	settings.allow_voting_fallingdamage = true;
	settings.locked = false;
	settings.competition = false;
	settings.scorebydamage = false;
	settings.changed = false;
	return settings;
}

[[nodiscard]] const char *get_next_map_impl(const char *current)
{
	if (!map_loop || map_loop->values.empty())
		return nullptr;

	for (size_t i = 0; i < map_loop->values.size(); ++i)
	{
		if (map_loop->values[i] == current)
			return map_loop->values[(i + 1) % map_loop->values.size()].c_str();
	}

	return map_loop->values.front().c_str();
}

} // namespace

const std::string &ra2_gamedir()
{
	static const std::string dir = module_directory().string();
	return dir;
}

std::vector<std::string> motd_lines;
int32_t votetries_setting = 3;
std::array<int32_t, 9> weapon_vals = { 1, 2, 4, 8, 16, 32, 64, 128, 256 };

void set_config(int first, int last)
{
	if (first > last)
		return;

	first = std::max(first, 0);
	last = std::min(last, static_cast<int>(arenas.size()) - 1);

	for (int i = first; i <= last; ++i)
	{
		arena_settings_t settings = default_settings_for_map();
		int32_t max_teams = 128;
		bool pickup = idmap;

		apply_settings(definition_blocks, settings, max_teams, pickup);
		if (map_block)
			apply_settings(map_block->block, settings, max_teams, pickup);
		if (i < static_cast<int>(arena_blocks.size()) && arena_blocks[i])
			apply_settings(arena_blocks[i]->block, settings, max_teams, pickup);

		settings.changed = false;
		arenas[i].settings = settings;
		arenas[i].proposed = settings;
		arenas[i].proposetime = 0_ms;
		arenas[i].votes_yes = 0;
		arenas[i].votes_no = 0;
		arenas[i].votetries = 0;
		arenas[i].proposer = nullptr;
		arenas[i].maxteams = max_teams;
		arenas[i].idarena = pickup;
	}
}

void read_config(const std::string &config_text)
{
	clear_config_state();

	tokenizer_t tok(config_text);
	if (!parse_block(tok, definition_blocks, false))
		definition_blocks.clear();
}

void list_keys(edict_t *ent)
{
	const std::vector<definition_t> *items = &definition_blocks;
	int argc = gi.argc();

	for (int i = 1; i < argc; ++i)
	{
		const definition_t *key = find_key(gi.argv(i), definition_type_t::block, *items);
		if (!key)
		{
			const std::string message(G_Fmt("Block not found: {}\n", gi.argv(i)));
			gi.Client_Print(ent, PRINT_HIGH, message.c_str());
			return;
		}

		items = &key->block;
	}

	std::string output;
	for (const definition_t &item : *items)
	{
		output += join_words(item.names);
		output += "  ";
		if (item.type == definition_type_t::value_list)
			output += G_Fmt("V  {}\n", join_words(item.values));
		else if (item.type == definition_type_t::block)
			output += "B\n";
		else
			output += "U\n";
	}

	gi.Client_Print(ent, PRINT_HIGH, output.c_str());
}

void load_config(int num_arenas_to_load)
{
	ensure_path_cvars();
	clear_config_state();

	const text_file_t config = load_gamedir_text_file(arenacfg->string);
	if (!config.text)
	{
		const std::string message(G_Fmt("Error: Couldn't read {}\n", config.tried));
		gi.Com_Print(message.c_str());
		return;
	}

	read_config(*config.text);

	arena_blocks.assign(std::max(num_arenas_to_load, 0), nullptr);
	map_block = find_key(level.mapname, definition_type_t::block, definition_blocks);
	if (map_block)
	{
		const std::string message(G_Fmt("arena.cfg info for map found: {}\n", level.mapname));
		gi.Com_Print(message.c_str());
		for (int i = 0; i < num_arenas_to_load; ++i)
		{
			const std::string index(G_Fmt("{}", i));
			arena_blocks[i] = find_key(index, definition_type_t::block, map_block->block);
		}
	}
	else
	{
		const std::string message(G_Fmt("arena.cfg info for map not found: {}\n", level.mapname));
		gi.Com_Print(message.c_str());
	}

	if (const definition_t *key = find_key("votetries", definition_type_t::value_list, definition_blocks))
		votetries_setting = atoi(get_val(*key, 0));
	if (const definition_t *key = find_key("grapple", definition_type_t::value_list, definition_blocks))
		allow_grapple = atoi(get_val(*key, 0)) != 0;

	map_loop = find_key("maploop", definition_type_t::value_list, definition_blocks);
	if (map_loop)
	{
		gi.Com_Print("Map loop read\n");

		if (const char *next_map = get_next_map_impl(level.mapname))
			Q_strlcpy(level.forcemap, next_map, sizeof(level.forcemap));
	}
}

const char *get_next_map(const char *current)
{
	return get_next_map_impl(current);
}

void print_map_loop(edict_t *ent)
{
	if (map_loop)
	{
		const std::string message(G_Fmt("{}\n", join_words(map_loop->values)));
		gi.Client_Print(ent, PRINT_MEDIUM, message.c_str());
	}
	else
		gi.Client_Print(ent, PRINT_MEDIUM, "No map loop set\n");
}

void load_motd()
{
	motd_lines.clear();

	const text_file_t motd = load_gamedir_text_file("motd.txt");
	if (!motd.text)
	{
		const std::string message(G_Fmt("Error: Couldn't read {}\n", motd.tried));
		gi.Com_Print(message.c_str());
		return;
	}

	{
		const std::string message(G_Fmt("Sucessfully read {}\n", motd.path.string()));
		gi.Com_Print(message.c_str());
	}

	std::istringstream stream(*motd.text);
	std::string line;
	while (std::getline(stream, line))
	{
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		motd_lines.push_back(line);
	}
}
