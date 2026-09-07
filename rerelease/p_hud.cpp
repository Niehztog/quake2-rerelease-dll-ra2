// Copyright (c) ZeniMax Media Inc.
// Licensed under the GNU General Public License 2.0.
#include "g_local.h"
#include "g_statusbar.h"
#include "rocketarena2/arena.h"
#include "rocketarena2/ra2_menu.h"

/*
======================================================================

INTERMISSION

======================================================================
*/

constexpr size_t MAX_SCOREBOARD_SIZE = 1024;

void DeathmatchScoreboard(edict_t *ent);

namespace
{
	[[nodiscard]] bool RA2_AppendLayout(std::string &layout, const std::string &entry)
	{
		if ((layout.length() + entry.length()) > MAX_SCOREBOARD_SIZE)
			return false;

		layout += entry;
		return true;
	}
}

void Serverwide_ScoreboardMessage(edict_t *ent);
void Arena_ScoreboardMessage(edict_t *ent);
void Pickup_ScoreboardMessage(edict_t *ent);

void MoveClientToIntermission(edict_t *ent)
{
	// [Paril-KEX]
	if (ent->client->ps.pmove.pm_type != PM_FREEZE)
		ent->s.event = EV_OTHER_TELEPORT;
	if (deathmatch->integer)
		ent->client->showscores = true;
	if (ra2->integer)
	{
		clear_menus(ent);
		ent->client->scoremode = 2;
	}
	ent->s.origin = level.intermission_origin;
	ent->client->ps.pmove.origin = level.intermission_origin;
	ent->client->ps.viewangles = level.intermission_angle;
	ent->client->ps.pmove.pm_type = PM_FREEZE;
	ent->client->ps.gunindex = 0;
	ent->client->ps.gunskin = 0;
	ent->client->ps.damage_blend[3] = ent->client->ps.screen_blend[3] = 0;
	ent->client->ps.rdflags = RDF_NONE;

	// clean up powerup info
	ent->client->quad_time = 0_ms;
	ent->client->invincible_time = 0_ms;
	ent->client->breather_time = 0_ms;
	ent->client->enviro_time = 0_ms;
	ent->client->invisible_time = 0_ms;
	ent->client->grenade_blew_up = false;
	ent->client->grenade_time = 0_ms;
	
	ent->client->showhelp = false;
	ent->client->showscores = false;

	globals.server_flags &= ~SERVER_FLAG_SLOW_TIME;

	// RAFAEL
	ent->client->quadfire_time = 0_ms;
	// RAFAEL
	// ROGUE
	ent->client->ir_time = 0_ms;
	ent->client->nuke_time = 0_ms;
	ent->client->double_time = 0_ms;
	ent->client->tracker_pain_time = 0_ms;
	// ROGUE

	ent->viewheight = 0;
	ent->s.modelindex = 0;
	ent->s.modelindex2 = 0;
	ent->s.modelindex3 = 0;
	ent->s.modelindex = 0;
	ent->s.effects = EF_NONE;
	ent->s.sound = 0;
	ent->solid = SOLID_NOT;
	ent->movetype = MOVETYPE_NOCLIP;

	gi.linkentity(ent);

	// add the layout

	if (deathmatch->integer)
	{
		DeathmatchScoreboard(ent);
		ent->client->showscores = true;
	}
}

// [Paril-KEX] update the level entry for end-of-unit screen
void G_UpdateLevelEntry()
{
	if (!level.entry)
		return;
	
	level.entry->found_secrets = level.found_secrets;
	level.entry->total_secrets = level.total_secrets;
	level.entry->killed_monsters = level.killed_monsters;
	level.entry->total_monsters = level.total_monsters;
}

inline void G_EndOfUnitEntry(std::stringstream &layout, const int &y, const level_entry_t &entry)
{
	layout << G_Fmt("yv {} ", y);

	// we didn't visit this level, so print it as an unknown entry
	if (!*entry.pretty_name)
	{
		layout << "table_row 1 ??? ";
		return;
	}

	layout << G_Fmt("table_row 4 \"{}\" ", entry.pretty_name) << 
		G_Fmt("{}/{} ", entry.killed_monsters, entry.total_monsters) << 
		G_Fmt("{}/{} ", entry.found_secrets, entry.total_secrets);

	int32_t minutes = entry.time.milliseconds() / 60000;
	int32_t seconds = (entry.time.milliseconds() / 1000) % 60;
	int32_t milliseconds = entry.time.milliseconds() % 1000;

	layout << G_Fmt("{:02}:{:02}:{:03} ", minutes, seconds, milliseconds);
}

void G_EndOfUnitMessage()
{
	// [Paril-KEX] update game level entry
	G_UpdateLevelEntry();

	std::stringstream layout;

	// sort entries
	std::sort(game.level_entries.begin(), game.level_entries.end(), [](const level_entry_t &a, const level_entry_t &b) {
		int32_t a_order = a.visit_order ? a.visit_order : (*a.pretty_name ? (MAX_LEVELS_PER_UNIT + 1) : (MAX_LEVELS_PER_UNIT + 2));
		int32_t b_order = b.visit_order ? b.visit_order : (*b.pretty_name ? (MAX_LEVELS_PER_UNIT + 1) : (MAX_LEVELS_PER_UNIT + 2));

		return a_order < b_order;
	});

	layout << "start_table 4 $m_eou_level $m_eou_kills $m_eou_secrets $m_eou_time ";

	int y = 16;
	level_entry_t totals {};
	int32_t num_rows = 0;

	for (auto &entry : game.level_entries)
	{
		if (!*entry.map_name)
			break;

		G_EndOfUnitEntry(layout, y, entry);

		y += 8;
		
		totals.found_secrets += entry.found_secrets;
		totals.killed_monsters += entry.killed_monsters;
		totals.time += entry.time;
		totals.total_monsters += entry.total_monsters;
		totals.total_secrets += entry.total_secrets;

		if (entry.visit_order)
			num_rows++;
	}

	y += 8;

	// make this a space so it prints totals
	if (num_rows > 1)
	{
		layout << "table_row 0 "; // empty row to separate totals
		totals.pretty_name[0] = ' ';
		G_EndOfUnitEntry(layout, y, totals);
	}

	layout << "xv 160 yt 0 draw_table ";

	layout << "ifgef " << (level.intermission_server_frame + (5_sec).frames()) << " yb -48 xv 0 loc_cstring2 0 \"$m_eou_press_button\" endif ";

	gi.WriteByte(svc_layout);
	gi.WriteString(layout.str().c_str());
	gi.multicast(vec3_origin, MULTICAST_ALL, true);

	for (auto player : active_players())
		player->client->showeou = true;
}

