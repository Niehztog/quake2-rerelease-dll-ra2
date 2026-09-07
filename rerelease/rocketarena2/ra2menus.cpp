// Copyright (c) ZeniMax Media Inc.
// Licensed under the GNU General Public License 2.0.

// rocketarena2/ra2menus.cpp -- Rocket Arena 2 menu content.

#include "../g_local.h"
#include "arena.h"
#include "ra2_menu.h"

#include <cstring>
#include <string>
#include <string_view>

const char *get_next_map(const char *current); // matches arena.h; kept for clarity at call sites in this file
void Cmd_arenaadmin_f(edict_t *ent, int32_t mode);
void show_arena_menu(edict_t *ent);
void show_observer_menu(edict_t *ent);
void show_teamconfirm_menu(edict_t *ent, int32_t arenanum);
void Cmd_menuhelp_f(edict_t *ent);
void Cmd_admin_f(edict_t *ent);

namespace
{
[[nodiscard]] const char *StringForProtect(int32_t protect)
{
	switch (protect)
	{
	case 0: return "Damage all          ";
	case 1: return "Dont damage team    ";
	case 2: return "Damage self not team";
	default: return "Damage all          ";
	}
}

[[nodiscard]] int32_t NumForProtect(std::string_view text)
{
	if (text == "Damage all          ")
		return 0;
	if (text == "Dont damage team    ")
		return 1;
	if (text == "Damage self not team")
		return 2;
	return 0;
}

[[nodiscard]] menuitem_t *item_data(qmenu_t *item)
{
	return MENUITEM(item);
}

void set_cvar_value(const char *name, int32_t value)
{
	const std::string value_text = std::string(G_Fmt("{}", value));
	gi.cvar_set(name, value_text.c_str());
}

[[nodiscard]] qmenu_t *find_item(qmenu_t *menu, std::string_view text)
{
	for (qmenu_t *node = MENU_INFO(menu)->items; node; node = node->next)
		if (item_data(node)->text == text)
			return node;
	return nullptr;
}

constexpr size_t MESSAGE_LINE_BYTES = 27;

[[nodiscard]] size_t utf8_sequence_length(std::string_view text, size_t pos)
{
	const uint8_t lead = static_cast<uint8_t>(text[pos]);
	size_t length = 1;

	if (lead >= 0xC2 && lead <= 0xDF)
		length = 2;
	else if (lead >= 0xE0 && lead <= 0xEF)
		length = 3;
	else if (lead >= 0xF0 && lead <= 0xF4)
		length = 4;
	else
		return length;

	if (pos + length > text.size())
		return 1;

	for (size_t i = 1; i < length; ++i)
		if ((static_cast<uint8_t>(text[pos + i]) & 0xC0) != 0x80)
			return 1;

	return length;
}

[[nodiscard]] size_t utf8_prefix_length(std::string_view text, size_t max_bytes)
{
	size_t length = 0;
	while (length < text.size())
	{
		const size_t sequence_length = utf8_sequence_length(text, length);
		if (length + sequence_length > max_bytes)
			break;
		length += sequence_length;
	}

	return length;
}

void add_wrapped_message_rows(qmenu_t *menu, std::string_view source)
{
	std::string line;
	size_t last_space = std::string::npos;

	for (size_t pos = 0; pos < source.size();)
	{
		const size_t sequence_length = utf8_sequence_length(source, pos);
		std::string_view sequence = source.substr(pos, sequence_length);
		pos += sequence_length;

		if (sequence == "\n")
			sequence = " ";

		line.append(sequence.data(), sequence.size());
		if (sequence == " ")
			last_space = line.size() - 1;

		if (line.size() < MESSAGE_LINE_BYTES)
			continue;

		const bool can_break_at_space = last_space != std::string::npos && last_space < MESSAGE_LINE_BYTES;
		const size_t cut = can_break_at_space ? last_space : utf8_prefix_length(line, MESSAGE_LINE_BYTES);
		const size_t erase_to = can_break_at_space ? cut + 1 : cut;

		AddMenuItem(menu, line.substr(0, cut).c_str(), nullptr, -1, nullptr);
		line.erase(0, erase_to);
		last_space = line.rfind(' ');
	}

	AddMenuItem(menu, line.c_str(), nullptr, -1, nullptr);
}

struct settings_view_t
{
	arena_settings_t &settings;
	int32_t weapons = 0;

	void preserve_if_locked(bool allow_voting, int32_t bit)
	{
		if (!allow_voting)
			weapons |= (settings.weapons & bit) ? bit : 0;
	}

