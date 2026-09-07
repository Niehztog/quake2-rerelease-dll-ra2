// Copyright (c) ZeniMax Media Inc.
// Licensed under the GNU General Public License 2.0.

// rocketarena2/arena.cpp -- Rocket Arena 2 arena/team/round system.

#include "../g_local.h"

#include "arena.h"

#include <algorithm>
#include <array>
#include <fstream>

bool SpawnPointClear(edict_t *spot); // p_client.cpp -- see SelectRandomArenaSpawnPoint

cvar_t *ra2 = nullptr;
cvar_t *admincode = nullptr;

std::array<qmenu_t, MAX_TEAMS> teams;
std::array<arena_t, MAX_ARENAS> arenas;
int32_t num_arenas = 0;
bool	idmap = false;

// votetries_setting's definition lives in maploop.cpp (matches RA2's own
// maploop.c ownership); arena.h provides the extern declaration used here.
bool	allow_grapple = false;
bool	broken = false;

std::array<std::string, MAX_ARENA_SKINS> teamskins =
{
	"r2red", "r2blue", "r2dgre", "r2oran", "r2yell", "r2aqua", "r2lgre"
};

std::array<std::string, 4> vwepmodels =
{
	"male", "female", "cyborg", "crakhor"
};

std::array<int32_t, MAX_ARENA_SKINS> teamskins_precachem = {};
std::array<int32_t, MAX_ARENA_SKINS> teamskins_precachef = {};
std::array<int32_t, MAX_ARENA_SKINS> teamskins_precachecw = {};
std::array<int32_t, MAX_ARENA_SKINS> teamskins_precachecb = {};
int32_t genericicon = 0;

std::array<const char *, 4> omode_descriptions =
{
	"Normal", "Free Flying", "Trackcam", "In Eyes"
};

extern void load_config(int32_t numarenas);
extern void set_config(int32_t first, int32_t last);
extern void show_observer_menu(edict_t *ent);
extern void show_arena_menu(edict_t *ent);
extern void show_teamconfirm_menu(edict_t *ent, int32_t arenanum);

namespace
{
	[[nodiscard]] inline arena_settings_t &SETTINGS(arena_t &arena)
	{
		return arena.settings;
	}

	[[nodiscard]] inline const arena_settings_t &SETTINGS(const arena_t &arena)
	{
		return arena.settings;
	}

	[[nodiscard]] inline team_t *TeamFromArenaNode(qmenu_t *node)
	{
		return TEAM((qmenu_t *) node->it);
	}

	void SetPlayerStringStat(edict_t *ent, player_stat_t stat, int32_t config_index, const std::string &text)
	{
		send_configstring(ent, config_index, text.c_str());
		ent->client->ps.stats[stat] = (int16_t) config_index;
	}

	[[nodiscard]] int32_t LookupSkinIcon(edict_t *ent)
	{
		char skin[MAX_INFO_VALUE] = { 0 };
		gi.Info_ValueForKey(ent->client->pers.userinfo, "skin", skin, sizeof(skin));

		auto skinicon = G_Fmt("{}_i", skin);
		int32_t image = gi.imageindex(skinicon.data());
		int32_t icon = genericicon;

		for (int32_t i = 0; i < MAX_ARENA_SKINS; i++)
		{
			if (teamskins_precachem[i] == image || teamskins_precachef[i] == image ||
				teamskins_precachecw[i] == image || teamskins_precachecb[i] == image)
			{
				icon = image;
				break;
			}
		}

		return icon;
	}

	[[nodiscard]] std::string BuildStatusString(int32_t arenanum)
	{
		auto &arena = arenas[arenanum];
		std::string base;

		switch (arena.state)
		{
		case ASTATE_WARMUP:
			base = arena.vs.empty() ? "Waiting for match to start" : arena.vs;
			break;
		case ASTATE_COUNTDOWN:
			base = arena.vs.empty() ? "Match starting" : arena.vs;
			break;
		case ASTATE_FIGHTING:
			base = arena.vs.empty() ? "Fight!" : arena.vs;
			break;
		case ASTATE_RESULTS:
		case ASTATE_NEXTROUND:
			base = arena.msg.empty() ? arena.vs : arena.msg;
			break;
		case ASTATE_ROUNDEND:
			base = "Waiting for teams";
			break;
		}

		return base;
	}

}

int32_t GetSkinIcon(edict_t *ent)
{
	return LookupSkinIcon(ent);
}

int32_t count_players_queue(qmenu_t *head)
{
	int32_t count = 0;

	for (qmenu_t *p = head->next; p; p = p->next)
	{
		auto *e = (edict_t *) p->it;
		if (e->client->resp.fightstate == FIGHT_ALIVE)
			count++;
	}

	return count;
}

void set_damage(int32_t arenanum, int32_t state)
{
	for (qmenu_t *tnode = arenas[arenanum].activeteams.next; tnode; tnode = tnode->next)
	{
		qmenu_t *slot = (qmenu_t *) tnode->it;
		for (qmenu_t *mnode = slot->next; mnode; mnode = mnode->next)
		{
			auto *e = (edict_t *) mnode->it;
			if (e->client->resp.fightstate != FIGHT_SPECTATING)
				e->takedamage = state;
		}
	}
}

void give_ammo(edict_t *ent)
{
		static constexpr std::array<int32_t, 9> weapon_vals_x = { 256, 1, 2, 4, 8, 16, 128, 64, 32 };
		auto &arena = arenas[ent->client->resp.context];
		std::array<gitem_t *, 9> weapons = {};
		gitem_t *it = nullptr;
		gitem_t *rl = nullptr;
		bool needswitch = false;

		ent->health = SETTINGS(arena).health ? SETTINGS(arena).health : 100;

		weapons[0] = FindItemByClassname("weapon_bfg");
		weapons[1] = FindItemByClassname("weapon_shotgun");
		weapons[2] = FindItemByClassname("weapon_supershotgun");
		weapons[3] = FindItemByClassname("weapon_machinegun");
		weapons[4] = FindItemByClassname("weapon_chaingun");
		weapons[5] = FindItemByClassname("weapon_grenadelauncher");
		weapons[6] = FindItemByClassname("weapon_railgun");
		weapons[7] = FindItemByClassname("weapon_hyperblaster");
		weapons[8] = FindItemByClassname("weapon_rocketlauncher");

		for (int32_t i = 8; i >= 0; i--)
		{
			if (!weapons[i])
				continue;

			if (SETTINGS(arena).weapons & weapon_vals_x[i])
			{
				if (!rl)
					rl = weapons[i];

				if (!ent->client->pers.inventory[rl->id] || needswitch)
				{
					ent->client->newweapon = rl;
					ent->client->pers.selected_item = rl->id;
					ent->client->ps.stats[STAT_SELECTED_ITEM] = (int16_t) rl->id;
					needswitch = false;
				}

				ent->client->pers.inventory[weapons[i]->id] = 1;
			}
			else
			{
				if (ent->client->pers.weapon == weapons[i])
					needswitch = true;

				ent->client->pers.inventory[weapons[i]->id] = 0;
			}
		}

		if (needswitch)
		{
			rl = FindItemByClassname("weapon_blaster");
			if (rl)
			{
				ent->client->newweapon = rl;
				ent->client->pers.selected_item = rl->id;
				ent->client->ps.stats[STAT_SELECTED_ITEM] = (int16_t) rl->id;
			}
		}

		if ((it = FindItemByClassname("ammo_shells")))
			ent->client->pers.inventory[it->id] = SETTINGS(arena).shells;
		if ((it = FindItemByClassname("ammo_bullets")))
			ent->client->pers.inventory[it->id] = SETTINGS(arena).bullets;
		if ((it = FindItemByClassname("ammo_slugs")))
			ent->client->pers.inventory[it->id] = SETTINGS(arena).slugs;
		if ((it = FindItemByClassname("ammo_grenades")))
			ent->client->pers.inventory[it->id] = SETTINGS(arena).grenades;
		if ((it = FindItemByClassname("ammo_rockets")))
			ent->client->pers.inventory[it->id] = SETTINGS(arena).rockets;
		if ((it = FindItemByClassname("ammo_cells")))
			ent->client->pers.inventory[it->id] = SETTINGS(arena).cells;
		if ((it = FindItemByClassname("item_armor_body")))
			ent->client->pers.inventory[it->id] = SETTINGS(arena).armor;

		if (allow_grapple)
			ent->client->pers.inventory[IT_WEAPON_GRAPPLE] = 1;
}

team_t *add_to_team(edict_t *ent, const char *teamname)
{
	for (int32_t i = 0; i < MAX_TEAMS; i++)
	{
		team_t *t = (team_t *) teams[i].it;
		if (!t || t->name != teamname)
			continue;

		if (t->arenanum)
		{
			if (count_queue(&teams[i]) == SETTINGS(arenas[t->arenanum]).playersperteam)
				return nullptr;
			if (SETTINGS(arenas[t->arenanum]).locked)
				return nullptr;
			// GameSpy player-registration call stripped here.
		}

		add_to_queue(&ent->client->resp.teammember, &teams[i]);
		ent->client->resp.teamnum = i;

		if (t->skin != -1)
			setteamskin(ent, ent->client->pers.userinfo, t->skin);

		gi.LocBroadcast_Print(PRINT_MEDIUM, "{} has been added to team {} ({})\n",
			ent->client->pers.netname, i, teamname);
		return t;
	}

	int32_t i = 0;
	for (; i < MAX_TEAMS; i++)
		if (!teams[i].it)
			break;

	if (i == MAX_TEAMS)
		return nullptr;

	auto *t = new team_t();
	t->name = teamname;
	t->teamnum = i;
	t->wins = -1;
	t->skin = -1;
	t->arenalink.it = &teams[i];
	teams[i].it = t;

	if (ent)
	{
		add_to_queue(&t->arenalink, &arenas[0].waitingteams);
		add_to_queue(&ent->client->resp.teammember, &teams[i]);
		ent->client->resp.teamnum = i;

		gi.LocBroadcast_Print(PRINT_MEDIUM, "{} has created team number {} ({})\n",
			ent->client->pers.netname, i, teamname);
		return t;
	}

	t->locked = true;
	return t;
}

