// Copyright (c) ZeniMax Media Inc.
// Licensed under the GNU General Public License 2.0.

// rocketarena2/arena.h -- Rocket Arena 2 arena/team/round system.
//
// RA2 is a from-scratch deathmatch ruleset: the map is carved into
// independent "arenas" (self-contained little deathmatches, each running
// its own warmup/countdown/fight/results round loop), and players queue up
// in "teams" (which may be as small as a single player) that get matched
// against each other and dropped into a free arena to fight a set number
// of rounds. Everything here is gated behind `ra2->integer`, exactly like
// `ctf->integer` gates the CTF ruleset elsewhere in this codebase; RA2 and
// CTF are mutually exclusive at runtime (never both active at once), and
// this file's declarations are only ever exercised when ra2 is enabled.
//
// Per-round statistics reporting is intentionally NOT ported, in either of
// the donor's two forms: neither RA2's own GameSpy pipeline (NewGame/
// SendGameSnapShot over the bucket+hashtable+MD5-auth stack) nor the
// reconstruction's local JSON round log that replaced it (ra2stats.c's
// `statsfile` cvar and RA2_Stats_*, hung off an arena_t::stats handle). Only
// the local text log in gslog.cpp reports match activity here, its `netlog`
// UDP forwarding is dropped with the rest, and arena_t deliberately carries
// no stats handle.
#pragma once

//
// tunable limits
//
constexpr int32_t MAX_ARENAS = 32;
constexpr int32_t MAX_TEAMS = 256;
constexpr int32_t MAX_ARENA_SKINS = 7; // skin/color numbers 0-6 handed out per arena

extern cvar_t *ra2;			// ruleset toggle; mirrors ctf->integer
extern cvar_t *admincode;		// stateless password compared by Cmd_arenaadmin_f

//
// arena_t.state -- per-arena round state machine, driven by arena_think()
//
enum arena_state_t
{
	ASTATE_WARMUP,		// waiting for enough players/teams to fill the arena
	ASTATE_COUNTDOWN,	// "fight!" countdown running, show_countdown() ticking
	ASTATE_FIGHTING,	// round in progress
	ASTATE_ROUNDEND,	// waiting for enough teams to start a brand-new match
						// after the previous one finished
	ASTATE_RESULTS,		// fixed pause after a round ends before moving on
	ASTATE_NEXTROUND	// pulling in the next round's teams, announcing results
};

//
// gclient_t->resp.fightstate -- is this client alive and fighting this round?
//
enum fightstate_t
{
	FIGHT_SPECTATING,
	FIGHT_ALIVE,
	FIGHT_DEAD
};

//
// gclient_t->resp.omode -- observer/spectator camera mode, cycled by ChangeOMode()
//
enum observer_mode_t
{
	OBSERVER_NORMAL,		// actually playing, not observing
	OBSERVER_FREEFLYING,	// noclipping around with no fixed target
	OBSERVER_TRACKCAM,		// following a tracked player, vanilla-chasecam style
	OBSERVER_EYECAM			// floating in the eyes of a tracked player
};

//
// roster dimensions UpdateStatusBars gathers into before formatting: two
// teams of four, matching the frame the original reserved for them
//
constexpr int32_t MAX_STATUS_TEAMS = 2;
constexpr int32_t MAX_STATUS_MEMBERS = 4;

// the offhand grapple hook is NOT re-implemented here: it reuses
// ctf/g_ctf.h's CTFWeapon_Grapple/CTFPlayerResetGrapple/CTFGrapplePull/
// CTFResetGrapple/CTFFireGrapple/CTFGrappleFire directly (already
// generalized, not gated behind ctf->integer), along with MOD_GRAPPLE and
// ctfgrapplestate_t/CTF_DEFAULT_GRAPPLE_SPEED/CTF_DEFAULT_GRAPPLE_PULL_SPEED.
// gclient_t::ctf_grapple/ctf_grapplestate/ctf_grapplereleasetime (see
// g_local.h) are shared with CTF for the same reason -- RA2 and CTF never
// run at once. Only the +hook button state (gclient_t::hookbutton) is
// RA2's own.

//
// absolute path of the directory this game library was loaded from, which is the
// mod's gamedir as the engine actually resolved it -- the working directory is not
// a reliable base for mod files (see module_directory() in maploop.cpp). Empty if
// the OS wouldn't say, in which case callers fall back to the "game" cvar.
//
const std::string &ra2_gamedir();		// maploop.cpp

//
// message-of-the-day: motd.txt is read once per level load and shown to
// each client on their first spawn (see init_player()).
//
extern std::vector<std::string> motd_lines;
void load_motd();		// maploop.cpp; called once per map load from arena_init()
const char *get_next_map(const char *current);		// maploop.cpp