// data is binary now.
// u8 num_teams
// u8 num_players
// [ repeat num_teams:
//   string team_name
// ]
// [ repeat num_players:
//   u8 client_index
//   s32 score
//   u8 ranking
//   (if num_teams > 0)
//     u8 team
// ]
void G_ReportMatchDetails(bool is_end)
{
	static std::array<uint32_t, MAX_CLIENTS> player_ranks;

	player_ranks = {};

	// CTF/TDM is simple
	if (ctf->integer || teamplay->integer)
	{
		CTFCalcRankings(player_ranks);

		gi.WriteByte(2);
		gi.WriteString("RED TEAM"); // team 0
		gi.WriteString("BLUE TEAM"); // team 1
	}
	else
	{
		// sort players by score, then match everybody to
		// the current highest score downwards until we run out of players.
		static std::array<edict_t *, MAX_CLIENTS> sorted_players;
		size_t num_active_players = 0;

		for (auto player : active_players())
			sorted_players[num_active_players++] = player;

		std::sort(sorted_players.begin(), sorted_players.begin() + num_active_players, [](const edict_t *a, const edict_t *b) { return b->client->resp.score < a->client->resp.score; });

		int32_t current_score = INT_MIN;
		int32_t current_rank = 0;

		for (size_t i = 0; i < num_active_players; i++)
		{
			if (!current_rank || sorted_players[i]->client->resp.score != current_score)
			{
				current_rank++;
				current_score = sorted_players[i]->client->resp.score;
			}

			player_ranks[sorted_players[i]->s.number - 1] = current_rank;
		}

		gi.WriteByte(0);
	}

	uint8_t num_players = 0;

	for (auto player : active_players())
	{
		// leave spectators out of this data, they don't need to be seen.
		if (player->client->pers.spawned && !player->client->resp.spectator)
		{
			// just in case...
			if (G_TeamplayEnabled() && player->client->resp.ctf_team == CTF_NOTEAM)
				continue;

			num_players++;
		}
	}

	gi.WriteByte(num_players);

	for (auto player : active_players())
	{
		// leave spectators out of this data, they don't need to be seen.
		if (player->client->pers.spawned && !player->client->resp.spectator)
		{
			// just in case...
			if (G_TeamplayEnabled() && player->client->resp.ctf_team == CTF_NOTEAM)
				continue;

			gi.WriteByte(player->s.number - 1);
			gi.WriteLong(player->client->resp.score);
			gi.WriteByte(player_ranks[player->s.number - 1]);

			if (G_TeamplayEnabled())
				gi.WriteByte(player->client->resp.ctf_team == CTF_TEAM1 ? 0 : 1);
		}
	}

	gi.ReportMatchDetails_Multicast(is_end);
}

void BeginIntermission(edict_t *targ)
{
	edict_t *ent, *client;
	int32_t count = 0;

	if (level.intermissiontime)
		return; // already activated

	// ZOID
	if (ctf->integer)
		CTFCalcScores();
	// ZOID

	game.autosaved = false;

	level.intermissiontime = level.time;

	// respawn any dead clients
	for (uint32_t i = 0; i < game.maxclients; i++)
	{
		client = g_edicts + 1 + i;
		if (!client->inuse)
			continue;
		if (client->health <= 0)
		{
			// give us our max health back since it will reset
			// to pers.health; in instanced items we'd lose the items
			// we touched so we always want to respawn with our max.
			if (P_UseCoopInstancedItems())
				client->client->pers.health = client->client->pers.max_health = client->max_health;

			respawn(client);
		}
	}

	level.intermission_server_frame = gi.ServerFrame();
	level.changemap = targ->map;
	level.intermission_clear = targ->spawnflags.has(SPAWNFLAG_CHANGELEVEL_CLEAR_INVENTORY);
	level.intermission_eou = false;
	level.intermission_fade = targ->spawnflags.has(SPAWNFLAG_CHANGELEVEL_FADE_OUT);

	// destroy all player trails
	PlayerTrail_Destroy(nullptr);

	// [Paril-KEX] update game level entry
	G_UpdateLevelEntry();

	if (strstr(level.changemap, "*"))
	{
		if (coop->integer)
		{
			for (uint32_t i = 0; i < game.maxclients; i++)
			{
				client = g_edicts + 1 + i;
				if (!client->inuse)
					continue;
				// strip players of all keys between units
				for (uint32_t n = 0; n < IT_TOTAL; n++)
					if (itemlist[n].flags & IF_KEY)
						client->client->pers.inventory[n] = 0;
			}
		}

		if (level.achievement && level.achievement[0])
		{
			gi.WriteByte(svc_achievement);
			gi.WriteString(level.achievement);
			gi.multicast(vec3_origin, MULTICAST_ALL, true);
		}

		level.intermission_eou = true;

		// "no end of unit" maps handle intermission differently
		if (!targ->spawnflags.has(SPAWNFLAG_CHANGELEVEL_NO_END_OF_UNIT))
			G_EndOfUnitMessage();
		else if (targ->spawnflags.has(SPAWNFLAG_CHANGELEVEL_IMMEDIATE_LEAVE) && !deathmatch->integer)
		{
			// Need to call this now
			G_ReportMatchDetails(true);
			level.exitintermission = 1; // go immediately to the next level
			return;
		}
	}
	else
	{
		if (!deathmatch->integer)
		{
			level.exitintermission = 1; // go immediately to the next level
			return;
		}
	}

	// Call while intermission is running
	G_ReportMatchDetails(true);

	level.exitintermission = 0;

	if (!level.level_intermission_set)
	{
		// find an intermission spot
		ent = G_FindByString<&edict_t::classname>(nullptr, "info_player_intermission");
		if (!ent)
		{ // the map creator forgot to put in an intermission point...
			ent = G_FindByString<&edict_t::classname>(nullptr, "info_player_start");
			if (!ent)
				ent = G_FindByString<&edict_t::classname>(nullptr, "info_player_deathmatch");
		}
		else
		{ // choose one of four spots
			int32_t i = irandom(4);
			while (i--)
			{
				ent = G_FindByString<&edict_t::classname>(ent, "info_player_intermission");
				if (!ent) // wrap around the list
					ent = G_FindByString<&edict_t::classname>(ent, "info_player_intermission");
			}
		}

		level.intermission_origin = ent->s.origin;
		level.intermission_angle = ent->s.angles;
	}

	// move all clients to the intermission point
	for (uint32_t i = 0; i < game.maxclients; i++)
	{
		client = g_edicts + 1 + i;
		if (!client->inuse)
			continue;
		count++;
		MoveClientToIntermission(client);
	}

	if (ra2->integer && !count)
		level.exitintermission = 1;
}