	void finalize_weapons()
	{
		settings.weapons = weapons;
	}
};

int32_t menuLeaveArena(edict_t *ent, qmenu_t *menu, qmenu_t *item, int32_t arg)
{
	(void) menu;
	(void) item;
	(void) arg;

	team_t *team = TEAM(&teams[ent->client->resp.teamnum]);
	arena_state_t state = arenas[team->arenanum].state;
	if (state != ASTATE_COUNTDOWN && state != ASTATE_ROUNDEND && ent->takedamage)
	{
		menu_centerprint(ent, "Sorry, you cannot leave the arena\nduring a match");
		return MSELECT_HANDLED;
	}

	remove_from_queue(&team->arenalink, nullptr);
	SendTeamToArena(&teams[ent->client->resp.teamnum], 0, true, true);
	return MSELECT_CLOSE;
}

int32_t menuAddtoArena(edict_t *ent, qmenu_t *menu, qmenu_t *item, int32_t arg)
{
	int32_t arenanum = 0;
	for (qmenu_t *node = MENU_INFO(menu)->items; node; node = node->next)
	{
		++arenanum;
		if (node == item)
			break;
	}

	if (!arenanum)
		return MSELECT_CLOSE;

	if (arg == 1)
		return AddtoArena(ent, arenanum, 0, false);

	return AddtoArena(ent, arenanum, 1, true);
}

int32_t menuLeaveTeamAr(edict_t *ent, qmenu_t *menu, qmenu_t *item, int32_t arg)
{
	(void) menu;
	(void) item;
	(void) arg;

	team_t *team = TEAM(&teams[ent->client->resp.teamnum]);
	arena_state_t state = arenas[team->arenanum].state;
	if (state != ASTATE_COUNTDOWN && state != ASTATE_ROUNDEND && ent->takedamage)
	{
		menu_centerprint(ent, "Sorry, you cannot leave the arena\nduring a match");
		return MSELECT_HANDLED;
	}

	remove_from_team(ent);
	move_to_arena(ent, 0, 1);
	init_player(ent);
	return MSELECT_CLOSE;
}

int32_t menuLeaveTeam(edict_t *ent, qmenu_t *menu, qmenu_t *item, int32_t arg)
{
	(void) menu;
	(void) item;
	(void) arg;
	remove_from_team(ent);
	init_player(ent);
	return MSELECT_CLOSE;
}

int32_t menuStepInOutofLine(edict_t *ent, qmenu_t *menu, qmenu_t *item, int32_t arg)
{
	(void) menu;
	(void) item;
	(void) arg;

	const int32_t arenanum = ent->client->resp.context;
	team_t *team = TEAM(&teams[ent->client->resp.teamnum]);
	const int32_t wasinline = !team->outofline;

	if (menuLeaveArena(ent, nullptr, nullptr, 0) == MSELECT_CLOSE)
		return AddtoArena(ent, arenanum, 1, wasinline != 0);

	return MSELECT_HANDLED;
}

int32_t menuShowSettingsPropose(edict_t *ent, qmenu_t *menu, qmenu_t *item, int32_t arg)
{
	(void) menu;
	(void) item;
	(void) arg;

	arena_t &arena = arenas[ent->client->resp.context];
	if (arena.proposetime > level.time)
	{
		const int32_t remaining = (arena.proposetime - level.time).seconds<int32_t>();
		if (remaining < 30)
		{
			const std::string msg = std::string(G_Fmt("Voting is in progress.\nPlease wait {} seconds", remaining));
			menu_centerprint(ent, msg.c_str());
		}
		else
			menu_centerprint(ent, "Voting is in progress.\nPlease wait");
		return MSELECT_HANDLED;
	}

	if (ent->client->resp.votes == 0)
	{
		const std::string msg = std::string(G_Fmt("Sorry, you cannot propose any more changes.\nYou have already proposed {} times\n", votetries_setting));
		menu_centerprint(ent, msg.c_str());
		return MSELECT_HANDLED;
	}

	ent->client->resp.votes--;
	Cmd_arenaadmin_f(ent, 1);
	return MSELECT_HANDLED;
}

int32_t menuShowSettingsVote(edict_t *ent, qmenu_t *menu, qmenu_t *item, int32_t arg)
{
	(void) menu;
	(void) item;
	(void) arg;

	arena_t &arena = arenas[ent->client->resp.context];
	if (arena.proposetime < level.time)
	{
		menu_centerprint(ent, "No changes have been proposed");
		return MSELECT_HANDLED;
	}

	if (ent->client->resp.ra2_voted)
	{
		menu_centerprint(ent, "You have already voted");
		return MSELECT_HANDLED;
	}

	Cmd_arenaadmin_f(ent, 2);
	return MSELECT_HANDLED;
}

int32_t menuAddtoTeam(edict_t *ent, qmenu_t *menu, qmenu_t *item, int32_t arg)
{
	(void) menu;
	(void) arg;

	if (add_to_team(ent, item_data(item)->text.c_str()))
	{
		team_t *team = TEAM(&teams[ent->client->resp.teamnum]);
		if (!team->arenanum)
		{
			show_arena_menu(ent);
			return MSELECT_CLOSE;
		}

		ent->client->resp.fightstate = FIGHT_SPECTATING;
		ent->takedamage = false;
		move_to_arena(ent, team->arenanum, 1);
		return MSELECT_CLOSE;
	}

	menu_centerprint(ent, "That team is already in an arena\nand full or\nthe arena is locked");
	return MSELECT_HANDLED;
}

int32_t menuNewTeam(edict_t *ent, qmenu_t *menu, qmenu_t *item, int32_t arg)
{
	(void) menu;
	(void) item;
	(void) arg;

	if (!create_own_team(ent))
	{
		menu_centerprint(ent, "Sorry, there are no free team slots");
		return MSELECT_HANDLED;
	}

	show_arena_menu(ent);
	return MSELECT_CLOSE;
}

int32_t menuChangeValue10AZ(edict_t *ent, qmenu_t *menu, qmenu_t *item, int32_t arg)
{
	(void) ent;
	(void) menu;
	auto *it = item_data(item);
	it->num += arg ? 10 : -10;
	if (it->num < 0)
		it->num = 0;
	return MSELECT_REDRAW;
}

int32_t menuChangeValue50AZ(edict_t *ent, qmenu_t *menu, qmenu_t *item, int32_t arg)
{
	(void) ent;
	(void) menu;
	auto *it = item_data(item);
	it->num += arg ? 50 : -50;
	if (it->num < 0)
		it->num = 0;
	return MSELECT_REDRAW;
}

int32_t menuChangeValue50(edict_t *ent, qmenu_t *menu, qmenu_t *item, int32_t arg)
{
	(void) ent;
	(void) menu;
	auto *it = item_data(item);
	it->num += arg ? 50 : -50;
	if (it->num <= 0)
		it->num = 50;
	return MSELECT_REDRAW;
}

int32_t menuChangeValue(edict_t *ent, qmenu_t *menu, qmenu_t *item, int32_t arg)
{
	(void) ent;
	(void) menu;
	auto *it = item_data(item);
	it->num += arg ? 1 : -1;
	if (it->num == 0)
		it->num = 1;
	return MSELECT_REDRAW;
}

int32_t menuChangeYesNo(edict_t *ent, qmenu_t *menu, qmenu_t *item, int32_t arg)
{
	(void) ent;
	(void) menu;
	(void) arg;
	auto *it = item_data(item);
	it->value = (!it->value.empty() && it->value[0] == 'Y') ? "NO " : "YES";
	return MSELECT_REDRAW;
}

int32_t menuChangeProtect(edict_t *ent, qmenu_t *menu, qmenu_t *item, int32_t arg)
{
	(void) ent;
	(void) menu;
	int32_t protect = NumForProtect(item_data(item)->value);
	protect += arg ? 1 : -1;
	if (protect < 0)
		protect = 2;
	if (protect > 2)
		protect = 0;
	item_data(item)->value = StringForProtect(protect);
	return MSELECT_REDRAW;
}

int32_t menuChangeMap(edict_t *ent, qmenu_t *menu, qmenu_t *item, int32_t arg)
{
	(void) ent;
	(void) menu;
	(void) arg;
	if (const char *next_map = get_next_map(item_data(item)->value.c_str()))
		item_data(item)->value = next_map;
	return MSELECT_REDRAW;
}

int32_t menuApplyAdmin(edict_t *ent, qmenu_t *menu, qmenu_t *item, int32_t arg)
{
	(void) ent;
	(void) item;
	(void) arg;

	const qmenu_t *frag = find_item(menu, "Fraglimit:        ");
	const qmenu_t *time = find_item(menu, "Timelimit:        ");
	const qmenu_t *map = find_item(menu, "Mapname:          ");
	if (frag)
		set_cvar_value("fraglimit", item_data(const_cast<qmenu_t *>(frag))->num);
	if (time)
		set_cvar_value("timelimit", item_data(const_cast<qmenu_t *>(time))->num);

	// no map row means nothing to change level to, so the fraglimit/timelimit
	// writes above stand on their own and the level runs on -- the original
	// returns here rather than ending the map with a null destination
	if (!map)
		return MSELECT_CLOSE;

	const std::string &map_name = item_data(const_cast<qmenu_t *>(map))->value;
	edict_t *e = G_Spawn();
	e->classname = "target_changelevel";
	char *map_copy = static_cast<char *>(gi.TagMalloc(map_name.size() + 1, TAG_LEVEL));
	std::strcpy(map_copy, map_name.c_str());
	e->map = map_copy;
	BeginIntermission(e);
	return MSELECT_CLOSE;
}

int32_t menuCancel(edict_t *ent, qmenu_t *menu, qmenu_t *item, int32_t arg)
{
	(void) ent;
	(void) menu;
	(void) item;
	(void) arg;
	return MSELECT_CLOSE;
}

int32_t menuApplyArenaAdmin(edict_t *ent, qmenu_t *menu, qmenu_t *item, int32_t arg)
{
	(void) arg;
	qmenu_t *arena_item = find_item(menu, "Arena:                 ");
	if (!arena_item)
		return MSELECT_CLOSE;

	const int32_t arenanum = item_data(arena_item)->num;
	arena_t &arena = arenas[arenanum];
	const bool applying_live = !item_data(item)->text.empty() && item_data(item)->text[0] == 'A';

	arena_settings_t *settings = &arena.settings;
	if (!applying_live)
	{
		if (arena.proposetime > level.time)
		{
			const std::string msg = std::string(G_Fmt("Voting is in progress.\nPlease wait {} seconds", (arena.proposetime - level.time).seconds<int32_t>()));
			menu_centerprint(ent, msg.c_str());
			return MSELECT_HANDLED;
		}

		arena.proposed = arena.settings;
		settings = &arena.proposed;
		start_voting(ent, arenanum);
		arena.votes_yes++;
		ent->client->resp.ra2_voted = true;
	}

	settings_view_t view{ *settings };
	settings->changed = true;
	view.preserve_if_locked(settings->allow_voting_shotgun, weapon_vals[0]);
	view.preserve_if_locked(settings->allow_voting_supershotgun, weapon_vals[1]);
	view.preserve_if_locked(settings->allow_voting_machinegun, weapon_vals[2]);
	view.preserve_if_locked(settings->allow_voting_chaingun, weapon_vals[3]);
	view.preserve_if_locked(settings->allow_voting_grenadelauncher, weapon_vals[4]);
	view.preserve_if_locked(settings->allow_voting_rocketlauncher, weapon_vals[5]);
	view.preserve_if_locked(settings->allow_voting_hyperblaster, weapon_vals[6]);
	view.preserve_if_locked(settings->allow_voting_railgun, weapon_vals[7]);
	view.preserve_if_locked(settings->allow_voting_bfg, weapon_vals[8]);

	for (qmenu_t *node = MENU_INFO(menu)->items; node; node = node->next)
	{
		menuitem_t *it = item_data(node);
		if (it->text == "Players per team:      ")
			settings->playersperteam = it->num;
		else if (it->text == "Initial Health:        ")
			settings->health = it->num;
		else if (it->text == "Initial Armor:         ")
			settings->armor = it->num;
		else if (it->text == "Minimum Ping:          ")
			settings->minping = it->num;
		else if (it->text == "Maximum Ping:          ")
			settings->maxping = it->num;
		else if (it->text == "Rounds:                ")
			settings->rounds = (it->num / 2) * 2 + 1;
		else if (it->text == "Allow Shotgun:         " && !it->value.empty() && it->value[0] == 'Y')
			view.weapons |= weapon_vals[0];
		else if (it->text == "Allow Super Shotgun:   " && !it->value.empty() && it->value[0] == 'Y')
			view.weapons |= weapon_vals[1];
		else if (it->text == "Allow Machine gun:     " && !it->value.empty() && it->value[0] == 'Y')
			view.weapons |= weapon_vals[2];
		else if (it->text == "Allow Chain gun:       " && !it->value.empty() && it->value[0] == 'Y')
			view.weapons |= weapon_vals[3];
		else if (it->text == "Allow Grenade Launcher:" && !it->value.empty() && it->value[0] == 'Y')
			view.weapons |= weapon_vals[4];
		else if (it->text == "Allow Rocket Launcher: " && !it->value.empty() && it->value[0] == 'Y')
			view.weapons |= weapon_vals[5];
		else if (it->text == "Allow Hyperblaster:    " && !it->value.empty() && it->value[0] == 'Y')
			view.weapons |= weapon_vals[6];
		else if (it->text == "Allow Railgun:         " && !it->value.empty() && it->value[0] == 'Y')
			view.weapons |= weapon_vals[7];
		else if (it->text == "Allow BFG10K:          " && !it->value.empty() && it->value[0] == 'Y')
			view.weapons |= weapon_vals[8];
		else if (it->text == "Health: ")
			settings->healthprotect = NumForProtect(it->value);
		else if (it->text == "Armor:  ")
			settings->armorprotect = NumForProtect(it->value);
		else if (it->text == "Falling Damage:        ")
			settings->fallingdamage = !it->value.empty() && it->value[0] == 'Y';
		else if (it->text == "Lock Arena:            ")
			settings->locked = !it->value.empty() && it->value[0] == 'Y';
		else if (it->text == "Competition Mode:      ")
			settings->competition = !it->value.empty() && it->value[0] == 'Y';
		else if (it->text == "Damage Scoring:        ")
			settings->scorebydamage = !it->value.empty() && it->value[0] == 'Y';
	}

	view.finalize_weapons();
	check_teams(arenanum);
	return MSELECT_CLOSE;
}

int32_t menuVote(edict_t *ent, qmenu_t *menu, qmenu_t *item, int32_t arg)
{
	(void) menu;
	(void) arg;
	arena_t &arena = arenas[ent->client->resp.context];
	if (arena.proposetime < level.time)
	{
		menu_centerprint(ent, "Sorry, voting is over");
		return MSELECT_HANDLED;
	}

	if (ent->client->resp.ra2_voted)
	{
		menu_centerprint(ent, "You have already voted");
		return MSELECT_HANDLED;
	}

	if (!item_data(item)->value.empty() && item_data(item)->value[0] == 'Y')
		arena.votes_yes++;
	else
		arena.votes_no++;

	ent->client->resp.ra2_voted = true;
	return MSELECT_CLOSE;
}

int32_t menuMotdContinue(edict_t *ent, qmenu_t *menu, qmenu_t *item, int32_t arg)
{
	(void) menu;
	(void) item;
	(void) arg;
	ent->client->pers.showmotd = false;
	menuRefreshTeamList(ent, nullptr, nullptr, 0);
	return MSELECT_CLOSE;
}

int32_t menuNo(edict_t *ent, qmenu_t *menu, qmenu_t *item, int32_t arg)
{
	(void) ent;
	(void) menu;
	(void) item;
	(void) arg;
	return MSELECT_CLOSE;
}

int32_t menuTeamConfirm(edict_t *ent, qmenu_t *menu, qmenu_t *item, int32_t arg)
{
	(void) menu;
	(void) arg;

	// Report AddtoArena's own verdict instead of swallowing it. On success it has
	// replaced this screen with the observer menu, so this one has to go -- the
	// alternative left a dead "Confirmation" buried on the stack, which nothing
	// noticed while only Cancel rows walked it and escape does now. On a refusal
	// it has pushed a message instead, and this screen is what belongs under it.
	return AddtoArena(ent, item_data(item)->num, 1, false) ? MSELECT_REDRAW : MSELECT_CLOSE;
}

} // namespace