team_t *create_own_team(edict_t *ent)
{
	std::string name = std::string(G_Fmt("{}'s Team", RA2_PlayerName(ent)));
	for (int32_t i = 0; i < MAX_TEAMS; )
	{
		if (teams[i].it && TEAM(&teams[i])->name == name)
		{
			name += '!';
			i = 0;
			continue;
		}
		++i;
	}

	return add_to_team(ent, name.c_str());
}

void remove_from_team(edict_t *ent)
{
	if (ent->client->resp.teamnum < 0)
		return;

	team_t *team = TEAM(&teams[ent->client->resp.teamnum]);
	if (!team)
		return;

	gi.LocBroadcast_Print(PRINT_MEDIUM, "{} has been removed from team {} ({})\n",
		ent->client->pers.netname, ent->client->resp.teamnum, team->name.c_str());

	// GameSpy remove-player call stripped here.
	remove_from_queue(&ent->client->resp.teammember, nullptr);
	check_teams(ent->client->resp.context);
	ent->client->resp.teamnum = -1;
}

// RA2 authors its own arenas with a spawn point per fighter per side, and the
// side argument splits the arena's list by parity: side 2 takes the even
// entries, side 1 the odd ones. Only pickup arenas come through here -- a
// normal arena spawns through SelectFarthestArenaSpawnPoint below, which is
// occupancy-aware -- and RA2 then picked at random within the side and lived
// with whatever it drew.
//
// That falls apart on a map RA2 was never designed around. q2dm1 is a pickup
// arena by arena.cfg but has ten generic info_player_deathmatch entities and no
// notion of sides, so a four-a-side round draws four times out of the five the
// parity split leaves it and puts two players on one spot in four rounds out of
// five. Preferring a spot nobody is standing on costs nothing where RA2's own
// arenas are concerned -- there the side's list is sized to the team, so every
// draw is free anyway -- and brings this path in line with how RA2 spreads
// players out everywhere else. Drop the free_spots pass for the original's raw
// draw.
edict_t *SelectRandomArenaSpawnPoint(const char *classn, int32_t arenanum, int32_t side)
{
	std::vector<edict_t *> spots;
	edict_t *spot = nullptr;

	while ((spot = G_FindByString<&edict_t::classname>(spot, classn)) != nullptr)
	{
		if (!idmap && spot->arena != arenanum)
			continue;
		spots.push_back(spot);
	}

	if (spots.empty())
		return nullptr;

	// this side's half of the list
	std::vector<edict_t *> side_spots;
	if (side)
		for (size_t i = (side == 1) ? 1 : 0; i < spots.size(); i += 2)
			side_spots.push_back(spots[i]);

	if (side_spots.empty())
		side_spots = spots;

	std::vector<edict_t *> free_spots;
	for (edict_t *s : side_spots)
		if (SpawnPointClear(s))
			free_spots.push_back(s);

	const std::vector<edict_t *> &pool = free_spots.empty() ? side_spots : free_spots;
	return pool[irandom((int32_t) pool.size())];
}

namespace
{
	// Is this client someone an arriving player should be kept away from? Only
	// the ones actually fighting in this arena count. An observer is alive, has
	// takedamage cleared and noclips, and move_to_arena drops them onto a spawn
	// point and leaves them hovering there, so counting observers means picking
	// spawns by where the audience stood. The player being placed is passed in
	// as `ignore` so they do not push themselves away from their own old spot.
	//
	// The rerelease's own PlayersRangeFromSpot() does neither -- it measures
	// against every live client on the server and has no way to exclude one --
	// so it cannot stand in for this.
	[[nodiscard]] bool is_live_body(edict_t *e, edict_t *ignore)
	{
		if (e == ignore)
			return false;
		if (!e->inuse)
			return false;
		if (!e->client)
			return false;
		if (e->health <= 0)
			return false;

		return true;
	}

	[[nodiscard]] bool is_arena_fighter(edict_t *e, int32_t arenanum, edict_t *ignore)
	{
		if (!is_live_body(e, ignore))
			return false;
		if (e->client->resp.fightstate != FIGHT_ALIVE)
			return false;
		if (!idmap && e->client->resp.context != arenanum)
			return false;

		return true;
	}

	// Distance from spot to the nearest client that counts: with fighters_only,
	// the ones fighting in this arena, otherwise every live body on the server.
	// Returns false when it counted nobody at all, which is the caller's cue
	// that this measure has nothing to say about this spot.
	bool range_from_spot(edict_t *spot, int32_t arenanum, edict_t *ignore,
						 bool fighters_only, float &range)
	{
		bool found = false;

		for (uint32_t n = 0; n < game.maxclients; n++)
		{
			edict_t *player = &g_edicts[n + 1];

			if (fighters_only)
			{
				if (!is_arena_fighter(player, arenanum, ignore))
					continue;
			}
			else if (!is_live_body(player, ignore))
				continue;

			const float playerdistance = (spot->s.origin - player->s.origin).length();

			if (!found || playerdistance < range)
			{
				range = playerdistance;
				found = true;
			}
		}

		return found;
	}

	// Returns the distance to the nearest player fighting in this arena, and
	// where nobody is fighting here, to the nearest live body anywhere.
	//
	// That fallback is not optional. Measuring against nothing and returning 0
	// scores every spot below SelectFarthestArenaSpawnPoint's floor, so the
	// caller falls through to SelectRandomArenaSpawnPoint -- and random spots
	// collide. Two arrivals on one spot is not cosmetic: an observer in NORMAL
	// mode is SOLID_BBOX on MOVETYPE_WALK, and neither KillBox nor
	// check_telefrag will touch a FIGHT_SPECTATING client, so once two of them
	// are inside each other nothing in the mod ever separates them again. The
	// staging area is exactly that case -- everyone there is FIGHT_SPECTATING,
	// so the fighter pass counts nobody, every time.
	[[nodiscard]] float fighters_range_from_spot(edict_t *spot, int32_t arenanum, edict_t *ignore)
	{
		float range = 0.f;

		if (range_from_spot(spot, arenanum, ignore, true, range))
			return range;

		range_from_spot(spot, arenanum, ignore, false, range);

		return range;
	}
}

edict_t *SelectFarthestArenaSpawnPoint(const char *classn, int32_t arenanum, edict_t *ignore)
{
	edict_t *spot = nullptr;
	edict_t *bestspot = nullptr;
	float bestdistance = 50.f;

	while ((spot = G_FindByString<&edict_t::classname>(spot, classn)) != nullptr)
	{
		if (!idmap && spot->arena != arenanum)
			continue;

		float bestplayerdistance = fighters_range_from_spot(spot, arenanum, ignore);
		if (bestplayerdistance > bestdistance)
		{
			bestspot = spot;
			bestdistance = bestplayerdistance;
		}
	}

	// if there is a player just spawned on each and every start spot
	// we have no choice to turn one into a telefrag meltdown
	return bestspot ? bestspot : SelectRandomArenaSpawnPoint(classn, arenanum, 0);
}

void track_SetStats(edict_t *ent)
{
	auto *target = ent->client->resp.track_target;
	if (!target)
		return;

	int32_t score = ent->client->resp.score;
	ent->client->ps.stats = target->client->ps.stats;
	ent->client->ps.stats[STAT_FRAGS] = (int16_t) score;

	// the copy above brought the target's layout bits along with it, but the
	// layout channel is unicast, so this client's own board/menu state owns it.
	// ClientEndServerFrame skips the whole stats block while tracking, which
	// makes this the only writer -- and why a menu opened while tracking never
	// used to draw at all.
	ent->client->ps.stats[STAT_LAYOUTS] &= ~(LAYOUTS_LAYOUT | LAYOUTS_RA2_NO_PUTAWAY);
	ent->client->ps.stats[STAT_LAYOUTS] |= RA2_LayoutFlag(ent);

	RA2_SetIDView(ent);
}

void eyecam_think(edict_t *ent, usercmd_t *ucmd)
{
	(void) ucmd;
	auto *target = ent->client->resp.track_target;
	if (!target || target->client->resp.fightstate != FIGHT_ALIVE)
	{
		track_next(ent);
		return;
	}

	vec3_t forward, dest;
	gi.unlinkentity(ent);
	ent->s.origin = target->s.origin;
	AngleVectors(target->client->v_angle, forward, nullptr, nullptr);
	dest = forward * 20.f;
	ent->velocity = {};
	ent->s.origin += dest;
	ent->s.origin[2] += 22;
	ent->s.angles = target->client->v_angle;
	ent->client->ps.viewangles = target->client->v_angle;
	ent->client->v_angle = target->client->v_angle;
	ent->client->ps.pmove.delta_angles = ent->s.angles - ent->client->resp.cmd_angles;
	gi.linkentity(ent);
	track_SetStats(ent);
}

void track_think(edict_t *ent, usercmd_t *ucmd)
{
	(void) ucmd;
	auto *target = ent->client->resp.track_target;
	if (!target || target->client->resp.fightstate != FIGHT_ALIVE)
	{
		track_next(ent);
		return;
	}

	vec3_t dest, forward;
	AngleVectors(ent->client->ps.viewangles, forward, nullptr, nullptr);
	dest = target->s.origin - (forward * 150.f);

	trace_t tr = gi.trace(target->s.origin, vec3_origin, vec3_origin, dest, target, MASK_SOLID);
	if (tr.fraction < 1.0f)
		dest = target->s.origin + (forward * (tr.fraction * -130.f));

	bool stuck = gi.trace(ent->s.origin, PLAYER_MINS, PLAYER_MAXS, ent->s.origin, ent, MASK_PLAYERSOLID).contents & MASK_SOLID;
	if (!stuck)
		tr = gi.trace(ent->s.origin, PLAYER_MINS, PLAYER_MAXS, dest, ent, MASK_SOLID);

	if (stuck || tr.fraction < 1.0f)
	{
		gi.unlinkentity(ent);
		ent->s.origin = dest;
		gi.linkentity(ent);
		ent->velocity = {};
	}
	else
	{
		ent->velocity = (dest - ent->s.origin) * 10.f;
	}

	track_SetStats(ent);
}