/*
==================
Serverwide_ScoreboardMessage

==================
*/
void Serverwide_ScoreboardMessage(edict_t *ent)
{
	std::string entry, line, layout;
	int32_t sorted[MAX_CLIENTS];
	int32_t sortedscores[MAX_CLIENTS];
	int32_t total = 0;

	for (uint32_t i = 0; i < game.maxclients; i++)
	{
		auto *cl_ent = g_edicts + 1 + i;
		if (!cl_ent->inuse)
			continue;

		int32_t score = game.clients[i].resp.score;
		uint32_t j = 0;
		for (; j < (uint32_t) total; j++)
			if (score > sortedscores[j])
				break;

		for (int32_t k = total; k > (int32_t) j; k--)
		{
			sorted[k] = sorted[k - 1];
			sortedscores[k] = sortedscores[k - 1];
		}

		sorted[j] = i;
		sortedscores[j] = score;
		total++;
	}

	// the header, then the hairline rule the original ruled under it. RA2 wrote
	// that rule as 37 bytes of 0x9b through string2, which XORs 0x80 back off
	// and lands on charset glyph 0x1b -- an 8x1 bar along the *top* of its cell.
	// Kex reads layout strings as UTF-8, so a byte over 0x7f cannot be spelled
	// here any more; '_' is the same 8x1 bar along the *bottom* of its cell, so
	// drawing it seven pixels higher puts the line back on the original's row.
	if (!RA2_AppendLayout(layout, "xv 0 yv 32 string2 \"Frags Ping   Name        Team       A\" "
								  "xv 0 yv 33 string \"_____________________________________\" "))
		return;

	if (total > 23)
		total = 23;

	for (int32_t i = 0; i < total; i++)
	{
		auto *cl = &game.clients[sorted[i]];
		auto *cl_ent = g_edicts + 1 + sorted[i];
		const char *teamname = "None";

		if (cl->resp.teamnum > -1 && cl->resp.teamnum < MAX_TEAMS && teams[cl->resp.teamnum].it)
			teamname = TEAM(&teams[cl->resp.teamnum])->name.c_str();

		char buffer[128];
		std::snprintf(buffer, sizeof(buffer), "%3d %4d %12.12s %12.12s %1d",
			cl->resp.score, cl->ping, RA2_PlayerName(cl_ent).c_str(), teamname, cl->resp.context);
		line = buffer;

		// the viewing player's own row draws plain, everyone else's in the alternate
		// charset. Kex reads strings as UTF-8, so shifting bytes up by 0x80 can't
		// express this any more -- the layout command carries the colour instead.
		entry = G_Fmt("xv 8 yv {} {} \"{}\" ", i * 8 + 48, (cl_ent == ent) ? "string" : "string2", line);
		if (!RA2_AppendLayout(layout, entry))
			break;
	}

	gi.WriteByte(svc_layout);
	gi.WriteString(layout.c_str());
}

/*
==================
Arena_ScoreboardMessage

==================
*/
void Arena_ScoreboardMessage(edict_t *ent)
{
	std::string entry, line, layout;
	int32_t sortedteams[MAX_TEAMS];
	int32_t teamscores[MAX_TEAMS];
	int32_t teampings[MAX_TEAMS];
	int32_t sortedplayers[MAX_CLIENTS];
	int32_t playerscores[MAX_CLIENTS];
	int32_t total = 0;
	int32_t arenanum = ent->client->resp.context;

	for (int32_t i = 0; i < MAX_TEAMS; i++)
	{
		auto *t = teams[i].it ? TEAM(&teams[i]) : nullptr;
		if (!t || t->arenanum != arenanum || t->outofline)
			continue;

		int32_t score = 0;
		int32_t ping = 0;
		int32_t count = 0;

		for (qmenu_t *node = teams[i].next; node; node = node->next)
		{
			auto *cl_ent = static_cast<edict_t *>(node->it);
			score += cl_ent->client->resp.score;
			ping += cl_ent->client->ping;
			count++;
		}

		if (!count)
			continue;

		ping /= count;

		int32_t j = 0;
		for (; j < total; j++)
			if (score > teamscores[j])
				break;

		for (int32_t k = total; k > j; k--)
		{
			sortedteams[k] = sortedteams[k - 1];
			teamscores[k] = teamscores[k - 1];
			teampings[k] = teampings[k - 1];
		}

		sortedteams[j] = i;
		teamscores[j] = score;
		teampings[j] = ping;
		total++;
	}

	if (!RA2_AppendLayout(layout, "xv 0 yv 40 string2 \"Teams\" xv 160 string2 \"Players\" "))
		return;

	int32_t row = 1;
	if (total > 20)
		total = 20;

	for (int32_t i = 0; i < total; i++)
	{
		auto *t = TEAM(&teams[sortedteams[i]]);

		char buffer[128];
		std::snprintf(buffer, sizeof(buffer), "%-2d %-3d %.11s", teamscores[i], teampings[i], t->name.c_str());
		line = buffer;

		// a team that's fighting draws plain, the ones waiting in the alternate charset
		entry = G_Fmt("xv 0 yv {} {} \"{}\" ", row * 8 + 40, t->fighting ? "string" : "string2", line);
		if (!RA2_AppendLayout(layout, entry))
			break;

		int32_t totalplayers = 0;
		for (qmenu_t *node = static_cast<qmenu_t *>(t->arenalink.it)->next; node; node = node->next)
		{
			auto *cl_ent = static_cast<edict_t *>(node->it);
			int32_t score = cl_ent->client->resp.score;
			int32_t j = 0;

			for (; j < totalplayers; j++)
				if (score > playerscores[j])
					break;

			for (int32_t k = totalplayers; k > j; k--)
			{
				sortedplayers[k] = sortedplayers[k - 1];
				playerscores[k] = playerscores[k - 1];
			}

			sortedplayers[j] = static_cast<int32_t>(cl_ent - g_edicts - 1);
			playerscores[j] = score;
			totalplayers++;
		}

		if (totalplayers > 20)
			totalplayers = 20;

		for (int32_t j = 0; j < totalplayers; j++)
		{
			auto *cl_ent = g_edicts + 1 + sortedplayers[j];
			auto *cl = &game.clients[sortedplayers[j]];

			std::snprintf(buffer, sizeof(buffer), "%-2d %-3d %.11s", cl->resp.score, cl->ping,
				RA2_PlayerName(cl_ent).c_str());
			line = buffer;

			// a live fighter draws plain, the dead and the audience in the alternate charset
			entry = G_Fmt("xv 160 yv {} {} \"{}\" ", row * 8 + 40, cl_ent->takedamage ? "string" : "string2", line);
			if (!RA2_AppendLayout(layout, entry))
				break;
			row++;
		}
	}

	gi.WriteByte(svc_layout);
	gi.WriteString(layout.c_str());
}