//
// a team of players sharing spawn points, a skin/color and a fate this round
//
struct team_t
{
	std::string name;

	int32_t teamnum = -1;	// this team's slot in the `teams` roster array
	int32_t arenanum = 0;	// which arena this team is queued/playing in

	int32_t wins = 0;		// rounds won so far this match

	// this team's own node on whichever arena_t::waitingteams/activeteams
	// list it currently belongs to (arenalink.it points back at
	// &teams[teamnum], see the TEAM() helper below).
	qmenu_t arenalink;

	bool locked = false;	// closed to new members
	int32_t side = -1;		// which spawn side (0/1) this team was assigned this round
	int32_t skin = -1;		// skin/color number (0..MAX_ARENA_SKINS-1)

	bool fighting = false;	// this team is currently in a live round
	bool outofline = false;	// this team is standing out of the round queue by choice
};

// each `teams[]` slot doubles as a qmenu_t sentinel: `.it` is the team_t*,
// and `.next`/`.prev` anchor that team's own member list (each member's
// node is `gclient_t.resp.teammember`, embedded in client_respawn_t -- see
// g_local.h). A pointer to a team's own slot reaches the team_t through
// one deref via TEAM().
[[nodiscard]] constexpr team_t *TEAM(qmenu_t *node) { return (team_t *) node->it; }

extern std::array<qmenu_t, MAX_TEAMS> teams;

//
// the votable settings for one arena. arena_t embeds one of these as its
// own live settings, and another as `proposed` -- a staged copy the
// propose/vote flow (menuShowSettingsPropose/Vote, Cmd_arenaadmin_f) holds
// separately until a vote passes, at which point `proposed` is copied back
// over the live settings.
//
struct arena_settings_t
{
	int32_t playersperteam = 1;
	int32_t rounds = 1;
	int32_t weapons = -1;		// bitmask, see weapon_vals[]
	int32_t armor = 0;			// 0 = map default, else forced starting armor
	int32_t health = 0;			// 0 = 100hp, else forced starting health
	int32_t minping = 0;
	int32_t maxping = 0;
	int32_t rocket_speed = 650;
	int32_t shells = 0, bullets = 0, slugs = 0, grenades = 0, rockets = 0, cells = 0;

	int32_t fastswitch = 1;
	int32_t armorprotect = 0;	// 0 = damage all, 1 = don't damage team, 2 = damage self not team
	int32_t healthprotect = 0;
	bool	fallingdamage = false;

	// arena.cfg-configurable toggles for whether each setting can be voted on
	// by clients. Cmd_arenaadmin_f gates its rows on these in propose mode
	// (mode 1); the admin's own mode 0 shows every row regardless, and
	// menuApplyArenaAdmin carries a locked weapon's current bit through
	// unchanged rather than dropping it.
	bool allow_voting_armor = true;
	bool allow_voting_health = true;
	bool allow_voting_minping = true;
	bool allow_voting_maxping = true;
	bool allow_voting_playersperteam = true;
	bool allow_voting_rounds = true;
	bool allow_voting_maxteams = true;
	bool allow_voting_armorprotect = true;
	bool allow_voting_healthprotect = true;
	bool allow_voting_shotgun = true;
	bool allow_voting_supershotgun = true;
	bool allow_voting_machinegun = true;
	bool allow_voting_chaingun = true;
	bool allow_voting_grenadelauncher = true;
	bool allow_voting_rocketlauncher = true;
	bool allow_voting_hyperblaster = true;
	bool allow_voting_railgun = true;
	bool allow_voting_bfg = true;
	bool allow_voting_fallingdamage = true;

	bool locked = false;
	bool competition = false;
	bool scorebydamage = false;
	bool changed = false;		// set when a vote alters the live settings; drives a reset-to-defaults check
};

//
// one arena: a self-contained little deathmatch running its own round loop
//
struct arena_t
{
	int32_t numteams = 2;			// teams needed for one round (always 2: red vs blue)

	qmenu_t waitingteams;			// teams queued, waiting to be assigned to this arena
	qmenu_t activeteams;			// teams currently playing in this arena

	arena_state_t state = ASTATE_WARMUP;
	bool	teamplay = false;		// true when playersperteam > 1 or this is a pickup (idarena) arena

	gtime_t countdown_next_tick;	// next time countdown ticks down by one
	int32_t countdown = 0;			// seconds remaining in WARMUP/COUNTDOWN, shown by show_countdown()

	bool	active = false;			// a round is actually in progress

	std::string msg;				// scratch round-end announcement text (built in NEXTROUND)
	std::string vs;					// "Red Team vs Blue Team" style fight description

	arena_settings_t settings;		// live settings
	gtime_t proposetime;			// when the current settings-change proposal expires, 0_ms = none
	arena_settings_t proposed;		// staged settings, pending a vote