void track_change(edict_t *ent, int32_t dir)
{
	gclient_t *cl = ent->client;
	edict_t *target = cl->resp.track_target;
	bool wrapped = false;

	if (!target)
	{
		target = &g_edicts[1];
		wrapped = true;
	}
	else if (target->client->resp.fightstate != FIGHT_ALIVE ||
		target->client->resp.context != cl->resp.context)
	{
		wrapped = true;
	}

	int32_t i = (int32_t) (target - g_edicts);
	edict_t *e = target;

	do
	{
		i += dir;
		if (i > (int32_t) game.maxclients)
			i = 1;
		else if (i < 1)
			i = game.maxclients;

		e = &g_edicts[i];
		if (e->inuse && e->client && e->client->resp.fightstate == FIGHT_ALIVE &&
			e->client->resp.context == cl->resp.context &&
			(!SETTINGS(arenas[cl->resp.context]).competition || cl->resp.teamnum == e->client->resp.teamnum) &&
			e->solid)
		{
			cl->resp.track_target = e;
			gi.LocClient_Print(ent, PRINT_HIGH, "Tracking {}\n", e->client->pers.netname);
			return;
		}
	} while (e != target);

	if (wrapped)
	{
		cl->resp.omode = cl->resp.lastomode;
		move_to_arena(ent, ent->client->resp.context, 2);
		gi.LocClient_Print(ent, PRINT_HIGH, "No one to track\n");
		return;
	}

	cl->resp.track_target = e;
	gi.LocClient_Print(ent, PRINT_HIGH, "Tracking {}\n", e->client->pers.netname);
}

void track_next(edict_t *ent)
{
	track_change(ent, 1);
}

void track_prev(edict_t *ent)
{
	track_change(ent, -1);
}

void SetObserverMode(edict_t *ent)
{
	switch ((observer_mode_t) ent->client->resp.omode)
	{
	case OBSERVER_NORMAL:
		ent->movetype = MOVETYPE_WALK;
		ent->solid = SOLID_BBOX;
		ent->clipmask = MASK_PLAYERSOLID;
		ent->svflags &= ~SVF_NOCLIENT;
		ent->client->resp.track_target = nullptr;
		ent->s.modelindex = 255;
		ent->client->ps.pmove.pm_flags &= ~(PMF_NO_POSITIONAL_PREDICTION | PMF_NO_ANGULAR_PREDICTION);
		break;

	case OBSERVER_FREEFLYING:
		ent->movetype = MOVETYPE_NOCLIP;
		ent->solid = SOLID_NOT;
		ent->clipmask = CONTENTS_NONE;
		ent->svflags |= SVF_NOCLIENT;
		ent->client->resp.track_target = nullptr;
		ent->client->ps.pmove.pm_time = 0;
		ent->client->ps.pmove.pm_flags &= ~(PMF_NO_POSITIONAL_PREDICTION | PMF_NO_ANGULAR_PREDICTION);
		ent->client->ps.pmove.pm_flags &= ~PMF_TIME_TELEPORT;
		break;

	case OBSERVER_TRACKCAM:
	case OBSERVER_EYECAM:
		ent->movetype = MOVETYPE_NOCLIP;
		ent->solid = SOLID_NOT;
		ent->clipmask = CONTENTS_NONE;
		ent->svflags |= SVF_NOCLIENT;
		if (ent->client->resp.omode == OBSERVER_TRACKCAM)
			ent->s.modelindex = 0;
		ent->client->ps.pmove.pm_flags |= PMF_NO_POSITIONAL_PREDICTION | PMF_NO_ANGULAR_PREDICTION;
		ent->client->ps.pmove.delta_angles = -ent->client->resp.cmd_angles;
		ent->s.angles = {};
		ent->client->ps.viewangles = {};
		ent->client->v_angle = {};
		if (!ent->client->resp.track_target || ent->client->resp.track_target->client->resp.fightstate != FIGHT_ALIVE)
			track_next(ent);
		break;
	}
}

void move_to_arena(edict_t *ent, int32_t arenanum, int32_t mode)
{
	edict_t *dest = nullptr;

	if (mode)
	{
		if (!arenas[arenanum].active)
			dest = SelectFarthestArenaSpawnPoint("misc_teleporter_dest", arenanum, ent);
		else
			dest = SelectFarthestArenaSpawnPoint("info_player_deathmatch", arenanum, ent);

		// menus are for players. A bot can't act on one, and one left up on it
		// freezes its pmove outright (PM_FREEZE, see ClientThink), so a bot
		// arriving in an arena would stand still for the whole round.
		const bool wants_menus = !RA2_IsBot(ent);

		if (arenanum)
		{
			if (ent->client->resp.context == 0)
			{
				ent->client->resp.context = arenanum;
				if (wants_menus)
					show_observer_menu(ent);
			}
		}
		else
		{
			ent->client->resp.track_target = nullptr;
			if (ent->client->resp.teamnum != -1 && wants_menus)
				show_arena_menu(ent);
		}

		ent->client->resp.context = arenanum;
	}
	else
	{
		ent->client->resp.context = arenanum;
		ClientUserinfoChanged(ent, ent->client->pers.userinfo);

		if (arenas[arenanum].idarena)
		{
			int32_t side = (TEAM(&teams[ent->client->resp.teamnum])->side != arenas[arenanum].sidepick) + 1;
			dest = SelectRandomArenaSpawnPoint("info_player_deathmatch", arenanum, side);
		}
		else
		{
			dest = SelectFarthestArenaSpawnPoint("info_player_deathmatch", arenanum, ent);
		}
	}

	if (!dest)
	{
		gi.Com_PrintFmt("no dest found\n");
		return;
	}

	gi.unlinkentity(ent);

	// Clip to the floor under the arena spot. RA2 (via Q2PRO) does this here
	// rather than in PutClientInServer, because in Rocket Arena the arena spot
	// -- not the spawn point -- is where the player actually lands, and an
	// arena's misc_teleporter_dest entities are authored well clear of the
	// ground. Falling back to origin+10 is the original's own else branch.
	{
		vec3_t below = dest->s.origin;
		vec3_t above = dest->s.origin;
		below[2] -= 64;
		above[2] += 16;

		trace_t floor = gi.trace(above, PLAYER_MINS, PLAYER_MAXS, below, ent, MASK_PLAYERSOLID);
		if (!floor.allsolid && !floor.startsolid)
		{
			ent->s.origin = floor.endpos;
			ent->groundentity = floor.ent;
			ent->groundentity_linkcount = floor.ent ? floor.ent->linkcount : 0;
		}
		else
		{
			ent->s.origin = dest->s.origin;
			ent->s.origin[2] += 10;
		}
	}

	ent->s.old_origin = ent->s.origin;
	ent->velocity = {};
	ent->client->ps.pmove.pm_time = 160;
	ent->client->ps.pmove.pm_flags |= PMF_TIME_TELEPORT;
	if (mode == 0)
		ent->s.event = EV_PLAYER_TELEPORT;
	ent->client->ps.pmove.delta_angles = dest->s.angles - ent->client->resp.cmd_angles;
	ent->s.angles = {};
	ent->client->ps.viewangles = {};
	ent->client->v_angle = {};
	AngleVectors(ent->client->v_angle, ent->client->v_forward, nullptr, nullptr);

	if (mode)
	{
		if (arenas[arenanum].active && ent->client->resp.omode == OBSERVER_NORMAL)
			ent->client->resp.omode = OBSERVER_FREEFLYING;
		if (SETTINGS(arenas[arenanum]).competition && mode != 2)
			ent->client->resp.omode = OBSERVER_EYECAM;
	}
	else
	{
		ent->client->resp.omode = OBSERVER_NORMAL;
		ent->client->resp.spawn_recheck = level.time + 500_ms;
	}

	SetObserverMode(ent);
	gi.linkentity(ent);

	// The overlap pass, which RA2 ran between the unlink and the link above.
	// It cannot live there any more, and had been doing nothing at all:
	//
	//  - the rerelease's KillBox searches ent->absmin/absmax, and only
	//    gi.linkentity() recomputes those, so before the link it swept the box
	//    the player had just been teleported out of. RA2's own KillBox traced
	//    from s.origin and so did not care where in the sequence it sat.
	//  - it also returns immediately on MOVETYPE_NOCLIP, which an arriving
	//    fighter still carries from the observer state it is leaving until the
	//    SetObserverMode() above puts it back on MOVETYPE_WALK.
	//
	// Between them, two fighters who drew the same spawn point were never
	// pushed apart, and RA2 has no other mechanism for that: a pickup side
	// picks uniformly out of half the map's info_player_deathmatch entities
	// with no regard for what is already standing there (five of q2dm1's ten),
	// so a four-a-side round shares a spot about four times in five.
	KillBox(ent, !!ent->client);

	// a shove drops both players to SOLID_NOT for check_telefrag() to undo,
	// and that only reaches the world on the next link
	gi.linkentity(ent);

	if (mode == 0)
		ent->client->resp.spawn_recheck = level.time + 500_ms;
	else
		ent->client->resp.spawn_recheck = 0_ms;

	if (arenas[arenanum].proposetime > level.time && !ent->client->resp.ra2_voted && arenas[arenanum].proposer &&
		!RA2_IsBot(ent))
	{
		menu_centerprint(ent, G_Fmt("Settings changes have been proposed\n by {}!\nGoto the observer menu (TAB) to vote",
			RA2_PlayerName(arenas[arenanum].proposer)).data());
		gi.local_sound(ent, ent, CHAN_AUTO | CHAN_RELIABLE, gi.soundindex("misc/pc_up.wav"),
			1.0f, ATTN_NONE, 0.0f, GetUnicastKey());
	}
}