/*
==================
Pickup_ScoreboardMessage

==================
*/
void Pickup_ScoreboardMessage(edict_t *ent)
{
	std::string entry, layout;
	int32_t scores[MAX_CLIENTS];
	int32_t redsorted[MAX_CLIENTS];
	int32_t bluesorted[MAX_CLIENTS];
	int32_t redtotal = 0, bluetotal = 0;
	int32_t redwins = 0, bluewins = 0;

	for (uint32_t i = 0; i < game.maxclients; i++)
	{
		auto *cl_ent = g_edicts + 1 + i;
		if (!cl_ent->inuse || cl_ent->client->resp.context != ent->client->resp.context || cl_ent->client->resp.teamnum < 0)
			continue;
		if (TEAM(&teams[cl_ent->client->resp.teamnum])->side)
			continue;

		int32_t score = game.clients[i].resp.score;
		uint32_t j = 0;
		for (; j < (uint32_t) redtotal; j++)
			if (score > scores[j])
				break;
		for (int32_t k = redtotal; k > (int32_t) j; k--)
		{
			redsorted[k] = redsorted[k - 1];
			scores[k] = scores[k - 1];
		}
		redsorted[j] = i;
		scores[j] = score;
		redwins = TEAM(&teams[cl_ent->client->resp.teamnum])->wins;
		redtotal++;
	}

	for (uint32_t i = 0; i < game.maxclients; i++)
	{
		auto *cl_ent = g_edicts + 1 + i;
		if (!cl_ent->inuse || cl_ent->client->resp.context != ent->client->resp.context || cl_ent->client->resp.teamnum < 0)
			continue;
		if (TEAM(&teams[cl_ent->client->resp.teamnum])->side != 1)
			continue;

		int32_t score = game.clients[i].resp.score;
		uint32_t j = 0;
		for (; j < (uint32_t) bluetotal; j++)
			if (score > scores[j])
				break;
		for (int32_t k = bluetotal; k > (int32_t) j; k--)
		{
			bluesorted[k] = bluesorted[k - 1];
			scores[k] = scores[k - 1];
		}
		bluesorted[j] = i;
		scores[j] = score;
		bluewins = TEAM(&teams[cl_ent->client->resp.teamnum])->wins;
		bluetotal++;
	}

	redwins = std::max(redwins, 0);
	bluewins = std::max(bluewins, 0);
	layout = G_Fmt("xv 0 yv 40 string2 \"Team Red  : {}\" xv 160 yv 40 string2 \"Team Blue : {}\" ", redwins, bluewins);

	if (redtotal > 20)
		redtotal = 20;
	if (bluetotal > 20)
		bluetotal = 20;

	// RA2 coloured only the name on this board: HiPrint/LoPrint were handed the
	// netname on its own, so the score and ping columns stayed in the alternate
	// charset whatever the player was doing, and just the name went plain while
	// they were alive. Reaching only the name needs a second draw command per
	// player here, roughly 60 more bytes of MAX_SCOREBOARD_SIZE per row, so a
	// board fuller than about 7-a-side loses its last rows to the cap.
	const auto append_player = [&](int32_t x, int32_t y, int32_t clientnum) -> bool
	{
		auto *cl = &game.clients[clientnum];
		auto *cl_ent = g_edicts + 1 + clientnum;

		char cols[16];
		std::snprintf(cols, sizeof(cols), "%2d %3d", cl->resp.score, cl->ping);

		char name[16];
		std::snprintf(name, sizeof(name), "%.12s", RA2_PlayerName(cl_ent).c_str());

		// the name sits one space past the columns, where the single
		// "%2d %3d %.12s" the original formatted would have placed it
		const int32_t name_x = x + 8 * (int32_t) (strlen(cols) + 1);

		// both halves in one entry, so the cap can never leave a row's columns
		// standing without the name they belong to
		entry = G_Fmt("xv {} yv {} string2 \"{}\" xv {} yv {} {} \"{}\" ",
			x, y, cols, name_x, y, cl_ent->takedamage ? "string" : "string2", name);
		return RA2_AppendLayout(layout, entry);
	};

	for (int32_t i = 0, y = 48; i < redtotal || i < bluetotal; i++, y += 8)
	{
		if (i < redtotal && !append_player(0, y, redsorted[i]))
			break;

		if (i < bluetotal && !append_player(160, y, bluesorted[i]))
			break;
	}

	gi.WriteByte(svc_layout);
	gi.WriteString(layout.c_str());
}

/*
==================
DeathmatchScoreboardMessage

==================
*/
void DeathmatchScoreboardMessage(edict_t *ent, edict_t *killer)
{
	static std::string entry, string;
	size_t		j;
	int			sorted[MAX_CLIENTS];
	int			sortedscores[MAX_CLIENTS];
	int			score;
	int			x, y;
	gclient_t  *cl;
	edict_t	*cl_ent;
	const char *tag;

	if (ra2->integer)
	{
		if (!ent->client->resp.context && ent->client->scoremode == 1)
			ent->client->scoremode = 2;

		if (ent->client->scoremode == 2)
		{
			Serverwide_ScoreboardMessage(ent);
			return;
		}

		if (arenas[ent->client->resp.context].idarena)
			Pickup_ScoreboardMessage(ent);
		else
			Arena_ScoreboardMessage(ent);
		return;
	}

	// ZOID
	if (G_TeamplayEnabled())
	{
		CTFScoreboardMessage(ent, killer);
		return;
	}
	// ZOID

	entry.clear();
	string.clear();

	//  sort the clients by score
	uint32_t total = 0;
	for (uint32_t i = 0; i < game.maxclients; i++)
	{
		cl_ent = g_edicts + 1 + i;
		if (!cl_ent->inuse || game.clients[i].resp.spectator)
			continue;
		score = game.clients[i].resp.score;
		for (j = 0; j < total; j++)
		{
			if (score > sortedscores[j])
				break;
		}
		for (uint32_t k = total; k > j; k--)
		{
			sorted[k] = sorted[k - 1];
			sortedscores[k] = sortedscores[k - 1];
		}
		sorted[j] = i;
		sortedscores[j] = score;
		total++;
	}

	// add the clients in sorted order
	if (total > 16)
		total = 16;

	for (uint32_t i = 0; i < total; i++)
	{
		cl = &game.clients[sorted[i]];
		cl_ent = g_edicts + 1 + sorted[i];

		x = (i >= 8) ? 130 : -72;
		y = 0 + 32 * (i % 8);

		// add a dogtag
		// [Paril-KEX] use dynamic dogtags
		tag = nullptr;

		//===============
		// ROGUE
		// allow new DM games to override the tag picture
		if (gamerules->integer)
		{
			if (DMGame.DogTag)
				DMGame.DogTag(cl_ent, killer, &tag);
		}
		// ROGUE
		//===============

		if (tag)
		{
			fmt::format_to(std::back_inserter(entry), FMT_STRING("xv {} yv {} picn {} "), x + 32, y, tag);

			if (string.length() + entry.length() > MAX_SCOREBOARD_SIZE)
				break;

			string += entry;
		}
		else
		{
			fmt::format_to(std::back_inserter(entry), FMT_STRING("xv {} yv {} dogtag {} "), x + 32, y, sorted[i]);

			if (string.length() + entry.length() > MAX_SCOREBOARD_SIZE)
				break;

			string += entry;
		}

		entry.clear();

		fmt::format_to(std::back_inserter(entry),
					FMT_STRING("client {} {} {} {} {} {} "),
					x, y, sorted[i], cl->resp.score, cl->ping, (int32_t) (level.time - cl->resp.entertime).minutes());

		if (string.length() + entry.length() > MAX_SCOREBOARD_SIZE)
			break;

		string += entry;

		entry.clear();
	}

	// [Paril-KEX] time & frags
	if (fraglimit->integer)
	{
		fmt::format_to(std::back_inserter(string), FMT_STRING("xv -20 yv -10 loc_string2 1 $g_score_frags \"{}\" "), fraglimit->integer);
	}
	if (timelimit->value && !level.intermissiontime)
	{
		fmt::format_to(std::back_inserter(string), FMT_STRING("xv 340 yv -10 time_limit {} "), gi.ServerFrame() + ((gtime_t::from_min(timelimit->value) - level.time)).milliseconds() / gi.frame_time_ms);
	}

	if (level.intermissiontime)
		fmt::format_to(std::back_inserter(string), FMT_STRING("ifgef {} yb -48 xv 0 loc_cstring2 0 \"$m_eou_press_button\" endif "), (level.intermission_server_frame + (5_sec).frames()));

	gi.WriteByte(svc_layout);
	gi.WriteString(string.c_str());
}

