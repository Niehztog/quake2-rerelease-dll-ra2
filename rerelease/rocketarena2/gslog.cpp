// Copyright (c) ZeniMax Media Inc.
// Licensed under the GNU General Public License 2.0.

// rocketarena2/gslog.cpp -- RA2's local StdLog text logger.
//
// StdLog is not an id format: it is Mark Davies' "Standard Logging" library
// (1998-99, Artistic License), which the competition mods of the era dropped
// in as `sl_*.c` -- OSP Tourney and Freeze Tag carry the same writer. RA2's
// copy is reproduced here, version string included, so existing parsers still
// read the output.
//
// The donor also forwarded every kill line to a collector as plaintext UDP,
// via a `netlog host:port` cvar. That is deliberately NOT ported. 1999's
// shipped server.cfg enabled it by default and pointed it at the author's own
// `ripper.planetquake.com:21998`; that domain is parked today and resolves to
// an unrelated host, so a stock config would have reported player names, kills
// and pings to a stranger on every frag. Dropping it also removes a blocking
// `getaddrinfo` from the death path, and leaves the port with no network code
// at all. The `netlog` and `public` cvars are therefore not registered here --
// see the release workflow, which comments the line out of the shipped cfg.

#include "../g_local.h"
#include "arena.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace
{

constexpr std::string_view GSLOG_VERSION = "1.22";
constexpr std::string_view GSLOG_PATCH_NAME = "Rocket Arena 2 v2.25";

cvar_t *gslog_logfile;
cvar_t *gslog_logname;
cvar_t *gslog_game;

void GSLogEnsureCvars()
{
	// "logfile" is the engine's own console-logging cvar; RA2 piggybacks on it
	// and treats the value 2 as "also write the StdLog". Kept as the donor had
	// it so existing server configs behave the same way.
	if (!gslog_logfile)
		gslog_logfile = gi.cvar("logfile", "0", CVAR_SERVERINFO);

	if (!gslog_logname)
		gslog_logname = gi.cvar("logname", "stdlog.log", CVAR_NOFLAGS);

	if (!gslog_game)
		gslog_game = gi.cvar("game", ".", CVAR_LATCH);
}

[[nodiscard]] bool GSLogEnabled()
{
	GSLogEnsureCvars();
	return gslog_logfile && gslog_logfile->integer == 2;
}

[[nodiscard]] int32_t GSLogTimeSeconds()
{
	return level.time.seconds<int32_t>();
}

[[nodiscard]] std::filesystem::path GSLogPath()
{
	GSLogEnsureCvars();

	const char *game_dir = (gslog_game && gslog_game->string && gslog_game->string[0]) ? gslog_game->string : ".";
	const char *log_name = (gslog_logname && gslog_logname->string && gslog_logname->string[0]) ? gslog_logname->string : "stdlog.log";

	// the game library's own directory is the mod's gamedir; the working directory
	// need not contain it, and on the remaster is typically not even writable
	if (const std::string &dir = ra2_gamedir(); !dir.empty())
		return std::filesystem::path(dir) / log_name;

	return std::filesystem::path(game_dir) / log_name;
}

bool GSLogWriteLocal(std::string_view text)
{
	const std::filesystem::path path = GSLogPath();
	std::ofstream file(path, std::ios::out | std::ios::app);
	if (!file)
	{
		gi.Com_PrintFmt("GSLog: couldn't open {}\n", path.string());
		return false;
	}

	file.write(text.data(), static_cast<std::streamsize>(text.size()));
	if (!file)
	{
		gi.Com_PrintFmt("GSLog: couldn't write {}\n", path.string());
		return false;
	}

	return true;
}

[[nodiscard]] std::string GSLogNetname(const edict_t *ent)
{
	if (!ent || !ent->client || !ent->client->pers.netname[0])
		return "<unknown>";

	// a log the server keeps wants the name, not the client-side token
	return RA2_PlayerName(ent);
}

[[nodiscard]] int32_t GSLogPing(const edict_t *ent)
{
	return (ent && ent->client) ? ent->client->ping : 0;
}

[[nodiscard]] const char *GSLogWeaponName(const gitem_t *weapon)
{
	return (weapon && weapon->use_name && weapon->use_name[0]) ? weapon->use_name : "BFG10K";
}

[[nodiscard]] bool GSLogWeaponGetsNamedSuicide(const gitem_t *weapon)
{
	if (!weapon || !weapon->classname)
		return false;

	return std::strcmp(weapon->classname, "weapon_grenadelauncher") == 0 ||
		   std::strcmp(weapon->classname, "weapon_rocketlauncher") == 0 ||
		   std::strcmp(weapon->classname, "weapon_bfg") == 0;
}

} // namespace