void ChangeOMode(edict_t *ent)
{
	if (ent->client->resp.fightstate != FIGHT_SPECTATING)
		return;

	if (ent->client->resp.omode != OBSERVER_TRACKCAM && ent->client->resp.omode != OBSERVER_EYECAM)
		ent->client->resp.lastomode = ent->client->resp.omode;

	ent->client->resp.omode = (ent->client->resp.omode + 1) % 4;
	gi.LocClient_Print(ent, PRINT_HIGH, "Switched Observer Mode to: {}\n",
		omode_descriptions[ent->client->resp.omode]);
	move_to_arena(ent, ent->client->resp.context, 1);
}

int32_t getfreeskin(int32_t arenanum)
{
	std::array<bool, MAX_ARENA_SKINS> used = {};

	for (int32_t i = 0; i < MAX_TEAMS; i++)
	{
		auto *t = (team_t *) teams[i].it;
		if (!t || t->arenanum != arenanum || t->skin == -1)
			continue;
		used[t->skin] = true;
	}

	for (int32_t i = 0; i < MAX_ARENA_SKINS; i++)
		if (!used[i])
			return i;

	return irandom(MAX_ARENA_SKINS);
}

std::string RA2_PlayerName(const edict_t *ent)
{
	if (!ent || !ent->client)
		return "";

	// netname is still the real thing for a bot, and for anyone reached from
	// inside ClientUserinfoChanged before the swap at its tail -- which is one
	// of setteamskin()'s callers, and the only place holding a name newer than
	// the saved userinfo. Testing for the token is what keeps both correct.
	if (strncmp(ent->client->pers.netname, "##P", 3) != 0)
		return ent->client->pers.netname;

	char name[MAX_INFO_VALUE] = { 0 };
	if (gi.Info_ValueForKey(ent->client->pers.userinfo, "name", name, sizeof(name)) && name[0])
		return name;

	return ent->client->pers.netname;
}

void setteamskin(edict_t *ent, char *userinfo, int32_t skinnum)
{
	char val[MAX_INFO_VALUE] = { 0 };
	gi.Info_ValueForKey(userinfo, "skin", val, sizeof(val));

	const char *model = "male";
	if (val[0] == 'f')
		model = "female";
	else if (val[0] == 'c' && val[1] == 'r')
		model = "crakhor";
	else if (val[0] == 'c' && val[1] == 'y')
		model = "cyborg";

	std::string skin = G_Fmt("{}/{}", model, teamskins[skinnum]).data();
	int32_t pnum = (int32_t) (ent - g_edicts - 1);

	if (strcmp(val, skin.c_str()) != 0)
		gi.configstring(CS_PLAYERSKINS + pnum, G_Fmt("{}\\{}", RA2_PlayerName(ent), skin).data());

	gi.Info_SetValueForKey(userinfo, "skin", skin.c_str());

	// RA2 followed this with a stuffed "skin <model>/nullxxx", to push the
	// client's own skin cvar to a sentinel and make it send its userinfo back;
	// ClientUserinfoChanged's /nullxxx branch then re-applied the team colour.
	// The rerelease client does not act on stuffed console text, so that
	// round-trip never happened -- and it is not needed here: the configstring
	// set above is what every other client renders, and ClientUserinfoChanged
	// re-forces the team skin on each userinfo update regardless. Dropping it
	// also leaves a player's real skin intact instead of spending the rest of
	// the session as "<model>/nullxxx".
}

void SendTeamToArena(qmenu_t *team, int32_t arenanum, bool observer, bool announce)
{
	if (!TEAM(team)->outofline)
	{
		if (arenanum == 0)
			TEAM(team)->skin = -1;
		else if (TEAM(team)->skin == -1)
		{
			if (SETTINGS(arenas[arenanum]).playersperteam > 1 || arenas[arenanum].idarena)
				TEAM(team)->skin = getfreeskin(arenanum);
		}
		else if (SETTINGS(arenas[arenanum]).playersperteam == 1 && !arenas[arenanum].idarena)
		{
			TEAM(team)->skin = -1;
		}
	}

	for (qmenu_t *mnode = team->next; mnode; mnode = mnode->next)
	{
		auto *ent = (edict_t *) mnode->it;
		if (TEAM(team)->skin != -1)
			setteamskin(ent, ent->client->pers.userinfo, TEAM(team)->skin);

		if (observer)
		{
			ent->client->resp.fightstate = FIGHT_SPECTATING;
			ent->takedamage = false;
			move_to_arena(ent, arenanum, 1);
		}
		else
		{
			ent->client->resp.fightstate = FIGHT_ALIVE;
			ent->takedamage = false;
			move_to_arena(ent, arenanum, 0);
			give_ammo(ent);
		}
	}

	if (announce)
	{
		if (observer)
		{
			TEAM(team)->fighting = false;
			add_to_queue(&TEAM(team)->arenalink, &arenas[arenanum].waitingteams);
		}
		else
		{
			add_to_queue(&TEAM(team)->arenalink, &arenas[arenanum].activeteams);
		}
	}

	TEAM(team)->arenanum = arenanum;
	arenas[arenanum].teamplay = SETTINGS(arenas[arenanum]).playersperteam > 1 || arenas[arenanum].idarena;
	gi.Com_PrintFmt("{}: {} {} entered\n", arenanum, TEAM(team)->teamnum, TEAM(team)->name.c_str());
}

int32_t AddtoArena(edict_t *ent, int32_t arenanum, int32_t allow_partial, bool skip_checks)
{
	if (!ent || !ent->client || !RA2_IsPlayableArena(arenanum))
		return 1;

	const int32_t teamnum = ent->client->resp.teamnum;
	if (teamnum < 0 || teamnum >= MAX_TEAMS || !teams[teamnum].it)
		return 1;

	auto &arena = arenas[arenanum];
	auto &settings = SETTINGS(arena);

	if (!skip_checks)
	{
		if (settings.minping && ent->client->ping < settings.minping)
		{
			menu_centerprint(ent, G_Fmt("Your ping is too low\nMinimum ping for this arena: {}", settings.minping).data());
			return 1;
		}
		if (settings.maxping && ent->client->ping > settings.maxping)
		{
			menu_centerprint(ent, G_Fmt("Your ping is too high\nMaximum ping for this arena: {}", settings.maxping).data());
			return 1;
		}
		if (settings.locked)
		{
			menu_centerprint(ent, "Sorry, that Arena is locked by an admin\n");
			return 1;
		}
		if (arena.idarena)
		{
			menu_centerprint(ent, "You must join a pickup team to\n enter that arena");
			return 1;
		}
		if (count_queue(&arena.activeteams) + count_queue(&arena.waitingteams) >= arena.maxteams)
		{
			menu_centerprint(ent, "Sorry, that arena is full");
			return 1;
		}
	}

	int32_t membercount = count_queue(&teams[ent->client->resp.teamnum]);
	if (membercount != settings.playersperteam && (membercount > settings.playersperteam || !allow_partial))
	{
		if (membercount < settings.playersperteam)
		{
			show_teamconfirm_menu(ent, arenanum);
			return 1;
		}

		menu_centerprint(ent, G_Fmt("You have the incorrect number\nof team members, you need {} to play \nin that arena",
			settings.playersperteam).data());
		return 1;
	}

	TEAM(&teams[ent->client->resp.teamnum])->outofline = skip_checks;
	if (!skip_checks)
	{
		remove_from_queue(&TEAM(&teams[ent->client->resp.teamnum])->arenalink, nullptr);
		SendTeamToArena(&teams[ent->client->resp.teamnum], arenanum, true, true);
		return 0;
	}

	SendTeamToArena(&teams[ent->client->resp.teamnum], arenanum, true, false);
	return 0;
}

void check_teams(int32_t arenanum)
{
	for (int32_t i = 0; i < MAX_TEAMS; i++)
	{
		if (!teams[i].it)
			continue;
		if (count_queue(&teams[i]) != 0)
			continue;
		if (TEAM(&teams[i])->locked)
			continue;

		remove_from_queue(&TEAM(&teams[i])->arenalink, nullptr);
		gi.Com_PrintFmt("Clearing team {} ({})\n", TEAM(&teams[i])->teamnum, TEAM(&teams[i])->name.c_str());
		delete TEAM(&teams[i]);
		teams[i] = {};
	}

	if (!arenanum)
		return;

	qmenu_t *head = &arenas[arenanum].waitingteams;

	// Collect first, evict second. SendTeamToArena() below re-appends the team's
	// arenalink to arenas[0].waitingteams -- when arenanum is 0 that is the very
	// list being walked, so the old cursor walk kept reaching the re-appended
	// node, rejected it again and looped forever, broadcasting as it went.
	// Removal also nulls a node's own links, so even for other arenas the saved
	// neighbour could go stale while SendTeamToArena ran. Bounded by MAX_TEAMS,
	// which is the most this queue can hold, and kept on the stack because
	// arena_think() calls this every frame.
	std::array<qmenu_t *, MAX_TEAMS> rejects;
	size_t num_rejects = 0;

	for (qmenu_t *tnode = head->next; tnode && num_rejects < rejects.size(); tnode = tnode->next)
	{
		bool rejected = false;
		qmenu_t *slot = (qmenu_t *) tnode->it;

		if (!arenas[arenanum].idarena)
		{
			for (qmenu_t *mnode = slot->next; mnode; mnode = mnode->next)
			{
				auto *e = (edict_t *) mnode->it;
				if ((e->client->ping > SETTINGS(arenas[arenanum]).maxping && e->client->ping < 1000) ||
					e->client->ping < SETTINGS(arenas[arenanum]).minping)
				{
					rejected = true;
					gi.LocClient_Print(e, PRINT_HIGH, "Sorry, your ping of {} does not work in this arena\n", e->client->ping);
				}
			}
		}

		if (count_queue(slot) > SETTINGS(arenas[arenanum]).playersperteam || rejected)
			rejects[num_rejects++] = tnode;
	}

	for (size_t i = 0; i < num_rejects; ++i)
	{
		qmenu_t *slot = (qmenu_t *) rejects[i]->it;
		gi.LocBroadcast_Print(PRINT_MEDIUM, "Removing team {} ({})\n",
			TEAM(slot)->teamnum, TEAM(slot)->name.c_str());
		remove_from_queue(rejects[i], nullptr);
		SendTeamToArena(slot, 0, true, true);
	}

	if (SETTINGS(arenas[arenanum]).changed && count_queue(&arenas[arenanum].activeteams) + count_queue(head) == 0)
	{
		set_config(arenanum, arenanum);
		gi.Com_PrintFmt("{}: Reseting to default config\n", arenanum);
	}
}