/*
==================
DeathmatchScoreboard

Draw instead of help message.
Note that it isn't that hard to overflow the 1400 byte message limit!
==================
*/
void DeathmatchScoreboard(edict_t *ent)
{
	DeathmatchScoreboardMessage(ent, ent->enemy);

	if (ra2->integer)
		gi.unicast(ent, ent->client->scoremode == 2);
	else
		gi.unicast(ent, true);
	ent->client->menutime = level.time + 3_sec;
}

/*
==================
Cmd_Score_f

Display the scoreboard
==================
*/
void Cmd_Score_f(edict_t *ent)
{
	if (level.intermissiontime)
		return;

	ent->client->showinventory = false;
	ent->client->showhelp = false;

	globals.server_flags &= ~SERVER_FLAG_SLOW_TIME;

	// ZOID
	if (ent->client->menu)
		PMenu_Close(ent);
	// ZOID

	if (!deathmatch->integer && !coop->integer)
		return;

	if (ra2->integer)
	{
		// RA2's cycle. 2 always steps back to 0, which is the "no board" end
		// of it; from anywhere else the lobby (context 0) jumps straight to 2,
		// since arena 0 has no arena page of its own, while a real arena steps
		// up through 1 -- its own arena or pickup page -- to reach 2. So a
		// player in an arena sees arena, serverwide, nothing, and one in the
		// lobby just toggles serverwide on and off.
		if (ent->client->scoremode == 2)
			ent->client->scoremode = 0;
		else if (!ent->client->resp.context)
			ent->client->scoremode = 2;
		else
			ent->client->scoremode++;

		// scoremode 0 is "no board", which is how the cycle ends -- the original
		// read scoremode straight into STAT_LAYOUTS, where the rerelease reads
		// showscores, so the flag has to follow the mode or the last board would
		// stay on screen until something else cleared it.
		if (ent->client->scoremode)
		{
			// one layout channel, so a board and a menu cannot both be up: park
			// the menu on its stack and let TAB bring it back.
			ent->client->showmenu = false;
			ent->client->showscores = true;
			DeathmatchScoreboard(ent);
		}
		else
		{
			DisplayMenu(ent);
		}
		return;
	}

	if (ent->client->showscores)
	{
		ent->client->showscores = false;
		ent->client->update_chase = true;
		return;
	}

	ent->client->showscores = true;
	DeathmatchScoreboard(ent);
}

/*
==================
HelpComputer

Draw help computer.
==================
*/
void HelpComputer(edict_t *ent)
{
	const char *sk;

	if (skill->integer == 0)
		sk = "$m_easy";
	else if (skill->integer == 1)
		sk = "$m_medium";
	else if (skill->integer == 2)
		sk = "$m_hard";
	else
		sk = "$m_nightmare";

	// send the layout

	std::string helpString = "";
	helpString += G_Fmt(
		"xv 32 yv 8 picn help "		   // background
		"xv 0 yv 25 cstring2 \"{}\" ",  // level name
		level.level_name);

	if (level.is_n64)
	{
		helpString += G_Fmt("xv 0 yv 54 loc_cstring 1 \"{{}}\" \"{}\" ",  // help 1
			game.helpmessage1);
	}
	else 
	{
		int y = 54;
		if (strlen(game.helpmessage1))
		{
			helpString += G_Fmt("xv 0 yv {} loc_cstring2 0 \"$g_pc_primary_objective\" "  // title
				"xv 0 yv {} loc_cstring 0 \"{}\" ",
				y,
				y + 11,
				game.helpmessage1);

			y += 58;
		}

		if (strlen(game.helpmessage2))
		{
			helpString += G_Fmt("xv 0 yv {} loc_cstring2 0 \"$g_pc_secondary_objective\" "  // title
				"xv 0 yv {} loc_cstring 0 \"{}\" ",
				y,
				y + 11,
				game.helpmessage2);
		}

	}

	helpString += G_Fmt("xv 55 yv 164 loc_string2 0 \"{}\" "
		"xv 265 yv 164 loc_rstring2 1 \"{{}}: {}/{}\" \"$g_pc_goals\" "
		"xv 55 yv 172 loc_string2 1 \"{{}}: {}/{}\" \"$g_pc_kills\" "
		"xv 265 yv 172 loc_rstring2 1 \"{{}}: {}/{}\" \"$g_pc_secrets\" ",
		sk,
		level.found_goals, level.total_goals,
		level.killed_monsters, level.total_monsters,
		level.found_secrets, level.total_secrets);

	gi.WriteByte(svc_layout);
	gi.WriteString(helpString.c_str());
	gi.unicast(ent, true);
}

/*
==================
Cmd_Help_f

Display the current help message
==================
*/
void Cmd_Help_f(edict_t *ent)
{
	// this is for backwards compatability
	if (deathmatch->integer)
	{
		Cmd_Score_f(ent);
		return;
	}

	if (level.intermissiontime)
		return;

	ent->client->showinventory = false;
	ent->client->showscores = false;
	if (ra2->integer)
		ent->client->scoremode = 0;

	if (ent->client->showhelp &&
			(ent->client->pers.game_help1changed == game.help1changed ||
			ent->client->pers.game_help2changed == game.help2changed))
	{
		ent->client->showhelp = false;
		globals.server_flags &= ~SERVER_FLAG_SLOW_TIME;
		return;
	}

	ent->client->showhelp = true;
	ent->client->pers.helpchanged = 0;
	globals.server_flags |= SERVER_FLAG_SLOW_TIME;
	HelpComputer(ent);
}

//=======================================================================

// [Paril-KEX] for stats we want to always be set in coop
// even if we're spectating
void G_SetCoopStats(edict_t *ent)
{
	if (coop->integer && g_coop_enable_lives->integer)
		ent->client->ps.stats[STAT_LIVES] = ent->client->pers.lives + 1;
	else
		ent->client->ps.stats[STAT_LIVES] = 0;

	// stat for text on what we're doing for respawn
	if (ent->client->coop_respawn_state)
		ent->client->ps.stats[STAT_COOP_RESPAWN] = CONFIG_COOP_RESPAWN_STRING + (ent->client->coop_respawn_state - COOP_RESPAWN_IN_COMBAT);
	else
		ent->client->ps.stats[STAT_COOP_RESPAWN] = 0;
}