const char *getarenaname(int32_t arenanum)
{
	edict_t *spot = nullptr;
	while ((spot = G_FindByString<&edict_t::classname>(spot, "info_player_intermission")) != nullptr)
		if (spot->arena == arenanum && spot->message)
			return spot->message;

	// A scratch buffer, the way the original's va() was one. Every caller copies
	// the result immediately (AddMenuItem into a std::string, multi_trigger into
	// its own TAG_LEVEL block), and TagMalloc'ing a fresh block here instead
	// leaked one per call -- show_arena_menu asks for every arena's name each
	// time the menu is rebuilt, which is every refresh, all map long.
	static char name[64];
	G_FmtTo(name, "Arena Number {}", arenanum);
	return name;
}

void show_observer_menu(edict_t *ent)
{
	qmenu_t *m = CreateQMenu(ent, "Observer Options");
	team_t *team = TEAM(&teams[ent->client->resp.teamnum]);

	if (!team->outofline)
	{
		AddMenuItem(m, "Change Arena Settings", nullptr, -1, menuShowSettingsPropose);
		AddMenuItem(m, "Vote on Changes", nullptr, -1, menuShowSettingsVote);
		AddMenuItem(m, "", nullptr, -1, nullptr);
	}

	if (!arenas[ent->client->resp.context].idarena)
	{
		const std::string step_text = std::string(G_Fmt("Step {} Line", team->outofline ? "into" : "out of"));
		AddMenuItem(m, step_text.c_str(), nullptr, -1, menuStepInOutofLine);
		AddMenuItem(m, "", nullptr, -1, nullptr);
	}

	AddMenuItem(m, "Leave Team", nullptr, -1, menuLeaveTeamAr);
	if (!arenas[ent->client->resp.context].idarena)
		AddMenuItem(m, "Leave Arena", nullptr, -1, menuLeaveArena);
	FinishMenu(ent, m, false);
}