void init_player(edict_t *ent)
{
	ent->client->resp.teammember = {};
	ent->client->resp.teammember.it = ent;
	ent->client->resp.fightstate = FIGHT_SPECTATING;
	ent->client->resp.context = 0;
	ent->client->resp.teamnum = -1;
	ent->client->resp.track_target = nullptr;
	ent->client->resp.lastomode = OBSERVER_FREEFLYING;
	ent->takedamage = false;

	// a bot can't read the motd or work a menu, and a menu left up on it would
	// also freeze its pmove (PM_FREEZE, see ClientThink). RA2_BotsJoinArenas()
	// picks its team and arena instead -- the same split CTF makes when
	// CTFStartClient() skips the join menu for SVF_BOT clients.
	if (RA2_IsBot(ent))
	{
		ent->client->pers.showmotd = false;
	}
	else if (ent->client->pers.showmotd)
	{
		motd_menu(ent);
	}
	else
	{
		menuRefreshTeamList(ent, nullptr, nullptr, 0);
	}

	send_configstring(ent, CONFIG_RA2_QUEUE1_NAME, " Red");
	send_configstring(ent, CONFIG_RA2_QUEUE2_NAME, "Blue");
	ent->client->ps.stats[STAT_RA2_QUEUE1_NAME] = CONFIG_RA2_QUEUE1_NAME;
	ent->client->ps.stats[STAT_RA2_QUEUE2_NAME] = CONFIG_RA2_QUEUE2_NAME;
}

void reinit_player(edict_t *ent)
{
	ent->client->resp.fightstate = FIGHT_SPECTATING;
	ent->client->resp.track_target = nullptr;
	ent->client->resp.lastomode = OBSERVER_FREEFLYING;
}

bool RA2_IsBot(const edict_t *ent)
{
	return ent->client != nullptr && (ent->svflags & SVF_BOT) != 0;
}

int32_t RA2_TeamIndexForBots(const edict_t *ent)
{
	if (!ent->client || ent->client->resp.teamnum < 0)
		return Team_None;

	// only a live round has sides; everyone else -- queued, dead, watching --
	// is teamless, the way a CTF spectator's team_index is 0.
	if (ent->client->resp.fightstate != FIGHT_ALIVE)
		return Team_None;

	const int32_t arenanum = ent->client->resp.context;
	if (arenanum < 0 || arenanum >= MAX_ARENAS)
		return Team_None;

	// bots/teams.txt maps sv.team 1 onto "Red Team" and 2 onto "Blue Team",
	// and says outright that two is all the bot systems support. An arena.cfg
	// that raises `maxteams` past its default of 2 would otherwise hand the AI
	// a side it has no entry for, so anything past the second active team
	// reads as teamless rather than as a third team.
	int32_t side = Team_None;
	for (qmenu_t *tnode = arenas[arenanum].activeteams.next; tnode; tnode = tnode->next)
	{
		if (++side > 2)
			break;
		if (TeamFromArenaNode(tnode)->teamnum == ent->client->resp.teamnum)
			return side;
	}

	return Team_None;
}

namespace
{
	// the arena the humans went to, which is where the bots belong. 0 when
	// nobody has picked one yet.
	[[nodiscard]] int32_t HumanArena()
	{
		for (auto player : active_players())
		{
			if (RA2_IsBot(player) || player->client->resp.teamnum < 0)
				continue;

			team_t *team = TEAM(&teams[player->client->resp.teamnum]);
			if (team && team->arenanum > 0)
				return team->arenanum;
		}

		return 0;
	}

	// the team in `arenanum` this bot should fill: the emptiest one it still
	// fits on, and of two equally empty ones the one with a human on it. That
	// is the "follow the player onto their team" half, and it only applies
	// where a team holds more than one player -- playersperteam > 1, or a
	// pickup arena, where arena_init sets that to 128 and the two standing
	// Pickup Red/Blue teams are the only ones there are. Balance has to come
	// first, or every bot would pile onto the human's side and leave the other
	// team empty, which never starts a round (check_for_teams).
	[[nodiscard]] team_t *OpenTeamInArena(int32_t arenanum)
	{
		const int32_t playersperteam = SETTINGS(arenas[arenanum]).playersperteam;

		team_t *best = nullptr;
		bool	best_has_human = false;
		int32_t best_members = 0;

		for (int32_t i = 0; i < MAX_TEAMS; i++)
		{
			auto *team = (team_t *) teams[i].it;
			if (!team || team->arenanum != arenanum)
				continue;

			const int32_t members = count_queue(&teams[i]);
			if (members >= playersperteam)
				continue;

			// an empty team that isn't one of a pickup arena's two standing
			// ones is a team everybody just left: check_teams() is about to
			// delete it, so don't move onto it.
			if (members == 0 && !arenas[arenanum].idarena)
				continue;

			bool has_human = false;
			for (qmenu_t *mnode = teams[i].next; mnode; mnode = mnode->next)
			{
				if (!RA2_IsBot((edict_t *) mnode->it))
				{
					has_human = true;
					break;
				}
			}

			if (!best || members < best_members ||
				(members == best_members && has_human && !best_has_human))
			{
				best = team;
				best_has_human = has_human;
				best_members = members;
			}
		}

		return best;
	}

	// AddtoArena() reports every refusal by opening a menu on the client, which
	// a bot can neither read nor dismiss, so ask its conditions up front. This
	// mirrors the !skip_checks half of AddtoArena(), plus the one membercount
	// case a fresh single-member team can still trip.
	[[nodiscard]] bool CanSignUpOwnTeam(const edict_t *bot, int32_t arenanum)
	{
		arena_t &arena = arenas[arenanum];
		const arena_settings_t &settings = SETTINGS(arena);

		if (settings.locked || arena.idarena || settings.playersperteam < 1)
			return false;
		if (settings.minping && bot->client->ping < settings.minping)
			return false;
		if (settings.maxping && bot->client->ping > settings.maxping)
			return false;

		return count_queue(&arena.activeteams) + count_queue(&arena.waitingteams) < arena.maxteams;
	}

	// seats one bot in `arenanum`, by the same two routes a player has out of
	// the menus: onto a team that is short of players (menuAddtoTeam), or onto
	// a new team of its own signed up for the arena (menuNewTeam +
	// menuAddtoArena).
	void SeatBot(edict_t *bot, int32_t arenanum)
	{
		if (team_t *team = OpenTeamInArena(arenanum))
		{
			if (!add_to_team(bot, team->name.c_str()))
				return;

			// that team is already signed up for this arena, so all that's left
			// is to seat the bot in it as an observer -- menuAddtoTeam's tail.
			bot->client->resp.fightstate = FIGHT_SPECTATING;
			bot->takedamage = false;
			move_to_arena(bot, arenanum, 1);
			return;
		}

		if (!CanSignUpOwnTeam(bot, arenanum) || !create_own_team(bot))
			return;

		AddtoArena(bot, arenanum, 1, false);
	}

	// a bot on a team of its own, sitting in the wrong arena, that is free to
	// walk out of it right now. Anything shared with a human stays put: that
	// human picked the arena.
	[[nodiscard]] bool BotShouldLeaveArena(edict_t *bot, int32_t target)
	{
		team_t *team = TEAM(&teams[bot->client->resp.teamnum]);
		if (!team || target == 0 || team->arenanum == target)
			return false;

		if (count_queue(&teams[team->teamnum]) != 1)
			return false;

		// the rule the "Leave Arena" menu item enforces (menuLeaveArena): a
		// fighter can't walk out mid-round.
		const arena_state_t state = arenas[team->arenanum].state;
		return state == ASTATE_COUNTDOWN || state == ASTATE_ROUNDEND || !bot->takedamage;
	}
}

void RA2_BotsJoinArenas()
{
	static gtime_t next_pass;

	if (num_arenas < 1)
		return;

	// level.time restarts at zero on a map change, so a deadline left over from
	// the previous level would park this until the clock caught back up.
	if (next_pass > level.time + 1_sec)
		next_pass = 0_ms;
	if (level.time < next_pass)
		return;
	next_pass = level.time + 1_sec;

	// with no human anywhere yet, the first arena is as good as any: the bots
	// play each other there until somebody turns up, and get migrated below if
	// that somebody picks a different one.
	const int32_t last_arena = std::min(num_arenas, MAX_ARENAS - 1);
	const int32_t target = std::min(HumanArena(), last_arena);
	const int32_t seat_in = target ? target : 1;

	for (auto bot : active_players())
	{
		if (!RA2_IsBot(bot))
			continue;

		if (bot->client->resp.teamnum >= 0)
		{
			if (!BotShouldLeaveArena(bot, target))
				continue;

			remove_from_team(bot);
			bot->client->resp.fightstate = FIGHT_SPECTATING;
			bot->takedamage = false;
			move_to_arena(bot, 0, 1);
			continue; // seated again by the next pass, now that it has no team
		}

		SeatBot(bot, seat_in);
	}
}

void show_stringc(const char *s, int32_t context)
{
	for (uint32_t i = 0; i < game.maxclients; i++)
	{
		auto *e = g_edicts + 1 + i;
		if (e->inuse && e->client && e->client->resp.context == context)
			gi.LocCenter_Print(e, s);
	}
}