struct powerup_info_t
{
	item_id_t item;
	gtime_t gclient_t::*time_ptr = nullptr;
	int32_t gclient_t::*count_ptr = nullptr;
} powerup_table[] = {
	{ IT_ITEM_QUAD, &gclient_t::quad_time },
	{ IT_ITEM_QUADFIRE, &gclient_t::quadfire_time },
	{ IT_ITEM_DOUBLE, &gclient_t::double_time },
	{ IT_ITEM_INVULNERABILITY, &gclient_t::invincible_time },
	{ IT_ITEM_INVISIBILITY, &gclient_t::invisible_time },
	{ IT_ITEM_ENVIROSUIT, &gclient_t::enviro_time },
	{ IT_ITEM_REBREATHER, &gclient_t::breather_time },
	{ IT_ITEM_IR_GOGGLES, &gclient_t::ir_time },
	{ IT_ITEM_SILENCER, nullptr, &gclient_t::silencer_shots }
};

/*
===============
G_SetStats
===============
*/
void G_SetStats(edict_t *ent)
{
	gitem_t	*item;
	item_id_t index;
	int		  cells = 0;
	item_id_t power_armor_type;
	unsigned int invIndex;

	//
	// health
	//
	if (ra2->integer)
		ent->client->ps.stats[STAT_HEALTH_ICON] = GetSkinIcon(ent);
	else if (ent->s.renderfx & RF_USE_DISGUISE)
		ent->client->ps.stats[STAT_HEALTH_ICON] = level.disguise_icon;
	else
		ent->client->ps.stats[STAT_HEALTH_ICON] = level.pic_health;
	ent->client->ps.stats[STAT_HEALTH] = ent->health;

	//
	// weapons
	//
	uint32_t weaponbits = 0;

	for (invIndex = IT_WEAPON_GRAPPLE; invIndex <= IT_WEAPON_DISRUPTOR; invIndex++)
	{
		if (ent->client->pers.inventory[invIndex])
		{
			weaponbits |= 1 << GetItemByIndex((item_id_t) invIndex)->weapon_wheel_index;
		}
	}

	ent->client->ps.stats[STAT_WEAPONS_OWNED_1] = (weaponbits & 0xFFFF);
	ent->client->ps.stats[STAT_WEAPONS_OWNED_2] = (weaponbits >> 16);

	ent->client->ps.stats[STAT_ACTIVE_WHEEL_WEAPON] = (ent->client->newweapon ? ent->client->newweapon->weapon_wheel_index :
		ent->client->pers.weapon ? ent->client->pers.weapon->weapon_wheel_index :
		-1);
	ent->client->ps.stats[STAT_ACTIVE_WEAPON] = ent->client->pers.weapon ? ent->client->pers.weapon->weapon_wheel_index : -1;

	//
	// ammo
	//
	ent->client->ps.stats[STAT_AMMO_ICON] = 0;
	ent->client->ps.stats[STAT_AMMO] = 0;

	if (ent->client->pers.weapon && ent->client->pers.weapon->ammo)
	{
		item = GetItemByIndex(ent->client->pers.weapon->ammo);

		if (!G_CheckInfiniteAmmo(item))
		{
			ent->client->ps.stats[STAT_AMMO_ICON] = gi.imageindex(item->icon);
			ent->client->ps.stats[STAT_AMMO] = ent->client->pers.inventory[ent->client->pers.weapon->ammo];
		}
	}
	
	memset(&ent->client->ps.stats[STAT_AMMO_INFO_START], 0, sizeof(uint16_t) * NUM_AMMO_STATS);
	for (unsigned int ammoIndex = AMMO_BULLETS; ammoIndex < AMMO_MAX; ++ammoIndex)
	{
		gitem_t *ammo = GetItemByAmmo((ammo_t) ammoIndex);
		uint16_t val = G_CheckInfiniteAmmo(ammo) ? AMMO_VALUE_INFINITE : clamp(ent->client->pers.inventory[ammo->id], 0, AMMO_VALUE_INFINITE - 1);
		G_SetAmmoStat((uint16_t *) &ent->client->ps.stats[STAT_AMMO_INFO_START], ammo->ammo_wheel_index, val);
	}

	//
	// armor
	//
	power_armor_type = PowerArmorType(ent);
	if (power_armor_type)
		cells = ent->client->pers.inventory[IT_AMMO_CELLS];

	index = ArmorIndex(ent);
	if (power_armor_type && (!index || (level.time.milliseconds() % 3000) < 1500))
	{ // flash between power armor and other armor icon
		ent->client->ps.stats[STAT_ARMOR_ICON] = power_armor_type == IT_ITEM_POWER_SHIELD ? gi.imageindex("i_powershield") : gi.imageindex("i_powerscreen");
		ent->client->ps.stats[STAT_ARMOR] = cells;
	}
	else if (index)
	{
		item = GetItemByIndex(index);
		ent->client->ps.stats[STAT_ARMOR_ICON] = gi.imageindex(item->icon);
		ent->client->ps.stats[STAT_ARMOR] = ent->client->pers.inventory[index];
	}
	else
	{
		ent->client->ps.stats[STAT_ARMOR_ICON] = 0;
		ent->client->ps.stats[STAT_ARMOR] = 0;
	}

	//
	// pickup message
	//
	if (level.time > ent->client->pickup_msg_time)
	{
		ent->client->ps.stats[STAT_PICKUP_ICON] = 0;
		ent->client->ps.stats[STAT_PICKUP_STRING] = 0;
	}

	// owned powerups
	memset(&ent->client->ps.stats[STAT_POWERUP_INFO_START], 0, sizeof(uint16_t) * NUM_POWERUP_STATS);
	for (unsigned int powerupIndex = POWERUP_SCREEN; powerupIndex < POWERUP_MAX; ++powerupIndex)
	{
		gitem_t *powerup = GetItemByPowerup((powerup_t) powerupIndex);
		uint16_t val;

		switch (powerup->id)
		{
		case IT_ITEM_POWER_SCREEN:
		case IT_ITEM_POWER_SHIELD:
			if (!ent->client->pers.inventory[powerup->id])
				val = 0;
			else if (ent->flags & FL_POWER_ARMOR)
				val = 2;
			else
				val = 1;
			break;
		case IT_ITEM_FLASHLIGHT:
			if (!ent->client->pers.inventory[powerup->id])
				val = 0;
			else if (ent->flags & FL_FLASHLIGHT)
				val = 2;
			else
				val = 1;
			break;
		default:
			val = clamp(ent->client->pers.inventory[powerup->id], 0, 3);
			break;
		}

		G_SetPowerupStat((uint16_t *) &ent->client->ps.stats[STAT_POWERUP_INFO_START], powerup->powerup_wheel_index, val);
	}

	ent->client->ps.stats[STAT_TIMER_ICON] = 0;
	ent->client->ps.stats[STAT_TIMER] = 0;

	//
	// timers
	//
	// PGM
	if (ent->client->owned_sphere)
	{
		if (ent->client->owned_sphere->spawnflags == SPHERE_DEFENDER) // defender
			ent->client->ps.stats[STAT_TIMER_ICON] = gi.imageindex("p_defender");
		else if (ent->client->owned_sphere->spawnflags == SPHERE_HUNTER) // hunter
			ent->client->ps.stats[STAT_TIMER_ICON] = gi.imageindex("p_hunter");
		else if (ent->client->owned_sphere->spawnflags == SPHERE_VENGEANCE) // vengeance
			ent->client->ps.stats[STAT_TIMER_ICON] = gi.imageindex("p_vengeance");
		else // error case
			ent->client->ps.stats[STAT_TIMER_ICON] = gi.imageindex("i_fixme");

		ent->client->ps.stats[STAT_TIMER] = ceil(ent->client->owned_sphere->wait - level.time.seconds());
	}
	else
	{
		powerup_info_t *best_powerup = nullptr;

		for (auto &powerup : powerup_table)
		{
			auto *powerup_time = powerup.time_ptr ? &(ent->client->*powerup.time_ptr) : nullptr;
			auto *powerup_count = powerup.count_ptr ? &(ent->client->*powerup.count_ptr) : nullptr;

			if (powerup_time && *powerup_time <= level.time)
				continue;
			else if (powerup_count && !*powerup_count)
				continue;

			if (!best_powerup)
			{
				best_powerup = &powerup;
				continue;
			}
			
			if (powerup_time && *powerup_time < ent->client->*best_powerup->time_ptr)
			{
				best_powerup = &powerup;
				continue;
			}
			else if (powerup_count && !best_powerup->time_ptr)
			{
				best_powerup = &powerup;
				continue;
			}
		}

		if (best_powerup)
		{
			int16_t value;

			if (best_powerup->count_ptr)
				value = (ent->client->*best_powerup->count_ptr);
			else
				value = ceil((ent->client->*best_powerup->time_ptr - level.time).seconds());

			ent->client->ps.stats[STAT_TIMER_ICON] = gi.imageindex(GetItemByIndex(best_powerup->item)->icon);
			ent->client->ps.stats[STAT_TIMER] = value;
		}
	}
	// PGM

	//
	// selected item
	//
	ent->client->ps.stats[STAT_SELECTED_ITEM] = ent->client->pers.selected_item;

	if (ent->client->pers.selected_item == IT_NULL)
		ent->client->ps.stats[STAT_SELECTED_ICON] = 0;
	else
	{
		ent->client->ps.stats[STAT_SELECTED_ICON] = gi.imageindex(itemlist[ent->client->pers.selected_item].icon);

		if (ent->client->pers.selected_item_time < level.time)
			ent->client->ps.stats[STAT_SELECTED_ITEM_NAME] = 0;
	}

	//
	// layouts
	//
	ent->client->ps.stats[STAT_LAYOUTS] = 0;

	if (deathmatch->integer)
	{
		if (ra2->integer)
		{
			// RA2 owns this channel outright, and RA2_LayoutFlag is the one rule
			// for it -- including at intermission, whose board is scoremode 2
			// (MoveClientToIntermission) and must not be putaway-able either.
			// The stock dead-player term is dead code here: pers.health is only
			// written by InitClientPersistant/PutClientInServer/SaveClientData
			// and stays 100 for an RA2 client's whole life. A dying fighter gets
			// its board from player_die -> Cmd_Help_f -> Cmd_Score_f, which sets
			// scoremode, so RA2_LayoutFlag already covers it.
			ent->client->ps.stats[STAT_LAYOUTS] |= RA2_LayoutFlag(ent);
		}
		else if (ent->client->pers.health <= 0 || level.intermissiontime || ent->client->showscores)
			ent->client->ps.stats[STAT_LAYOUTS] |= LAYOUTS_LAYOUT;
		if (ent->client->showinventory && ent->client->pers.health > 0)
			ent->client->ps.stats[STAT_LAYOUTS] |= LAYOUTS_INVENTORY;
	}
	else
	{
		if (ent->client->showscores || ent->client->showhelp || ent->client->showeou)
			ent->client->ps.stats[STAT_LAYOUTS] |= LAYOUTS_LAYOUT;
		if (ent->client->showinventory && ent->client->pers.health > 0)
			ent->client->ps.stats[STAT_LAYOUTS] |= LAYOUTS_INVENTORY;

		if (ent->client->showhelp)
			ent->client->ps.stats[STAT_LAYOUTS] |= LAYOUTS_HELP;
	}

	if (level.intermissiontime || ent->client->awaiting_respawn)
	{
		if (ent->client->awaiting_respawn || (level.intermission_eou || level.is_n64 || (deathmatch->integer && level.intermissiontime)))
			ent->client->ps.stats[STAT_LAYOUTS] |= LAYOUTS_HIDE_HUD;

		// N64 always merges into one screen on level ends
		if (level.intermission_eou || level.is_n64 || (deathmatch->integer && level.intermissiontime))
			ent->client->ps.stats[STAT_LAYOUTS] |= LAYOUTS_INTERMISSION;
	}
	
	if (level.story_active)
		ent->client->ps.stats[STAT_LAYOUTS] |= LAYOUTS_HIDE_CROSSHAIR;
	else
		ent->client->ps.stats[STAT_LAYOUTS] &= ~LAYOUTS_HIDE_CROSSHAIR;

	// [Paril-KEX] key display
	if (!deathmatch->integer)
	{
		int32_t key_offset = 0;
		player_stat_t stat = STAT_KEY_A;
		
		ent->client->ps.stats[STAT_KEY_A] = 
		ent->client->ps.stats[STAT_KEY_B] = 
		ent->client->ps.stats[STAT_KEY_C] = 0;

		// there's probably a way to do this in one pass but
		// I'm lazy
		std::array<item_id_t, IT_TOTAL> keys_held;
		size_t num_keys_held = 0;

		for (auto &item : itemlist)
		{
			if (!(item.flags & IF_KEY))
				continue;
			else if (!ent->client->pers.inventory[item.id])
				continue;

			keys_held[num_keys_held++] = item.id;
		}

		if (num_keys_held > 3)
			key_offset = (int32_t) (level.time.seconds() / 5);

		for (int32_t i = 0; i < min(num_keys_held, (size_t) 3); i++, stat = (player_stat_t) (stat + 1))
			ent->client->ps.stats[stat] = gi.imageindex(GetItemByIndex(keys_held[(i + key_offset) % num_keys_held])->icon);
	}

	//
	// frags
	//
	ent->client->ps.stats[STAT_FRAGS] = ent->client->resp.score;

	//
	// help icon / current weapon if not shown
	//
	if (ent->client->pers.helpchanged >= 1 && ent->client->pers.helpchanged <= 2 && (level.time.milliseconds() % 1000) < 500) // haleyjd: time-limited
		ent->client->ps.stats[STAT_HELPICON] = gi.imageindex("i_help");
	else if ((ent->client->pers.hand == CENTER_HANDED) && ent->client->pers.weapon)
		ent->client->ps.stats[STAT_HELPICON] = gi.imageindex(ent->client->pers.weapon->icon);
	else
		ent->client->ps.stats[STAT_HELPICON] = 0;

	ent->client->ps.stats[STAT_SPECTATOR] = 0;

	// set & run the health bar stuff
	for (size_t i = 0; i < MAX_HEALTH_BARS; i++)
	{
		byte *health_byte = reinterpret_cast<byte *>(&ent->client->ps.stats[STAT_HEALTH_BARS]) + i;

		if (!level.health_bar_entities[i])
			*health_byte = 0;
		else if (level.health_bar_entities[i]->timestamp)
		{
			if (level.health_bar_entities[i]->timestamp < level.time)
			{
				level.health_bar_entities[i] = nullptr;
				*health_byte = 0;
				continue;
			}

			*health_byte = 0b10000000;
		}
		else
		{
			// enemy dead
			if (!level.health_bar_entities[i]->enemy->inuse || level.health_bar_entities[i]->enemy->health <= 0)
			{
				// hack for Makron
				if (level.health_bar_entities[i]->enemy->monsterinfo.aiflags & AI_DOUBLE_TROUBLE)
				{
					*health_byte = 0b10000000;
					continue;
				}

				if (level.health_bar_entities[i]->delay)
				{
					level.health_bar_entities[i]->timestamp = level.time + gtime_t::from_sec(level.health_bar_entities[i]->delay);
					*health_byte = 0b10000000;
				}
				else
				{
					level.health_bar_entities[i] = nullptr;
					*health_byte = 0;
				}
				
				continue;
			}
			else if (level.health_bar_entities[i]->spawnflags.has(SPAWNFLAG_HEALTHBAR_PVS_ONLY) && !gi.inPVS(ent->s.origin, level.health_bar_entities[i]->enemy->s.origin, true))
			{
				*health_byte = 0;
				continue;
			}

			float health_remaining = ((float) level.health_bar_entities[i]->enemy->health) / level.health_bar_entities[i]->enemy->max_health;
			*health_byte = ((byte) (health_remaining * 0b01111111)) | 0b10000000;
		}
	}

	if (ra2->integer)
	{
		ent->client->ps.stats[STAT_RA2_SKIN_ICON] = ent->client->ps.stats[STAT_HEALTH_ICON];

		//
		// join queue
		//
		if (!ent->client->resp.context)
		{
			// back in the staging area: nothing here belongs to an arena any
			// more. Without this the countdown, the "Red vs Blue" line and the
			// queue counters of whichever arena this player last stood in
			// would sit on their HUD for the rest of the map. STAT_RA2_ROUNDINFO
			// needs no clearing of its own -- the statusbar only draws it
			// inside the STAT_RA2_COUNTDOWN gate.
			ent->client->ps.stats[STAT_RA2_COUNTDOWN] = 0;
			ent->client->ps.stats[STAT_RA2_ARENASTATUS] = 0;
			ent->client->ps.stats[STAT_RA2_SHOWQUEUE] = 0;
			ent->client->ps.stats[STAT_RA2_LINEPOSITION] = 0;
		}
		else if (arenas[ent->client->resp.context].idarena)
		{
			// a pickup arena's two standing sides, Red then Blue. Mid-round the
			// counters are how many of each side are still standing; between
			// rounds they are how many have signed up.
			const arena_t &arena = arenas[ent->client->resp.context];
			const bool live = arena.state == ASTATE_FIGHTING ||
							  arena.state == ASTATE_RESULTS ||
							  arena.state == ASTATE_NEXTROUND;

			for (int32_t side = 0; side < 2; side++)
			{
				const player_stat_t stat = side ? STAT_RA2_QUEUE2 : STAT_RA2_QUEUE1;
				team_t *team = arena.pickupteam[side];
				qmenu_t *slot = team ? (qmenu_t *) team->arenalink.it : nullptr;

				ent->client->ps.stats[stat] = (int16_t)
					(slot ? (live ? count_players_queue(slot) : count_queue(slot)) : 0);
			}

			// the " Red"/"Blue" labels init_player unicasts once per client
			ent->client->ps.stats[STAT_RA2_QUEUE1_NAME] = CONFIG_RA2_QUEUE1_NAME;
			ent->client->ps.stats[STAT_RA2_QUEUE2_NAME] = CONFIG_RA2_QUEUE2_NAME;
			ent->client->ps.stats[STAT_RA2_SHOWQUEUE] = 1;
		}
		else
		{
			ent->client->ps.stats[STAT_RA2_SHOWQUEUE] = 0;
		}

		RA2_SetIDView(ent);
	}
	else
	{
		// ZOID
		SetCTFStats(ent);
		// ZOID
	}
}