void show_arena_menu(edict_t *ent)
{
	qmenu_t *m = CreateQMenu(ent, "Choose Your Arena");
	for (int32_t i = 1; i <= num_arenas; i++)
	{
		if (arenas[i].idarena)
			AddMenuItem(m, getarenaname(i), " (PT)", -1, menuAddtoArena);
		else
			AddMenuItem(m, getarenaname(i), " T:", count_queue(&arenas[i].waitingteams) + count_queue(&arenas[i].activeteams), menuAddtoArena);
	}

	AddMenuItem(m, "", nullptr, -1, nullptr);
	AddMenuItem(m, "Leave Team", nullptr, -1, menuLeaveTeam);
	FinishMenu(ent, m, true);
}

int32_t menuRefreshTeamList(edict_t *ent, qmenu_t *menu, qmenu_t *item, int32_t arg)
{
	(void) menu;
	(void) item;
	(void) arg;

	qmenu_t *m = CreateQMenu(ent, "Choose your team");
	AddMenuItem(m, "Start New Team", nullptr, -1, menuNewTeam);
	for (int32_t i = 0; i < MAX_TEAMS; i++)
		if (teams[i].it)
			AddMenuItem(m, TEAM(&teams[i])->name.c_str(), " Players: ", count_queue(&teams[i]), menuAddtoTeam);
	AddMenuItem(m, "Refresh List", nullptr, -1, menuRefreshTeamList);
	AddMenuItem(m, "", nullptr, -1, nullptr);
	AddMenuItem(m, "Confused? try /cmd menuhelp", nullptr, -1, nullptr);
	FinishMenu(ent, m, true);

	// a refresh replaces this screen, it does not descend into a new one: without
	// the close the superseded copy stayed on the stack and every press of the
	// row buried another one. init_player calls this directly and ignores it.
	return MSELECT_CLOSE;
}

