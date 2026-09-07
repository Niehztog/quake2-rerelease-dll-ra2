// Copyright (c) ZeniMax Media Inc.
// Licensed under the GNU General Public License 2.0.

// rocketarena2/ra2_menu.h -- generic in-game menu engine for Rocket Arena 2,
// built on the stock statusbar/layout program language (see
// gi.WriteByte(svc_layout)/CS_STATUSBAR usage in ra2_menu.cpp). Content-free:
// everything here is reusable by any RA2 menu screen (see ra2menus.cpp for
// the actual menu content).
//
// This is a deliberately separate, simpler engine from ctf/p_ctf_menu.h's
// PMenu: RA2's menus need a stack of nested screens (menuqueue), scrollable
// "(More)" paging, non-selectable label items, and a 3-way select-callback
// return convention, none of which PMenu provides.
#pragma once

// Historical soft limit on a statusbar/layout program's length (RA2's
// original fixed buffer size); gclient_t.menutext is a std::string now and
// isn't hard-capped, but DisplayMenu/UpdateStatusBars should still treat
// this as the practical budget the classic engine layout parser is
// comfortable with.
constexpr size_t MAXSTATUSBAR = 1400;
constexpr size_t MAXMENUTEXT = MAXSTATUSBAR;

// a qmenu_t is a generic intrusive queue node. The same shape is reused for
// several different things: the per-client stack of pending/active menus
// (gclient_t's embedded `menuqueue` sentinel, see g_local.h), the list of
// items hanging off a menu (menuinfo_t reuses this exact layout for itself,
// see below), team membership/arena-assignment queues (see arena.h), and
// individual item/menu wrapper nodes whose `it` points at the real payload.
struct qmenu_t
{
	void	*it = nullptr;
	qmenu_t *next = nullptr;
	qmenu_t *prev = nullptr;
};

// a select callback runs when an item is chosen with invuse (see UseMenu).
// Return 0 to close the menu, 1 to leave it up and just redraw it (e.g.
// after changing a value in place), or 2 if the callback already took care
// of redrawing/replacing the menu itself (e.g. it pushed a new screen).
enum menuselect_result_t
{
	MSELECT_CLOSE,
	MSELECT_REDRAW,
	MSELECT_HANDLED
};

using menuselect_t = int32_t (*)(edict_t *ent, qmenu_t *menu, qmenu_t *item, int32_t arg);

// the payload of an item node (item->it). value and the trailing number are
// both optional (value is nullptr, num is negative) and are appended after
// text when the item is drawn. An item with no select callback is a plain,
// non-selectable label -- MenuNext/MenuPrev skip over it.
struct menuitem_t
{
	std::string	 text;
	std::string	 value;
	int32_t		 num = -1;
	menuselect_t select = nullptr;
};

// the payload of a menu node (menu->it, i.e. what CreateQMenu allocates).
struct menuinfo_t
{
	std::string title;
	qmenu_t	   *items = nullptr;
	int32_t		flags = 0;
	uint64_t	 instance_id = 0;
};

// menuinfo_t::flags bits
enum menuinfo_flags_t
{
	MENU_NONE	  = 0,
	MENU_NOABORT  = bit_v<0>, // can't be cancelled with a bare invuse/putaway
};
MAKE_ENUM_BITFLAGS(menuinfo_flags_t);

// a team's teams[] slot carries the team_t itself in its `it` field (see
// arena.h); a menu node's `it` carries a menuinfo_t*; an item node's `it`
// carries a menuitem_t*.
[[nodiscard]] constexpr menuinfo_t *MENU_INFO(qmenu_t *node) { return (menuinfo_t *) node->it; }
[[nodiscard]] constexpr menuitem_t *MENUITEM(qmenu_t *node) { return (menuitem_t *) node->it; }

//
// arena.cpp/ra2_menu.cpp -- the generic intrusive-queue primitives, shared
// between ra2_menu.cpp's per-client menu/item lists and arena.cpp's own
// team/arena-membership queues (arena_t's waitingteams/activeteams, team_t's
// own arenalink, gclient_t's embedded teammember node).
//
void	 add_to_queue(qmenu_t *node, qmenu_t *head);
// unlinks node from whatever list it currently lives on; returns node.
qmenu_t *remove_from_queue(qmenu_t *node, qmenu_t *head);
// unlinks node from wherever it lives, then splices it onto the front of head's list.
void	 add_to_front_queue(qmenu_t *node, qmenu_t *head);
// DisplayMenu uses count_queue to page the item list.
int32_t	 count_queue(qmenu_t *head);

//
// ra2_menu.cpp
//
// A statusbar/layout program is not escaped by the parser, so a title, a team
// name or a player name carrying a quote or a backslash would end the string
// early; newlines have no meaning inside one either. Anything that formats
// user-supplied text into a layout goes through here (see DisplayMenu and
// arena.cpp's UpdateStatusBars).
[[nodiscard]] std::string layout_escape(std::string_view text);

void	 SendMenu(edict_t *ent);
void	 SendStatusBar(edict_t *ent, const char *string, bool transmit);
// is a menu both loaded and on screen? Callers outside the menu engine want
// this rather than curmenulink: a hidden menu must not swallow the item keys.
[[nodiscard]] bool MenuShown(const edict_t *ent);
// the screen on top is the only one on this client's stack, so there is nothing
// to back out to. Cmd_PutAway_f hides such a screen rather than popping it.
[[nodiscard]] bool MenuIsBase(const edict_t *ent);
// which STAT_LAYOUTS bit this client's layout content has earned -- the one rule
// for all three things RA2 multiplexes onto the single layout channel.
[[nodiscard]] layout_flags_t RA2_LayoutFlag(const edict_t *ent);

void	 DisplayMenu(edict_t *ent);
qmenu_t *CreateQMenu(edict_t *ent, const char *title, menuinfo_flags_t flags = MENU_NONE);
qmenu_t *AddMenuItem(qmenu_t *menu, const char *text, const char *value, int32_t num, menuselect_t select);
// pushes `menu` onto ent's menu stack and displays it (or just refreshes
// the top of stack, if show is false and a menu is already showing).
void	 FinishMenu(edict_t *ent, qmenu_t *menu, bool show);
void	 MenuNext(edict_t *ent);
void	 MenuPrev(edict_t *ent);
// activates the currently selected item of the top-of-stack menu. `arg` is
// handed straight to the item's select callback and is what tells the
// value-editing rows which way to step: invuse (ENTER) passes 1, invdrop
// passes 0, so one binding raises a setting and the other lowers it. It also
// picks menuAddtoArena's two ways into an arena apart -- see ra2menus.cpp.
void	 UseMenu(edict_t *ent, int32_t arg);
// drops the top-of-stack screen and shows the one under it, or closes the menu
// entirely if this was the last one and it isn't MENU_NOABORT. Reached from a
// select callback returning MSELECT_CLOSE ("Cancel"/"No"/"Continue" rows) and
// from menu_centerprint replacing a message screen -- not from a key of its
// own: invdrop is a second select key under RA2, see UseMenu.
void	 PopMenu(edict_t *ent);
// ticks any per-frame menu bookkeeping (e.g. auto-refreshing status text).
bool	 MenuThink(edict_t *ent);
// frees this client's whole menu stack and forgets it. clear_menus() is this
// plus putting the screen back, and is what an in-level caller wants; teardown
// paths that run once the level is already gone (SpawnEntities, ShutdownGame)
// take this one, which touches neither the layout nor the network.
void	 free_client_menus(gclient_t *client);
void	 clear_menus(edict_t *ent);