	int32_t votetries = 0;			// eligible-voter count snapshotted when a poll started
	int32_t votes_yes = 0, votes_no = 0;
	edict_t *proposer = nullptr;

	bool	idarena = false;		// pickup arena: no team membership required, join solo
	int32_t sidepick = 0;			// this round's coin flip (which team spawns on which side)
	int32_t maxteams = 2;			// teams allowed to sign up for this arena

	int32_t round = 1;				// current round number, 1-based
	std::array<team_t *, 2> pickupteam = {}; // auto-created "Pickup Red"/"Pickup Blue" teams (idarena only)
};

extern std::array<arena_t, MAX_ARENAS> arenas;
extern int32_t num_arenas;
extern bool	   idmap;			// this map has no per-arena spawn point tagging; treat it as one big pickup arena

[[nodiscard]] inline bool RA2_IsPlayableArena(int32_t arenanum)
{
	return arenanum > 0 && arenanum <= num_arenas && arenanum < MAX_ARENAS;
}

extern int32_t votetries_setting;	// default max settings-proposals allowed per client per level
extern bool	   allow_grapple;		// arena.cfg-configured: is the grapple hook enabled on this map
extern bool	   broken;				// admin safety valve: freezes HUD/round updates server-wide

extern std::array<std::string, MAX_ARENA_SKINS> teamskins; // skin name per skin/color number, e.g. "r2red"
// the four player body models. Original RA2 data, kept for fidelity; the mod
// itself never reads it (setteamskin picks the model off the client's own skin).
extern std::array<std::string, 4>				 vwepmodels;
// NOTE: these store the precached *image index* for each team-skin icon
// (not a true bool) so GetSkinIcon() can identify a player's current skin
// image by equality comparison against these values -- matches RA2's own
// (mis-named) `qboolean teamskins_precache*[]` arrays, which are really
// plain ints since qboolean is just int in the original headers.
extern std::array<int32_t, MAX_ARENA_SKINS>	 teamskins_precachem;
extern std::array<int32_t, MAX_ARENA_SKINS>	 teamskins_precachef;
extern std::array<int32_t, MAX_ARENA_SKINS>	 teamskins_precachecw;
extern std::array<int32_t, MAX_ARENA_SKINS>	 teamskins_precachecb;
extern int32_t genericicon;

extern std::array<const char *, 4> omode_descriptions;

// bit value per weapon slot for arena_settings_t::weapons, indexed
// 0=shotgun, 1=supershotgun, 2=machinegun, 3=chaingun, 4=grenadelauncher,
// 5=rocketlauncher, 6=hyperblaster, 7=railgun, 8=bfg (maploop.cpp)
extern std::array<int32_t, 9> weapon_vals;

//
// arena.cpp -- team/queue bookkeeping shared with ra2_menu.cpp/maploop.cpp
//
int32_t count_players_queue(qmenu_t *head); // like count_queue, but only nodes whose edict_t payload is FIGHT_ALIVE
int32_t GetSkinIcon(edict_t *ent); // current player's team-skin icon, or genericicon if not a precached arena skin

void	set_damage(int32_t arenanum, int32_t state);
void	give_ammo(edict_t *ent);

team_t *add_to_team(edict_t *ent, const char *teamname);
void	remove_from_team(edict_t *ent);

// creates a team named "<player>'s Team" (uniquified with trailing '!'s) and
// puts `ent` on it, queued for arena 0. Shared by the "Start New Team" menu
// item and by the bot auto-join below, which needs the same team out of code.
team_t *create_own_team(edict_t *ent);

edict_t *SelectRandomArenaSpawnPoint(const char *classn, int32_t arenanum, int32_t side);
// `ignore` is the client being placed, left out of the distance measure so it
// is not pushed away from the spot it is standing on right now; null when the
// caller is not placing anybody (see arena_init).
edict_t *SelectFarthestArenaSpawnPoint(const char *classn, int32_t arenanum, edict_t *ignore);

void track_SetStats(edict_t *ent);
void eyecam_think(edict_t *ent, usercmd_t *ucmd);
void track_think(edict_t *ent, usercmd_t *ucmd);
void track_change(edict_t *ent, int32_t dir);
void track_next(edict_t *ent);
void track_prev(edict_t *ent);
void SetObserverMode(edict_t *ent);

void move_to_arena(edict_t *ent, int32_t arenanum, int32_t mode);
void ChangeOMode(edict_t *ent);

int32_t		getfreeskin(int32_t arenanum);
// the name this player actually set. The rerelease replaces a human client's
// pers.netname with a "##P<n>" token for the client to decode (see
// G_EncodedPlayerName) -- only localized prints and a few configstrings expand
// it, so anywhere RA2 bakes a name into a raw svc_layout string, a menu, a team
// name or its own log, the token would show through literally. Bots are never
// tokenised, which is why only the humans read as "##P0".
[[nodiscard]] std::string RA2_PlayerName(const edict_t *ent);