void Cmd_menuhelp_f(edict_t *ent)
{
	gi.LocClient_Print(ent, PRINT_HIGH, "H| Use invprev and invnext ([ and ])\nH| to navigate the menu\nH| invuse (ENTER) selects\nH| inven (TAB) toggles it on/off\n");
}

void Cmd_admin_f(edict_t *ent)
{
	if (admincode->value == 0)
		return;

	const int32_t code = std::atoi(gi.argv(1));
	if (static_cast<float>(code) != admincode->value)
	{
		gi.LocClient_Print(ent, PRINT_HIGH, "Sorry, incorrect admin code\n");
		return;
	}

	qmenu_t *m = CreateQMenu(ent, "Admin Menu");
	AddMenuItem(m, "Fraglimit:        ", nullptr, fraglimit->integer, menuChangeValue10AZ);
	AddMenuItem(m, "Timelimit:        ", nullptr, static_cast<int32_t>(timelimit->value), menuChangeValue10AZ);
	qmenu_t *map_item = AddMenuItem(m, "Mapname:          ", "                                ", -1, menuChangeMap);
	item_data(map_item)->value = level.mapname;
	AddMenuItem(m, "", nullptr, -1, nullptr);
	AddMenuItem(m, "Apply", nullptr, -1, menuApplyAdmin);
	AddMenuItem(m, "Cancel", nullptr, -1, menuCancel);
	FinishMenu(ent, m, true);
}