void show_string(int32_t priority, const char *s, int32_t context)
{
	for (uint32_t i = 0; i < game.maxclients; i++)
	{
		auto *e = g_edicts + 1 + i;
		if (e->inuse && e->client && e->client->resp.context == context)
			gi.LocClient_Print(e, (print_type_t) priority, s);
	}
}

void stuffcmd(edict_t *ent, const char *s)
{
	gi.WriteByte(svc_stufftext);
	gi.WriteString(s);
	gi.unicast(ent, true);
}

// RA2's announcer. The original mod stuffed a "play <file>" console command at
// every client in the arena, which the rerelease client never acts on, so none
// of the voices were ever heard. Sent as a real sound now -- unicast per client
// rather than gi.sound(), because every arena shares one map and a broadcast
// would let the whole server hear another arena's announcements. ATTN_NONE
// keeps it at full volume wherever the listener happens to be standing.
void send_sound_to_arena(const char *soundname, int32_t context)
{
	int32_t index = gi.soundindex(soundname);
	uint32_t key = GetUnicastKey();

	for (uint32_t i = 0; i < game.maxclients; i++)
	{
		auto *e = g_edicts + 1 + i;
		if (e->inuse && e->client && e->client->resp.context == context)
			gi.local_sound(e, e, CHAN_AUTO | CHAN_RELIABLE, index, 1.0f, ATTN_NONE, 0.0f, key);
	}
}

void send_configstring(edict_t *e, int32_t index, const char *string)
{
	gi.WriteByte(svc_configstring);
	gi.WriteShort(index);
	gi.WriteString(string);
	gi.unicast(e, true);
}

void show_countdown(int32_t countdown, int32_t arenanum)
{
	// one key for the whole tick, so a splitscreen connection is not sent the
	// same "3.. 2.. 1.. fight" twice (see GetUnicastKey in g_weapon.cpp)
	const uint32_t key = GetUnicastKey();

	for (uint32_t i = 0; i < game.maxclients; i++)
	{
		auto *e = &g_edicts[i + 1];
		if (!e->inuse || !e->client || e->client->resp.context != arenanum)
			continue;

		if (arenas[arenanum].state == ASTATE_WARMUP)
			send_configstring(e, CONFIG_RA2_ARENASTATUS, "Waiting for match to start");
		else
			send_configstring(e, CONFIG_RA2_ARENASTATUS, arenas[arenanum].vs.c_str());

		if (SETTINGS(arenas[arenanum]).rounds > 1)
			send_configstring(e, CONFIG_RA2_ROUNDINFO, G_Fmt("Round {} of {}", arenas[arenanum].round, SETTINGS(arenas[arenanum]).rounds).data());
		else
			send_configstring(e, CONFIG_RA2_ROUNDINFO, "");

		e->client->ps.stats[STAT_RA2_ARENASTATUS] = CONFIG_RA2_ARENASTATUS;
		e->client->ps.stats[STAT_RA2_ROUNDINFO] = CONFIG_RA2_ROUNDINFO;
		e->client->ps.stats[STAT_RA2_COUNTDOWN] = (int16_t) countdown;

		if (countdown > 0 && countdown < 4 && arenas[arenanum].state != ASTATE_WARMUP)
			gi.local_sound(e, e, CHAN_AUTO | CHAN_RELIABLE,
				gi.soundindex(G_Fmt("ra/{}.wav", countdown).data()), 1.0f, ATTN_NONE, 0.0f, key);
		else if (countdown == 0 && arenas[arenanum].state != ASTATE_WARMUP)
		{
			gi.local_sound(e, e, CHAN_AUTO | CHAN_RELIABLE,
				gi.soundindex("ra/fight.wav"), 1.0f, ATTN_NONE, 0.0f, key);
			gi.LocCenter_Print(e, "FIGHT!");
		}
	}

	// Deliberate superset: the original only refreshes the audience bar from
	// arena_think's FIGHTING and RESULTS arms, so the line-position readout
	// went dark during warmup and countdown -- which is exactly when a player
	// waiting in the queue wants it. It also stands in for the original's own
	// repaint here, which reinstated dm_statusbar at countdown 15/10/5; this
	// port's SendStatusBar already resends whenever the content changes.
	UpdateStatusBars(arenanum);
}

int32_t show_rank(qmenu_t *node)
{
	int32_t count = 0;
	for (qmenu_t *p = node->prev; p; p = p->prev)
		count++;
	return count;
}

bool check_for_teams(int32_t arenanum)
{
	qmenu_t *head = &arenas[arenanum].waitingteams;
	int32_t count = count_queue(head);
	if (count < arenas[arenanum].numteams)
		return false;

	int32_t i = 0;
	for (qmenu_t *tnode = head->next; tnode; tnode = tnode->next)
	{
		if (i >= arenas[arenanum].numteams)
			break;
		i++;
		if (count_queue((qmenu_t *) tnode->it) == 0)
			return false;
	}

	return true;
}

int32_t fill_arena(int32_t arenanum)
{
	int32_t firstskin = -1;
	std::string vs;

	arenas[arenanum].sidepick = irandom(2);

	for (int32_t count = 0; count < arenas[arenanum].numteams; count++)
	{
		qmenu_t *popped = remove_from_queue(nullptr, &arenas[arenanum].waitingteams);
		if (!popped)
		{
			gi.Com_PrintFmt("Team left during multi-round match\n");
			return 1;
		}

		team_t *team = TEAM((qmenu_t *) popped->it);
		if (firstskin == -1)
			firstskin = team->skin;
		else if (firstskin == team->skin)
		{
			gi.Com_PrintFmt("Skin conflict in arena {}\n", arenanum);
			team->skin = (firstskin + 1) % MAX_ARENA_SKINS;
		}

		SendTeamToArena((qmenu_t *) popped->it, arenanum, false, true);
		if (!vs.empty())
			vs += " vs ";
		vs += team->name;

		if (arenas[arenanum].round == 1)
			team->wins = 0;
		team->fighting = true;
	}

	arenas[arenanum].vs = vs;
	gi.Com_PrintFmt("{}: {}\n", arenanum, arenas[arenanum].vs.c_str());
	return 1;
}

int32_t fight_done(int32_t arenanum)
{
	int32_t winner = -1;

	for (qmenu_t *tnode = arenas[arenanum].activeteams.next; tnode; tnode = tnode->next)
	{
		qmenu_t *slot = (qmenu_t *) tnode->it;
		for (qmenu_t *mnode = slot->next; mnode; mnode = mnode->next)
		{
			auto *e = (edict_t *) mnode->it;
			if (!e->takedamage || e->deadflag)
				continue;

			if (winner == -1)
				winner = e->client->resp.teamnum;
			else if (winner != e->client->resp.teamnum)
				return -2;
		}
	}

	return winner;
}

static void loc_buildboxpoints(vec3_t p[8], vec3_t org, vec3_t mins, vec3_t maxs)
{
	p[0] = org + mins;
	p[1] = p[0]; p[1][0] -= mins[0];
	p[2] = p[0]; p[2][1] -= mins[1];
	p[3] = p[0]; p[3][0] -= mins[0]; p[3][1] -= mins[1];
	p[4] = org + maxs;
	p[5] = p[4]; p[5][0] -= maxs[0];
	p[6] = p[0]; p[6][1] -= maxs[1];
	p[7] = p[0]; p[7][0] -= maxs[0]; p[7][1] -= maxs[1];
}

static bool loc_CanSee(edict_t *targ, edict_t *inflictor)
{
	if (targ->movetype == MOVETYPE_PUSH)
		return false;

	vec3_t targpoints[8];
	loc_buildboxpoints(targpoints, targ->s.origin, targ->mins, targ->maxs);
	vec3_t viewpoint = inflictor->s.origin;
	viewpoint[2] += inflictor->viewheight;

	for (int32_t i = 0; i < 8; i++)
	{
		trace_t trace = gi.trace(viewpoint, vec3_origin, vec3_origin, targpoints[i], inflictor, MASK_SOLID);
		if (trace.fraction == 1.0f)
			return true;
	}

	return false;
}

void RA2_SetIDView(edict_t *ent)
{
	ent->client->ps.stats[STAT_RA2_ID_VIEW] = 0;

	// an observer always gets the name of whoever they're watching -- that's
	// RA2's own behaviour, and it's the only label the eyecam has. A fighter
	// gets one only after asking for it with "id".
	if (ent->client->resp.fightstate != FIGHT_SPECTATING && !ent->client->resp.id_state)
		return;

	edict_t *target = ent->client->resp.track_target;
	if (target)
	{
		ent->client->ps.stats[STAT_RA2_ID_VIEW] = (int16_t) (target - g_edicts);
		return;
	}

	vec3_t forward, viewpoint = ent->s.origin;
	viewpoint[2] += ent->viewheight;
	AngleVectors(ent->client->v_angle, forward, nullptr, nullptr);
	forward = viewpoint + (forward * 1024.f);
	// RA2 traced for CONTENTS_MONSTER because 3.20 players were monsters as far
	// as the collision world was concerned; the rerelease gives them a
	// CONTENTS_PLAYER of their own, so that mask alone never hits anybody.
	trace_t tr = gi.trace(viewpoint, vec3_origin, vec3_origin, forward, ent, CONTENTS_SOLID | CONTENTS_MONSTER | CONTENTS_PLAYER);
	if (tr.fraction < 1.0f && tr.ent && tr.ent->client && tr.ent->solid)
		ent->client->ps.stats[STAT_RA2_ID_VIEW] = (int16_t) (tr.ent - g_edicts);
}