void GSLogStartup()
{
	GSLogEnsureCvars();
	if (!GSLogEnabled())
		return;

	std::ostringstream record;
	record << "\t\tStdLog\t" << GSLOG_VERSION << '\n';
	record << "\t\tPatchName\t" << GSLOG_PATCH_NAME << '\n';
	GSLogWriteLocal(record.str());
}

void GSLogShutdown()
{
	GSLogEnsureCvars();
	if (!GSLogEnabled())
		return;

	std::ostringstream record;
	record << "\t\tGameEnd\t\t\t" << GSLogTimeSeconds() << '\n';
	GSLogWriteLocal(record.str());
}

void GSLogNewmap()
{
	if (!GSLogEnabled())
		return;

	std::ostringstream record;
	record << "\t\tMAP\t" << level.level_name << '\n';
	record << "\t\tGameStart\t\t\t" << GSLogTimeSeconds() << '\n';
	GSLogWriteLocal(record.str());
}

void GSLogEnter(edict_t *ent)
{
	if (!GSLogEnabled() || !ent || !ent->client)
		return;

	std::ostringstream record;
	record << "\t\tPlayerConnect\t" << GSLogNetname(ent) << "\t\t" << GSLogTimeSeconds() << '\n';
	GSLogWriteLocal(record.str());
}

void GSLogExit(edict_t *ent)
{
	if (!GSLogEnabled() || !ent || !ent->client)
		return;

	std::ostringstream record;
	record << "\t\tPlayerLeft\t" << GSLogNetname(ent) << "\t\t" << GSLogTimeSeconds() << '\n';
	GSLogWriteLocal(record.str());
}

void GSLogDeath(edict_t *self, edict_t *inflictor, edict_t *attacker)
{
	(void) inflictor;

	if (!GSLogEnabled() || !self || !self->client)
		return;

	std::ostringstream record;

	if (attacker == self)
	{
		const gitem_t *weapon = self->client->pers.weapon;
		if (GSLogWeaponGetsNamedSuicide(weapon))
		{
			record << GSLogNetname(self) << "\t\tSuicide\t" << GSLogWeaponName(weapon) << "\t-1\t"
				   << GSLogTimeSeconds() << '\t' << GSLogPing(self) << '\n';
		}
		else
		{
			record << GSLogNetname(self) << "\t\tSuicide\t\t-1\t" << GSLogTimeSeconds()
				   << '\t' << GSLogPing(self) << '\n';
		}

		GSLogWriteLocal(record.str());
		return;
	}

	if (attacker && attacker->client)
	{
		record << GSLogNetname(attacker) << '\t' << GSLogNetname(self) << "\tKill\t"
			   << GSLogWeaponName(attacker->client->pers.weapon) << "\t1\t"
			   << GSLogTimeSeconds() << '\t' << GSLogPing(attacker) << '\n';
		GSLogWriteLocal(record.str());
		return;
	}

	record << GSLogNetname(self) << "\t\tSuicide\t\t-1\t" << GSLogTimeSeconds()
		   << '\t' << GSLogPing(self) << '\n';
	GSLogWriteLocal(record.str());
}