void Cmd_arenaadmin_f(edict_t *ent, int32_t mode)
{
	qmenu_t *m = nullptr;
	arena_settings_t *live = nullptr;
	arena_settings_t *prop = nullptr;
	menuselect_t changevalue = nullptr;
	menuselect_t changevalue50 = nullptr;
	menuselect_t changevalue50az = nullptr;
	menuselect_t changeyesno = nullptr;
	menuselect_t changeprotect = nullptr;
	int32_t arenanum = 0;

	switch (mode)
	{
	case 0:
		if (admincode->value == 0)
			return;
		if (static_cast<float>(std::atoi(gi.argv(1))) != admincode->value)
			return;
		arenanum = std::atoi(gi.argv(2));
		// falls through: case 1 substitutes resp.context when no arena was given
	case 1:
		if (!arenanum)
			arenanum = ent->client->resp.context;
		if (arenanum < 1 || arenanum > num_arenas)
			return;
		live = &arenas[arenanum].settings;
		prop = &arenas[arenanum].proposed;
		changevalue = menuChangeValue;
		changevalue50 = menuChangeValue50;
		changevalue50az = menuChangeValue50AZ;
		changeyesno = menuChangeYesNo;
		changeprotect = menuChangeProtect;
		break;

	case 2:
		arenanum = ent->client->resp.context;
		if (arenanum < 1 || arenanum > num_arenas)
			return;
		live = &arenas[arenanum].settings;
		prop = &arenas[arenanum].proposed;
		m = CreateQMenu(ent, "Proposed Changes");
		AddMenuItem(m, "Arena:                 ", nullptr, arenanum, nullptr);
		if (!arenas[arenanum].idarena && prop->playersperteam != live->playersperteam)
			AddMenuItem(m, "Players per team:      ", nullptr, prop->playersperteam, nullptr);
		if (prop->health != live->health)
			AddMenuItem(m, "Initial Health:        ", nullptr, prop->health, nullptr);
		if (prop->armor != live->armor)
			AddMenuItem(m, "Initial Armor:         ", nullptr, prop->armor, nullptr);
		if (!arenas[arenanum].idarena)
		{
			if (prop->minping != live->minping)
				AddMenuItem(m, "Minimum Ping:          ", nullptr, prop->minping, nullptr);
			if (prop->maxping != live->maxping)
				AddMenuItem(m, "Maximum Ping:          ", nullptr, prop->maxping, nullptr);
		}
		if (prop->rounds != live->rounds)
			AddMenuItem(m, "Rounds:                ", nullptr, prop->rounds, nullptr);
		if ((prop->weapons & weapon_vals[0]) != (live->weapons & weapon_vals[0]))
			AddMenuItem(m, "Allow Shotgun:         ", (prop->weapons & weapon_vals[0]) ? "YES" : "NO ", -1, nullptr);
		if ((prop->weapons & weapon_vals[1]) != (live->weapons & weapon_vals[1]))
			AddMenuItem(m, "Allow Super Shotgun:   ", (prop->weapons & weapon_vals[1]) ? "YES" : "NO ", -1, nullptr);
		if ((prop->weapons & weapon_vals[2]) != (live->weapons & weapon_vals[2]))
			AddMenuItem(m, "Allow Machine gun:     ", (prop->weapons & weapon_vals[2]) ? "YES" : "NO ", -1, nullptr);
		if ((prop->weapons & weapon_vals[3]) != (live->weapons & weapon_vals[3]))
			AddMenuItem(m, "Allow Chain gun:       ", (prop->weapons & weapon_vals[3]) ? "YES" : "NO ", -1, nullptr);
		if ((prop->weapons & weapon_vals[4]) != (live->weapons & weapon_vals[4]))
			AddMenuItem(m, "Allow Grenade Launcher:", (prop->weapons & weapon_vals[4]) ? "YES" : "NO ", -1, nullptr);
		if ((prop->weapons & weapon_vals[5]) != (live->weapons & weapon_vals[5]))
			AddMenuItem(m, "Allow Rocket Launcher: ", (prop->weapons & weapon_vals[5]) ? "YES" : "NO ", -1, nullptr);
		if ((prop->weapons & weapon_vals[6]) != (live->weapons & weapon_vals[6]))
			AddMenuItem(m, "Allow Hyperblaster:    ", (prop->weapons & weapon_vals[6]) ? "YES" : "NO ", -1, nullptr);
		if ((prop->weapons & weapon_vals[7]) != (live->weapons & weapon_vals[7]))
			AddMenuItem(m, "Allow Railgun:         ", (prop->weapons & weapon_vals[7]) ? "YES" : "NO ", -1, nullptr);
		if ((prop->weapons & weapon_vals[8]) != (live->weapons & weapon_vals[8]))
			AddMenuItem(m, "Allow BFG10K:          ", (prop->weapons & weapon_vals[8]) ? "YES" : "NO ", -1, nullptr);
		if (prop->healthprotect != live->healthprotect)
			AddMenuItem(m, "Health: ", StringForProtect(prop->healthprotect), -1, nullptr);
		if (prop->armorprotect != live->armorprotect)
			AddMenuItem(m, "Armor:  ", StringForProtect(prop->armorprotect), -1, nullptr);
		if (prop->fallingdamage != live->fallingdamage)
			AddMenuItem(m, "Falling Damage:        ", prop->fallingdamage ? "YES" : "NO ", -1, nullptr);
		if (prop->competition != live->competition)
			AddMenuItem(m, "Competition Mode:      ", prop->competition ? "YES" : "NO ", -1, nullptr);
		if (prop->scorebydamage != live->scorebydamage)
			AddMenuItem(m, "Damage Scoring:        ", prop->scorebydamage ? "YES" : "NO ", -1, nullptr);
		AddMenuItem(m, "", nullptr, -1, nullptr);
		break;
	default:
		return;
	}

	if (mode != 2)
	{
		m = CreateQMenu(ent, "Arena Admin Menu");
		AddMenuItem(m, "Arena:                 ", nullptr, arenanum, nullptr);
		if (!arenas[arenanum].idarena && (mode == 0 || live->allow_voting_playersperteam))
			AddMenuItem(m, "Players per team:      ", nullptr, live->playersperteam, changevalue);
		if (mode == 0 || live->allow_voting_health)
			AddMenuItem(m, "Initial Health:        ", nullptr, live->health, changevalue50);
		if (mode == 0 || live->allow_voting_armor)
			AddMenuItem(m, "Initial Armor:         ", nullptr, live->armor, changevalue50az);
		if (!arenas[arenanum].idarena)
		{
			if (mode == 0 || live->allow_voting_minping)
				AddMenuItem(m, "Minimum Ping:          ", nullptr, live->minping, changevalue50az);
			if (mode == 0 || live->allow_voting_maxping)
				AddMenuItem(m, "Maximum Ping:          ", nullptr, live->maxping, changevalue50az);
		}
		if (mode == 0 || live->allow_voting_rounds)
			AddMenuItem(m, "Rounds:                ", nullptr, live->rounds, changevalue);
		if (mode == 0 || live->allow_voting_shotgun)
			AddMenuItem(m, "Allow Shotgun:         ", (live->weapons & weapon_vals[0]) ? "YES" : "NO ", -1, changeyesno);
		if (mode == 0 || live->allow_voting_supershotgun)
			AddMenuItem(m, "Allow Super Shotgun:   ", (live->weapons & weapon_vals[1]) ? "YES" : "NO ", -1, changeyesno);
		if (mode == 0 || live->allow_voting_machinegun)
			AddMenuItem(m, "Allow Machine gun:     ", (live->weapons & weapon_vals[2]) ? "YES" : "NO ", -1, changeyesno);
		if (mode == 0 || live->allow_voting_chaingun)
			AddMenuItem(m, "Allow Chain gun:       ", (live->weapons & weapon_vals[3]) ? "YES" : "NO ", -1, changeyesno);
		if (mode == 0 || live->allow_voting_grenadelauncher)
			AddMenuItem(m, "Allow Grenade Launcher:", (live->weapons & weapon_vals[4]) ? "YES" : "NO ", -1, changeyesno);
		if (mode == 0 || live->allow_voting_rocketlauncher)
			AddMenuItem(m, "Allow Rocket Launcher: ", (live->weapons & weapon_vals[5]) ? "YES" : "NO ", -1, changeyesno);
		if (mode == 0 || live->allow_voting_hyperblaster)
			AddMenuItem(m, "Allow Hyperblaster:    ", (live->weapons & weapon_vals[6]) ? "YES" : "NO ", -1, changeyesno);
		if (mode == 0 || live->allow_voting_railgun)
			AddMenuItem(m, "Allow Railgun:         ", (live->weapons & weapon_vals[7]) ? "YES" : "NO ", -1, changeyesno);
		if (mode == 0 || live->allow_voting_bfg)
			AddMenuItem(m, "Allow BFG10K:          ", (live->weapons & weapon_vals[8]) ? "YES" : "NO ", -1, changeyesno);
		if (mode == 0 || live->allow_voting_healthprotect)
			AddMenuItem(m, "Health: ", StringForProtect(live->healthprotect), -1, changeprotect);
		if (mode == 0 || live->allow_voting_armorprotect)
			AddMenuItem(m, "Armor:  ", StringForProtect(live->armorprotect), -1, changeprotect);
		if (mode == 0 || live->allow_voting_fallingdamage)
			AddMenuItem(m, "Falling Damage:        ", live->fallingdamage ? "YES" : "NO ", -1, changeyesno);
		if (mode == 0)
			AddMenuItem(m, "Lock Arena:            ", live->locked ? "YES" : "NO ", -1, changeyesno);
		AddMenuItem(m, "Competition Mode:      ", live->competition ? "YES" : "NO ", -1, changeyesno);
		AddMenuItem(m, "Damage Scoring:        ", live->scorebydamage ? "YES" : "NO ", -1, changeyesno);
		AddMenuItem(m, "", nullptr, -1, nullptr);
	}

	switch (mode)
	{
	case 0:
		AddMenuItem(m, "Apply", nullptr, -1, menuApplyArenaAdmin);
		[[fallthrough]];
	case 1:
		AddMenuItem(m, "Propose", nullptr, -1, menuApplyArenaAdmin);
		break;
	case 2:
		AddMenuItem(m, "Vote ", "Yes", -1, menuVote);
		AddMenuItem(m, "Vote ", "No", -1, menuVote);
		break;
	}

	AddMenuItem(m, "Cancel", nullptr, -1, menuCancel);
	FinishMenu(ent, m, true);
}