/*
================
UpdateStatusBars

The audience's own bar: where this client's team stands in the queue, and the
two live rosters with each fighter's health. RA2 swapped the whole CS_STATUSBAR
configstring out from under an observer to draw this and swapped dm_statusbar
back in for everybody else; here it is a layout, because that is the channel
the rerelease leaves a mod (see SendMenu), which also means it has to yield to
a menu and to the scoreboard -- the same three-way the original got for free by
having two channels. The conditions are the original's: an observer, not
tracking anybody, with no board and no menu up.

The other thing the original got for free is that escape ignored this bar. A
layout does not ignore it, so the bar takes RA2_LayoutFlag's private bit rather
than LAYOUTS_LAYOUT; see there.
================
*/
void UpdateStatusBars(int32_t arenanum)
{
	auto &arena = arenas[arenanum];

	// gather the rosters first: one string serves every observer in the arena
	std::array<team_t *, MAX_STATUS_TEAMS> roster_team = {};
	std::array<int32_t, MAX_STATUS_TEAMS> roster_count = {};
	std::array<std::array<edict_t *, MAX_STATUS_MEMBERS>, MAX_STATUS_TEAMS> roster_member = {};
	int32_t numteams = 0;

	for (qmenu_t *tnode = arena.activeteams.next; tnode && numteams < MAX_STATUS_TEAMS; tnode = tnode->next)
	{
		qmenu_t *slot = (qmenu_t *) tnode->it;
		roster_team[numteams] = TEAM(slot);

		int32_t members = 0;
		for (qmenu_t *mnode = slot->next; mnode && members < MAX_STATUS_MEMBERS; mnode = mnode->next)
		{
			auto *e = (edict_t *) mnode->it;
			if (!e->takedamage || e->deadflag)
				continue;
			roster_member[numteams][members++] = e;
		}

		roster_count[numteams] = members;
		numteams++;
	}

	std::string bar = G_Fmt("xl 8 yb -10 string2 \"Line Position:\" xl 100 yb -24 num 2 {} ",
		(int32_t) STAT_RA2_LINEPOSITION).data();

	// competition mode keeps the rosters off the audience's screen
	if (!SETTINGS(arena).competition)
	{
		int32_t y = 40;
		for (int32_t ti = 0; ti < numteams; ti++)
		{
			bar += G_Fmt("xl 8 yt {} string2 \"{}\" ", y, layout_escape(roster_team[ti]->name));
			y += 8;

			for (int32_t i = 0; i < roster_count[ti]; i++)
			{
				edict_t *e = roster_member[ti][i];
				bar += G_Fmt("xl 8 yt {} string2 \"{}: {}\" ", y,
					layout_escape(RA2_PlayerName(e)), e->health);
				y += 8;
			}

			y += 8;
		}
	}

	std::string status = BuildStatusString(arenanum);
	std::string roundinfo = SETTINGS(arena).rounds > 1 ?
		G_Fmt("Round {} of {}", arena.round, SETTINGS(arena).rounds).data() : "";

	for (uint32_t i = 0; i < game.maxclients; i++)
	{
		auto *e = &g_edicts[i + 1];
		if (!e->inuse || !e->client || e->client->resp.context != arenanum)
			continue;

		SetPlayerStringStat(e, STAT_RA2_ARENASTATUS, CONFIG_RA2_ARENASTATUS, status);
		SetPlayerStringStat(e, STAT_RA2_ROUNDINFO, CONFIG_RA2_ROUNDINFO, roundinfo);
		e->client->ps.stats[STAT_RA2_COUNTDOWN] = (int16_t) arena.countdown;

		// the pickup queue counters, the skin icon and the observer ID view are
		// per-client and per-frame rather than per-arena-tick, so G_SetStats
		// owns all three -- which is where the original keeps them too.
		const bool wants_bar = e->client->resp.fightstate == FIGHT_SPECTATING &&
			!e->client->resp.track_target && !e->client->scoremode && !MenuShown(e);

		if (wants_bar)
		{
			int32_t rank = 0;
			if (e->client->resp.teamnum >= 0 && e->client->resp.teamnum < MAX_TEAMS &&
				teams[e->client->resp.teamnum].it)
			{
				rank = show_rank(&TEAM(&teams[e->client->resp.teamnum])->arenalink);
			}

			e->client->ps.stats[STAT_RA2_LINEPOSITION] = (int16_t) rank;
			e->client->showscores = true;
			SendStatusBar(e, bar.c_str(), false);
		}
		else if (!MenuShown(e) && !e->client->scoremode && !e->client->menutext.empty())
		{
			// the bar was up and this client no longer qualifies for it; nobody
			// else owns the channel, so take it down
			e->client->ps.stats[STAT_RA2_LINEPOSITION] = 0;
			e->client->showscores = false;
			SendStatusBar(e, "", true);
		}
	}
}

void check_telefrag(int32_t arenanum)
{
	for (uint32_t i = 0; i < game.maxclients; i++)
	{
		auto *e = g_edicts + i + 1;
		if (!e->inuse || !e->client || e->client->resp.context != arenanum ||
			e->client->resp.fightstate == FIGHT_SPECTATING || !e->client->resp.spawn_recheck ||
			e->client->resp.spawn_recheck > level.time)
		{
			continue;
		}

		trace_t tr = gi.trace(e->s.origin, e->mins, e->maxs, e->s.origin, nullptr, MASK_PLAYERSOLID);
		if (tr.contents == CONTENTS_SOLID)
		{
			e->solid = SOLID_NOT;
			vec3_t angles = { 0, (float) irandom(360), 0 };
			vec3_t forward;
			AngleVectors(angles, forward, nullptr, nullptr);
			e->velocity += forward * 600.f;
			e->client->resp.spawn_recheck = level.time + 500_ms;
		}
		else
		{
			// KillBox owns the deadline from here: its RA2 preamble clears it,
			// and re-arms it only if it had to shove this client apart from
			// someone. Clearing it again afterwards -- which RA2 does not do --
			// stranded whoever was shoved on this pass at SOLID_NOT with no
			// recheck ever coming, and a client that is never solid again can
			// still run and shoot but can no longer be hit, tracked by an
			// observer, or killed to end the round.
			e->solid = SOLID_BBOX;
			gi.unlinkentity(e);
			KillBox(e, !!e->client);
			gi.linkentity(e);
		}
	}
}

void start_voting(edict_t *proposer, int32_t arenanum)
{
	auto &arena = arenas[arenanum];
	arena.proposetime = level.time + ((arena.state == ASTATE_FIGHTING || arena.state == ASTATE_COUNTDOWN) ? 30000_sec : 30_sec);
	arena.votes_yes = 0;
	arena.votes_no = 0;
	arena.votetries = 0;
	arena.proposer = proposer;

	const uint32_t vote_key = GetUnicastKey();

	for (uint32_t i = 0; i < game.maxclients; i++)
	{
		auto *cl_ent = g_edicts + i + 1;
		if (!cl_ent->inuse || !cl_ent->client || cl_ent->client->resp.context != arenanum)
			continue;

		// a bot never votes, so counting it as an eligible voter would only
		// raise the bar check_voting() measures the yes votes against, and a
		// bot-heavy arena could never pass a settings change at all.
		if (RA2_IsBot(cl_ent))
			continue;

		cl_ent->client->resp.ra2_voted = false;
		arena.votetries++;

		if (cl_ent->client->resp.fightstate != FIGHT_SPECTATING)
			continue;

		if (cl_ent != proposer && proposer)
			menu_centerprint(cl_ent, G_Fmt("Settings changes have been proposed\nby {}!\nGoto the observer menu (TAB) to vote",
				RA2_PlayerName(proposer)).data());
		gi.local_sound(cl_ent, cl_ent, CHAN_AUTO | CHAN_RELIABLE, gi.soundindex("misc/pc_up.wav"),
			1.0f, ATTN_NONE, 0.0f, vote_key);
	}

	gi.Com_PrintFmt("Starting Voting in Arena {} with {} voters\n", arenanum, arena.votetries);
}

void check_voting(int32_t arenanum)
{
	auto &arena = arenas[arenanum];
	if (!arena.proposetime || arena.proposetime > level.time)
		return;

	arena.proposetime = 0_ms;
	bool passed = (double) (arena.votes_yes - arena.votes_no) >= (double) arena.votetries * (1.0 / 3.0);
	if (passed)
	{
		arena.settings = arena.proposed;
		arena.settings.changed = true;
	}

	std::string msg = G_Fmt("Changes {}! Yes votes: {} No votes: {}\n",
		passed ? "Passed" : "Failed", arena.votes_yes, arena.votes_no).data();

	for (uint32_t i = 0; i < game.maxclients; i++)
	{
		auto *cl_ent = g_edicts + i + 1;
		if (!cl_ent->inuse || !cl_ent->client || cl_ent->client->resp.context != arenanum)
			continue;

		gi.LocClient_Print(cl_ent, PRINT_CHAT, msg.c_str());

		// `changed` and not `passed`: the flag is sticky until set_config()
		// resets an emptied arena, so once one poll has carried in this arena
		// every later poll -- including a failed one -- hands everybody their
		// proposal budget back. That is the original's own reading of it.
		if (arena.settings.changed)
			cl_ent->client->resp.votes = votetries_setting;
	}

	gi.Com_Print(msg.c_str());
	check_teams(arenanum);
}