void		setteamskin(edict_t *ent, char *userinfo, int32_t skinnum);

void	SendTeamToArena(qmenu_t *team, int32_t arenanum, bool observer, bool announce);
int32_t AddtoArena(edict_t *ent, int32_t arenanum, int32_t allow_partial, bool skip_checks);
void	check_teams(int32_t arenanum);

void init_player(edict_t *ent);
void reinit_player(edict_t *ent);

//
// bot integration.
//
// RA2's join flow is menus and nothing else -- ClientCommand dispatches no
// join/team/observe command at all (see g_cmds.cpp) -- so an engine bot, which
// only ever sends movement and buttons, can never leave the arena-0 observer
// state init_player() drops it in, and a server full of bots stays stuck in
// warmup. CTF hits the same wall and gets around it by keeping bots out of the
// menu path entirely: CTFStartClient() skips the observer/join-menu branch for
// SVF_BOT clients, and CTFAssignTeam() hands the bot the smaller team itself
// (ctf/g_ctf.cpp). Same two halves here, plus the extra step RA2 needs -- a
// team is a queue object that also has to be signed up for an arena before a
// round will pull it in.
//
[[nodiscard]] bool RA2_IsBot(const edict_t *ent);

// seats every bot that still has no team, and migrates idle bots to whichever
// arena the humans went to. Called once per frame from multi_arena_think(),
// self-throttled to one pass a second.
void RA2_BotsJoinArenas();

// this client's side of its arena's live round as a 1-indexed team number,
// which is the shape the engine's bot AI expects from sv_entity_t::team (CTF
// feeds it 1/2 the same way, via the skinnum team_index; 0 means "no team").
// RA2's own resp.teamnum runs to MAX_TEAMS and means nothing outside its own
// arena, so it is not usable as-is. Team_None for anyone not fighting.
[[nodiscard]] int32_t RA2_TeamIndexForBots(const edict_t *ent);

void show_stringc(const char *s, int32_t context);
void show_string(int32_t priority, const char *s, int32_t context);
void stuffcmd(edict_t *ent, const char *s);
void send_sound_to_arena(const char *soundname, int32_t context);
void send_configstring(edict_t *e, int32_t index, const char *string);
void show_countdown(int32_t countdown, int32_t arenanum);
int32_t show_rank(qmenu_t *node);

bool	check_for_teams(int32_t arenanum);
int32_t fill_arena(int32_t arenanum);
int32_t fight_done(int32_t arenanum);

void RA2_SetIDView(edict_t *ent);
void UpdateStatusBars(int32_t arenanum);
void check_telefrag(int32_t arenanum);

void start_voting(edict_t *proposer, int32_t arenanum);
void check_voting(int32_t arenanum);

void arena_think(int32_t arenanum);
void	arena_shutdown();
void	multi_arena_think();
void arena_init(edict_t *wsent);

//
// arena-aware teleporter touch. This REPLACES the touch callback that the
// stock (or CTF) trigger_teleport/misc_teleporter's constructed trigger
// use when ra2->integer -- SP_trigger_teleport/SP_func_illusionary/
// SP_info_teleport_destination themselves are NOT overridden (RA2's own
// versions are functionally identical no-ops/plain setup, so the existing
// stock/CTF spawn functions are reused as-is; see g_spawn.cpp).
// When self->arena > 0, touching the trigger calls AddtoArena to join that
// arena instead of moving the toucher to a fixed destination.
// NOTE: plain declaration only -- the TOUCH() macro (used at the definition
// in arena.cpp) registers a static savegame pointer record; using it here
// too would re-register it once per translation unit that includes this
// header, corrupting InitSave()'s dedup check (fatal with g_strict_saves 1).
void ra2_teleporter_touch(edict_t *self, edict_t *other, const trace_t &tr, bool other_touching_self);

//
// ra2menus.cpp
//
const char *getarenaname(int32_t arenanum);
void		menu_centerprint(edict_t *ent, const char *message);
int32_t		menuRefreshTeamList(edict_t *ent, qmenu_t *menu, qmenu_t *item, int32_t arg);
void		motd_menu(edict_t *ent);

//
// gslog.cpp -- the local StdLog text logger. Kept: this is a plain local
// text file of match activity, unrelated to the excluded GameSpy
// stats-reporting pipeline (MD5-auth/bucket/hashtable). The donor's `netlog`
// UDP forwarding is not ported either; see gslog.cpp for why.
//
void GSLogStartup();
void GSLogShutdown();
void GSLogNewmap();
void GSLogEnter(edict_t *ent);
void GSLogExit(edict_t *ent);
void GSLogDeath(edict_t *self, edict_t *inflictor, edict_t *attacker);