void motd_menu(edict_t *ent)
{
	if (motd_lines.empty())
	{
		menuMotdContinue(ent, nullptr, nullptr, 0);
		return;
	}

	qmenu_t *m = CreateQMenu(ent, "Message of the Day");
	AddMenuItem(m, "---------Continue----------", nullptr, -1, menuMotdContinue);
	for (const std::string &line : motd_lines)
		AddMenuItem(m, line.c_str(), nullptr, -1, nullptr);
	FinishMenu(ent, m, true);
}

void show_teamconfirm_menu(edict_t *ent, int32_t arenanum)
{
	qmenu_t *m = CreateQMenu(ent, "Confirmation");
	AddMenuItem(m, "You have too few players", nullptr, -1, nullptr);
	AddMenuItem(m, "Do you wish to continue?", nullptr, -1, nullptr);
	AddMenuItem(m, "", nullptr, -1, nullptr);
	AddMenuItem(m, "Yes, continue to arena ", nullptr, arenanum, menuTeamConfirm);
	AddMenuItem(m, "No, choose another", nullptr, -1, menuNo);
	FinishMenu(ent, m, true);
}

void menu_centerprint(edict_t *ent, const char *message)
{
	// with no menu on screen this is just a centerprint, which is what the
	// original tested showmenu for: a client sitting on a loaded-but-hidden
	// observer menu should get the message printed over its view, not have a
	// menu opened on top of it (see MenuShown / Cmd_Inven_f).
	if (!MenuShown(ent))
	{
		gi.LocCenter_Print(ent, message ? message : "");
		return;
	}

	if (ent->client->curmenulink)
	{
		menuinfo_t *info = MENU_INFO(ent->client->curmenulink);
		if (info && info->title == "Message")
		{
			ent->client->menuusetime = {};
			PopMenu(ent);
		}
	}

	qmenu_t *m = CreateQMenu(ent, "Message");
	AddMenuItem(m, "---------Continue----------", nullptr, -1, menuNo);

	add_wrapped_message_rows(m, message ? message : "");

	FinishMenu(ent, m, true);
}