void arena_think(int32_t arenanum)
{
	check_teams(arenanum);
	check_voting(arenanum);
	check_telefrag(arenanum);

	auto &arena = arenas[arenanum];
	if (arena.state == ASTATE_COUNTDOWN || arena.state == ASTATE_WARMUP)
	{
		if (!arena.countdown_next_tick)
		{
			arena.countdown_next_tick = level.time + 1_sec;
			if (arena.state == ASTATE_WARMUP)
				arena.countdown = 15;
			else if (arena.proposetime > level.time)
				arena.countdown = 10;
			else
				arena.countdown = 5;
			show_countdown(arena.countdown, arenanum);
			return;
		}

		if (arena.countdown_next_tick >= level.time)
			return;

		arena.countdown--;
		show_countdown(arena.countdown, arenanum);
		if (arena.countdown != 0)
		{
			arena.countdown_next_tick = level.time + 1_sec;
			return;
		}

		arena.countdown_next_tick = 0_ms;
		if (arena.state == ASTATE_WARMUP)
		{
			if (!check_for_teams(arenanum))
			{
				arena.state = ASTATE_ROUNDEND;
				show_stringc("Not enough teams to start", arenanum);
				return;
			}

			arena.state = ASTATE_COUNTDOWN;
			fill_arena(arenanum);
			return;
		}

		arena.state = ASTATE_FIGHTING;
		set_damage(arenanum, true);
		return;
	}
	else if (arena.state == ASTATE_FIGHTING && !broken)
	{
		UpdateStatusBars(arenanum);
		if (fight_done(arenanum) <= -2)
			return;
		arena.state = ASTATE_RESULTS;
		return;
	}
	else if (arena.state == ASTATE_ROUNDEND)
	{
		if (!check_for_teams(arenanum))
			return;

		arena.round = 1;
		if (!arena.idarena)
		{
			arena.state = ASTATE_COUNTDOWN;
			fill_arena(arenanum);
			return;
		}

		arena.state = ASTATE_WARMUP;
		return;
	}
	else if (arena.state == ASTATE_RESULTS)
	{
		UpdateStatusBars(arenanum);
		if (!arena.countdown_next_tick)
		{
			arena.countdown_next_tick = level.time + 3_sec;
			return;
		}
		if (arena.countdown_next_tick >= level.time)
			return;

		arena.state = ASTATE_NEXTROUND;
		arena.countdown_next_tick = 0_ms;
		if (arena.proposetime && (arena.proposetime - level.time) > 30_sec)
			arena.proposetime = level.time + 30_sec;
		return;
	}
	else if (arena.state == ASTATE_NEXTROUND)
	{
		int32_t winner = fight_done(arenanum);
		if (winner == -1)
		{
			arena.msg = "It was a tie!";
			if (count_queue(&arena.activeteams) != 0)
			{
				qmenu_t *tnode = arena.activeteams.next;
				if (count_queue((qmenu_t *) tnode->it) != 0)
					arena.round--;
			}
		}
		else
		{
			// GameSpy team/server snapshot updates stripped here.
			auto *t = TEAM(&teams[winner]);
			t->wins++;
			if (t->wins > SETTINGS(arena).rounds / 2)
			{
				arena.msg = G_Fmt("{} has won the match!!", t->name).data();
				arena.round = SETTINGS(arena).rounds;
			}
			else
			{
				arena.msg = G_Fmt("{} has won the round!", t->name).data();
			}
		}

		gi.Com_PrintFmt("{}: {} {}\n", arenanum, winner, arena.msg.c_str());
		set_damage(arenanum, false);
		show_stringc(arena.msg.c_str(), arenanum);
		arena.sidepick = irandom(2);

		// The round-vs-match test is loop-invariant, so it selects the whole walk
		// rather than varying per team. Keeping them as one loop meant the
		// match-over branch drained activeteams by the head while the cursor rode
		// tnode->next -- and once a node had been spliced into waitingteams, that
		// cursor was walking waitingteams instead. Iteration count then came from
		// the wrong list: too few passes stranded active teams in the arena
		// forever, too many popped an empty queue and handed nullptr to
		// TeamFromArenaNode.
		if (arena.round < SETTINGS(arena).rounds)
		{
			// nothing here relinks activeteams (announce is false), so a plain walk is safe
			for (qmenu_t *tnode = arena.activeteams.next; tnode; tnode = tnode->next)
			{
				auto *t = TeamFromArenaNode(tnode);
				int32_t morewins = SETTINGS(arena).rounds / 2 - t->wins + 1;
				std::string msg = G_Fmt("{} has {} wins and needs {} more to take the match\n", t->name, t->wins, morewins).data();
				show_string(PRINT_HIGH, msg.c_str(), arenanum);
				SendTeamToArena((qmenu_t *) tnode->it, arenanum, false, false);
			}
		}
		else
		{
			// match over: drain every active team back into the waiting queue,
			// winners to the front. Draining by the head is the only safe way to
			// walk a list the body relinks, and it stops on an empty queue.
			while (qmenu_t *popped = remove_from_queue(nullptr, &arena.activeteams))
			{
				team_t *t = TeamFromArenaNode(popped);
				t->fighting = false;
				if (t->teamnum == winner || winner == -1)
					add_to_front_queue(popped, &arena.waitingteams);
				else
					add_to_queue(popped, &arena.waitingteams);
			}
		}

		if (arena.round >= SETTINGS(arena).rounds)
		{
			arena.state = ASTATE_ROUNDEND;
			return;
		}

		arena.state = ASTATE_COUNTDOWN;
		arena.round++;
		return;
	}
}

void multi_arena_think()
{
	if (level.intermissiontime)
		return;
	if (num_arenas <= 0)
		return;

	RA2_BotsJoinArenas();

	int32_t i = (int32_t) (gi.ServerFrame() % (num_arenas * 2));
	if (i % 2)
		return;

	arena_think(i / 2 + 1);
}

void arena_shutdown()
{
	for (qmenu_t &slot : teams)
		delete TEAM(&slot);

	teams = {};
	arenas = {};
	num_arenas = 0;
	idmap = false;
}

void arena_init(edict_t *wsent)
{
	if (!wsent)
		return;

	// unreachable in practice -- PreInitGame registers ra2 long before any
	// worldspawn -- but kept in step with it, default included.
	if (!ra2)
		ra2 = gi.cvar("ra2", "1", CVAR_SERVERINFO | CVAR_LATCH);
	admincode = gi.cvar("admincode", "0", CVAR_NOFLAGS);

	arena_shutdown();

	const int32_t requested_arenas = wsent->arena;
	if (requested_arenas == 0)
	{
		num_arenas = 1;
		idmap = true;
	}
	else
	{
		num_arenas = std::clamp(requested_arenas, 1, MAX_ARENAS - 1);
		idmap = false;
		if (num_arenas != requested_arenas)
			gi.Com_PrintFmt("Worldspawn requested {} arenas; using {}\n", requested_arenas, num_arenas);
	}

	load_config(num_arenas + 1);
	set_config(1, num_arenas);

	for (int32_t i = 0; i <= num_arenas && i < MAX_ARENAS; i++)
	{
		arenas[i].state = ASTATE_ROUNDEND;
		arenas[i].active = idmap;
		arenas[i].numteams = 2;
		arenas[i].countdown_next_tick = 0_ms;
		arenas[i].countdown = 0;
		arenas[i].proposetime = 0_ms;
		arenas[i].round = 0;
		arenas[i].teamplay = SETTINGS(arenas[i]).playersperteam > 1 || arenas[i].idarena;

		if (!SelectFarthestArenaSpawnPoint("misc_teleporter_dest", i, nullptr))
		{
			gi.Com_PrintFmt("Setting arena {} to idarena mode\n", i);
			arenas[i].active = true;
		}

		if (i && arenas[i].idarena)
		{
			team_t *t = add_to_team(nullptr, G_Fmt("#{} Pickup Red", i).data());
			if (!t)
				gi.Com_Error("Unable to create pickup Red team");
			t->side = 0;
			SendTeamToArena(&teams[t->teamnum], i, true, true);
			arenas[i].pickupteam[0] = t;

			t = add_to_team(nullptr, G_Fmt("#{} Pickup Blue", i).data());
			if (!t)
				gi.Com_Error("Unable to create pickup Blue team");
			t->side = 1;
			SendTeamToArena(&teams[t->teamnum], i, true, true);
			arenas[i].pickupteam[1] = t;

			arenas[i].maxteams = 2;
			SETTINGS(arenas[i]).playersperteam = 128;
			arenas[i].teamplay = true;
		}
	}

	load_motd();
}

TOUCH(ra2_teleporter_touch) (edict_t *self, edict_t *other, const trace_t &tr, bool other_touching_self) -> void
{
	(void) tr;
	(void) other_touching_self;

	if (!other->client)
		return;

	if (self->arena > 0)
	{
		if (!RA2_IsPlayableArena(self->arena))
		{
			menu_centerprint(other, "This arena is unavailable");
			return;
		}

		if (other->client->resp.teamnum != -1)
		{
			AddtoArena(other, self->arena, 1, false);
			return;
		}

		menu_centerprint(other, "You must join or create a team first");
		return;
	}

	edict_t *dest = G_FindByString<&edict_t::targetname>(nullptr, self->target);
	if (!dest)
	{
		gi.Com_Print("Couldn't find destination\n");
		return;
	}

	CTFPlayerResetGrapple(other);
	gi.unlinkentity(other);
	other->s.origin = dest->s.origin;
	other->s.old_origin = dest->s.origin;
	other->s.origin[2] += 10;
	other->velocity = {};
	other->client->ps.pmove.pm_time = 160;
	other->client->ps.pmove.pm_flags |= PMF_TIME_TELEPORT;
	// RA2 -- only play the teleport flash when the toucher is actually an
	// active combatant; spectators/dead players teleport silently
	if (other->client->resp.fightstate == FIGHT_ALIVE)
	{
		// a bare trigger_teleport (no separate visible pad entity) self-owns,
		// so the teleport flash plays at the trigger itself rather than crashing
		(self->owner ? self->owner : self)->s.event = EV_PLAYER_TELEPORT;
		other->s.event = EV_PLAYER_TELEPORT;
	}
	other->client->ps.pmove.delta_angles = dest->s.angles - other->client->resp.cmd_angles;
	// RA2 faces the toucher the way the destination points, where vanilla (and
	// the stock rerelease teleporter) zero these and leave delta_angles alone
	// to do the turning. Only delta_angles reaches the client, so this is what
	// the server itself sees for the frame -- which is what the trackcam and
	// the observer ID view read.
	other->s.angles = dest->s.angles;
	other->client->ps.viewangles = dest->s.angles;
	other->client->v_angle = dest->s.angles;
	AngleVectors(other->client->v_angle, other->client->v_forward, nullptr, nullptr);
	gi.linkentity(other);
	KillBox(other, !!other->client);

	if (other->client->owned_sphere)
	{
		auto *sphere = other->client->owned_sphere;
		sphere->s.origin = other->s.origin;
		sphere->s.origin[2] = other->absmax[2];
		sphere->s.angles[YAW] = other->s.angles[YAW];
		gi.linkentity(sphere);
	}
}