/*
===============
G_CheckChaseStats
===============
*/
void G_CheckChaseStats(edict_t *ent)
{
	gclient_t *cl;

	for (uint32_t i = 1; i <= game.maxclients; i++)
	{
		cl = g_edicts[i].client;
		if (!g_edicts[i].inuse || cl->chase_target != ent)
			continue;
		cl->ps.stats = ent->client->ps.stats;
		G_SetSpectatorStats(g_edicts + i);
	}
}

/*
===============
G_SetSpectatorStats
===============
*/
void G_SetSpectatorStats(edict_t *ent)
{
	gclient_t *cl = ent->client;

	if (!cl->chase_target)
		G_SetStats(ent);

	cl->ps.stats[STAT_SPECTATOR] = 1;

	// layouts are independant in spectator
	cl->ps.stats[STAT_LAYOUTS] = 0;
	// reachable under RA2: RA2 forces deathmatch and excludes ctf/teamplay, so
	// ClientUserinfoChanged honours a userinfo "spectator 1", spectator_respawn
	// sets resp.spectator, and PutClientInServer's RA2 arm returns before the
	// line that would clear it again. Without this arm such a client would have
	// its layout bit rebuilt the stock way and lose escape all over again.
	if (ra2->integer)
		cl->ps.stats[STAT_LAYOUTS] |= RA2_LayoutFlag(ent);
	else if (cl->pers.health <= 0 || level.intermissiontime || cl->showscores)
		cl->ps.stats[STAT_LAYOUTS] |= LAYOUTS_LAYOUT;
	if (cl->showinventory && cl->pers.health > 0)
		cl->ps.stats[STAT_LAYOUTS] |= LAYOUTS_INVENTORY;

	if (cl->chase_target && cl->chase_target->inuse)
		cl->ps.stats[STAT_CHASE] = CS_PLAYERSKINS +
								   (cl->chase_target - g_edicts) - 1;
	else
		cl->ps.stats[STAT_CHASE] = 0;
}
