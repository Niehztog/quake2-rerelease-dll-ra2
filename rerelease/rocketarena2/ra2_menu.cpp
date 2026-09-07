// Copyright (c) ZeniMax Media Inc.
// Licensed under the GNU General Public License 2.0.

// rocketarena2/ra2_menu.cpp -- generic Rocket Arena 2 menu engine.

#include "../g_local.h"
#include "ra2_menu.h"

#include <algorithm>
#include <utility>

void DeathmatchScoreboard(edict_t *ent); // p_hud.cpp -- see hide_menu()

namespace
{
constexpr int32_t MAXMENUITEMS = 18;
constexpr gtime_t MENU_USE_COOLDOWN = 500_ms;
constexpr gtime_t MENU_RESEND_TIME = 1_sec;
uint64_t next_menu_instance_id = 0;

// a menu is loaded on this client's stack, shown or not
[[nodiscard]] bool menu_open(const edict_t *ent)
{
	return ent && ent->client && ent->client->curmenulink;
}

// ...and it is the thing currently on screen. RA2 kept these apart -- the
// observer menu is loaded hidden (FinishMenu(m, false)) and TAB toggles it --
// so everything that draws or takes input tests this one.
[[nodiscard]] bool menu_visible(const edict_t *ent)
{
	return menu_open(ent) && ent->client->showmenu;
}

[[nodiscard]] qmenu_t *first_menu_item(qmenu_t *menu)
{
	return menu ? MENU_INFO(menu)->items : nullptr;
}

[[nodiscard]] qmenu_t *last_menu_item(qmenu_t *menu)
{
	qmenu_t *node = first_menu_item(menu);
	if (!node)
		return nullptr;

	while (node->next)
		node = node->next;

	return node;
}

[[nodiscard]] qmenu_t *first_selectable(qmenu_t *menu)
{
	for (qmenu_t *node = first_menu_item(menu); node; node = node->next)
		if (MENUITEM(node)->select)
			return node;

	return first_menu_item(menu);
}

[[nodiscard]] qmenu_t *last_selectable(qmenu_t *menu)
{
	for (qmenu_t *node = last_menu_item(menu); node; node = node->prev)
		if (MENUITEM(node)->select)
			return node;

	return last_menu_item(menu);
}

[[nodiscard]] int32_t count_menu_items(qmenu_t *menu)
{
	int32_t count = 0;
	for (qmenu_t *node = first_menu_item(menu); node; node = node->next)
		++count;
	return count;
}

[[nodiscard]] int32_t menu_item_index(qmenu_t *menu, qmenu_t *item)
{
	int32_t index = 0;
	for (qmenu_t *node = first_menu_item(menu); node; node = node->next, ++index)
		if (node == item)
			return index;
	return 0;
}

template<typename T, typename... Args>
[[nodiscard]] T *tag_new(Args &&...args)
{
	void *mem = gi.TagMalloc(sizeof(T), TAG_LEVEL);
	return new(mem) T(std::forward<Args>(args)...);
}

template<typename T>
void tag_delete(T *ptr)
{
	if (!ptr)
		return;
	ptr->~T();
	gi.TagFree(ptr);
}

void free_menu(qmenu_t *menu)
{
	if (!menu)
		return;

	menuinfo_t *info = MENU_INFO(menu);
	for (qmenu_t *node = info->items; node; )
	{
		qmenu_t *next = node->next;
		tag_delete(MENUITEM(node));
		tag_delete(node);
		node = next;
	}

	tag_delete(info);
	tag_delete(menu);
}

// Every change of the current menu goes through here so menugen stays in step
// with curmenulink; comparing the pointer alone is not enough, because a freed
// menu's address can be recycled by the very next tag_new().
void set_current_menu(edict_t *ent, qmenu_t *menu)
{
	ent->client->curmenulink = menu;
	ent->client->menugen++;
}

// takes the menu off the screen without touching the stack, and hands the one
// layout channel the rerelease has back to the scoreboard if one was up under
// it. The original needed no such arbitration: its menus lived on the statusbar
// configstring and the scoreboard on svc_layout, so either could be up alone.
// The channel is still one here, but which of the two escape may act on is now
// spelled out per client -- see RA2_LayoutFlag.
void hide_menu(edict_t *ent)
{
	SendStatusBar(ent, "", true);

	ent->client->showscores = ent->client->scoremode != 0;
	if (ent->client->showscores)
		DeathmatchScoreboard(ent);

	ent->client->ps.stats[STAT_LAYOUTS] &= ~(LAYOUTS_LAYOUT | LAYOUTS_RA2_NO_PUTAWAY);
	ent->client->ps.stats[STAT_LAYOUTS] |= RA2_LayoutFlag(ent);
}

void show_no_menu(edict_t *ent)
{
	set_current_menu(ent, nullptr);
	ent->client->selected = nullptr;
	ent->client->showmenu = false;
	hide_menu(ent);
}

[[nodiscard]] qmenu_t *queue_tail(qmenu_t *head)
{
	qmenu_t *node = head;
	while (node && node->next)
		node = node->next;
	return node;
}

[[nodiscard]] qmenu_t *find_menu_instance(gclient_t *client, qmenu_t *address, uint64_t instance_id)
{
	for (qmenu_t *node = client->menuqueue.next; node; node = node->next)
		if (node == address && MENU_INFO(node)->instance_id == instance_id)
			return node;

	return nullptr;
}

} // namespace

std::string layout_escape(std::string_view text)
{
	std::string out;
	out.reserve(text.size() + 8);

	for (char ch : text)
	{
		if (ch == '\\' || ch == '"')
			out.push_back('\\');
		if (ch == '\n' || ch == '\r')
			out.push_back(' ');
		else
			out.push_back(ch);
	}

	return out;
}

void add_to_queue(qmenu_t *node, qmenu_t *head)
{
	if (!node || !head)
		return;

	for (; head->next; head = head->next)
		;

	head->next = node;
	node->prev = head;
	node->next = nullptr;
}

qmenu_t *remove_from_queue(qmenu_t *node, qmenu_t *head)
{
	if (!node)
	{
		if (!head || !head->next)
			return nullptr;
		node = head->next;
	}

	if (node->prev)
		node->prev->next = node->next;
	if (node->next)
		node->next->prev = node->prev;

	node->prev = nullptr;
	node->next = nullptr;
	return node;
}

void add_to_front_queue(qmenu_t *node, qmenu_t *head)
{
	if (!node || !head)
		return;

	remove_from_queue(node, nullptr);

	node->prev = head;
	node->next = head->next;
	if (head->next)
		head->next->prev = node;
	head->next = node;
}

int32_t count_queue(qmenu_t *head)
{
	int32_t count = 0;
	if (!head)
		return 0;

	for (qmenu_t *node = head->next; node; node = node->next)
		++count;

	return count;
}

void SendMenu(edict_t *ent)
{
	if (!ent || !ent->client)
		return;

	gi.WriteByte(svc_layout);
	gi.WriteString(ent->client->menutext.c_str());
	gi.unicast(ent, true);
}

void SendStatusBar(edict_t *ent, const char *string, bool transmit)
{
	if (!ent || !ent->client)
		return;

	const std::string next = string ? string : "";
	const bool changed = ent->client->menutext != next;
	ent->client->menutext = next;
	ent->client->menutime = level.time + MENU_RESEND_TIME;

	if (transmit || changed)
		SendMenu(ent);
}

bool MenuShown(const edict_t *ent)
{
	return menu_visible(ent);
}

bool MenuIsBase(const edict_t *ent)
{
	if (!menu_open(ent))
		return false;

	// menuqueue is an embedded sentinel head and add_to_queue appends, so the
	// only screen on the stack is the one whose prev is the head itself. This
	// is the same pair of terms PopMenu guards MENU_NOABORT with.
	qmenu_t *current = ent->client->curmenulink;
	return queue_tail(&ent->client->menuqueue) == current &&
		current->prev == &ent->client->menuqueue;
}

// RA2 multiplexes one layout channel three ways, and only one of the three is
// something escape should be routed at: a visible menu, which Cmd_PutAway_f
// backs out of. A board is RA2's own and is dismissed through the score cycle
// (Cmd_Score_f), and the audience bar is furniture the original drew on
// CS_STATUSBAR, a channel that carried no putaway meaning at all. Both of those
// take the private bit instead, which the cgame draws and hides from the engine
// -- otherwise the engine remaps escape to putaway (see layout_flags_t) and the
// ingame menu becomes unreachable for as long as they are up.
layout_flags_t RA2_LayoutFlag(const edict_t *ent)
{
	if (!ent || !ent->client || !ent->client->showscores)
		return (layout_flags_t) 0;

	return menu_visible(ent) ? LAYOUTS_LAYOUT : LAYOUTS_RA2_NO_PUTAWAY;
}

void DisplayMenu(edict_t *ent)
{
	if (!ent || !ent->client)
		return;

	if (!menu_visible(ent))
	{
		// a loaded-but-hidden menu keeps its stack; only a client with no menu
		// at all gets torn down
		if (menu_open(ent))
			hide_menu(ent);
		else
			show_no_menu(ent);
		return;
	}

	gclient_t *cl = ent->client;
	qmenu_t *menu = cl->curmenulink;
	menuinfo_t *info = MENU_INFO(menu);
	qmenu_t *selected = cl->selected ? cl->selected : first_selectable(menu);
	cl->selected = selected;

	std::string layout = "xv 32 yv 8 picn inventory ";
	layout += "xv 202 yv 12 string2 \"Menu\" ";
	layout += G_Fmt("xv 0 yv 24 cstring2 \"{}\" ", layout_escape(info->title));

	const int32_t total_items = count_menu_items(menu);
	const int32_t selected_index = selected ? menu_item_index(menu, selected) : 0;
	const int32_t page_start = (selected_index / MAXMENUITEMS) * MAXMENUITEMS;
	const bool has_top_more = page_start > 0;
	const bool has_bottom_more = total_items > (page_start + MAXMENUITEMS);

	int32_t y = 32;
	if (has_top_more)
		layout += "xv 50 yv 32 string2 \"(More)\" ";
	else
		layout += "xv 50 ";

	int32_t index = 0;
	int32_t shown = 0;
	for (qmenu_t *node = info->items; node; node = node->next, ++index)
	{
		if (index < page_start)
			continue;
		if (shown >= MAXMENUITEMS)
			break;

		y += 8;
		++shown;

		menuitem_t *row = MENUITEM(node);
		const bool is_selected = (node == selected);
		std::string entry;
		entry.reserve(row->text.size() + row->value.size() + 16);
		// "> " marks the selection: the original's '\r' cursor glyph never drew,
		// and layout_escape rewrites it to a plain space anyway.
		entry += is_selected ? "> " : "  ";
		entry += row->text;
		entry += row->value;
		if (row->num >= 0)
			entry += G_Fmt("{}", row->num);

		const std::string escaped = layout_escape(entry);
		if (layout.size() + escaped.size() + 50 >= MAXSTATUSBAR)
			break;

		// The selected row draws in the alternate charset and the rest plain -- the
		// distinction hi_print() used to make by shifting each byte up by 0x80, which
		// cancelled string2's own 0x80 XOR and so rendered the shifted (unselected)
		// rows plain. That trick cannot work on Kex, which renders strings as UTF-8:
		// a shifted byte lands in 0x80-0xff, where it reads as a multi-byte lead byte
		// and eats the characters after it ('C' + 0x80 == 0xc3 swallowed the next byte
		// and rendered as a single "ï"). Ask the layout for the colour instead.
		layout += G_Fmt("yv {} {} \"{}\" ", y, is_selected ? "string2" : "string", escaped);
	}

	if (has_bottom_more && shown == MAXMENUITEMS)
	{
		y += 10;
		layout += G_Fmt("yv {} string2 \"(More)\" ", y);
	}

	cl->showscores = true;
	cl->ps.stats[STAT_LAYOUTS] &= ~(LAYOUTS_LAYOUT | LAYOUTS_RA2_NO_PUTAWAY);
	cl->ps.stats[STAT_LAYOUTS] |= RA2_LayoutFlag(ent);
	SendStatusBar(ent, layout.c_str(), false);
}

qmenu_t *CreateQMenu(edict_t *ent, const char *title, menuinfo_flags_t flags)
{
	(void) ent;
	qmenu_t *menu = tag_new<qmenu_t>();
	menuinfo_t *info = tag_new<menuinfo_t>();
	info->title = title ? title : "";
	info->flags = flags;
	info->instance_id = ++next_menu_instance_id;
	menu->it = info;
	return menu;
}

qmenu_t *AddMenuItem(qmenu_t *menu, const char *text, const char *value, int32_t num, menuselect_t select)
{
	if (!menu)
		return nullptr;

	qmenu_t *node = tag_new<qmenu_t>();
	menuitem_t *item = tag_new<menuitem_t>();
	item->text = text ? text : "";
	item->value = value ? value : "";
	item->num = num;
	item->select = select;
	node->it = item;

	menuinfo_t *info = MENU_INFO(menu);
	if (!info->items)
	{
		info->items = node;
	}
	else
	{
		qmenu_t *tail = info->items;
		while (tail->next)
			tail = tail->next;
		tail->next = node;
		node->prev = tail;
	}

	return node;
}

void FinishMenu(edict_t *ent, qmenu_t *menu, bool show)
{
	if (!ent || !ent->client || !menu)
		return;

	// `show` is the menu's initial visibility, not a hint -- show_observer_menu
	// loads its screen with show == false, and TAB is what puts it up. Making it
	// current regardless is what lets that work.
	add_to_queue(menu, &ent->client->menuqueue);
	set_current_menu(ent, menu);
	ent->client->selected = first_selectable(menu);
	ent->client->showmenu = show;
	DisplayMenu(ent);
}

void MenuNext(edict_t *ent)
{
	if (!menu_visible(ent) || !ent->client->selected)
		return;

	qmenu_t *menu = ent->client->curmenulink;
	qmenu_t *node = ent->client->selected;

	while (node->next)
	{
		node = node->next;
		if (MENUITEM(node)->select)
		{
			ent->client->selected = node;
			DisplayMenu(ent);
			return;
		}
	}

	if (qmenu_t *first = first_selectable(menu))
		ent->client->selected = first;
	DisplayMenu(ent);
}

void MenuPrev(edict_t *ent)
{
	if (!menu_visible(ent) || !ent->client->selected)
		return;

	qmenu_t *menu = ent->client->curmenulink;
	qmenu_t *node = ent->client->selected;

	while (node->prev)
	{
		node = node->prev;
		if (MENUITEM(node)->select)
		{
			ent->client->selected = node;
			DisplayMenu(ent);
			return;
		}
	}

	if (qmenu_t *last = last_selectable(menu))
		ent->client->selected = last;
	DisplayMenu(ent);
}

void UseMenu(edict_t *ent, int32_t arg)
{
	if (!menu_visible(ent) || !ent->client->selected)
		return;
	if (ent->client->menuusetime > level.time)
		return;

	ent->client->menuusetime = level.time + MENU_USE_COOLDOWN;

	qmenu_t *menu = ent->client->curmenulink;
	qmenu_t *item = ent->client->selected;
	menuitem_t *menu_item = MENUITEM(item);
	if (!menu_item->select)
		return;

	const uint32_t gen = ent->client->menugen;
	const uint64_t instance_id = MENU_INFO(menu)->instance_id;
	const auto result = static_cast<menuselect_result_t>(menu_item->select(ent, menu, item, arg));

	if (result == MSELECT_CLOSE)
	{
		if (ent->client->menugen == gen)
		{
			PopMenu(ent);
			return;
		}

		// A callback may have pushed a replacement screen. Close the captured
		// source only if it is still the same live allocation; TagMalloc can
		// recycle an address after a callback has freed it.
		if (qmenu_t *source = find_menu_instance(ent->client, menu, instance_id))
		{
			remove_from_queue(source, &ent->client->menuqueue);
			free_menu(source);
		}
		return;
	}

	// The callback owns a replacement screen and its visibility/layout.
	if (ent->client->menugen != gen)
		return;

	if (result == MSELECT_REDRAW)
		DisplayMenu(ent);
}

void PopMenu(edict_t *ent)
{
	if (!menu_visible(ent))
		return;

	qmenu_t *current = ent->client->curmenulink;
	if (MenuIsBase(ent) && (MENU_INFO(current)->flags & MENU_NOABORT))
		return;

	remove_from_queue(current, &ent->client->menuqueue);
	free_menu(current);

	qmenu_t *next = queue_tail(&ent->client->menuqueue);
	if (next && next != &ent->client->menuqueue)
	{
		set_current_menu(ent, next);
		ent->client->selected = first_selectable(next);
		ent->client->showmenu = true; // popping back to a screen shows it
		DisplayMenu(ent);
	}
	else
	{
		show_no_menu(ent);
	}
}

bool MenuThink(edict_t *ent)
{
	if (!menu_visible(ent))
		return false;

	if (ent->client->menutime <= level.time)
	{
		SendMenu(ent);
		ent->client->menutime = level.time + MENU_RESEND_TIME;
		return true;
	}

	return false;
}

// Releases a client's whole menu stack without drawing or sending anything.
// Every menu node, its menuinfo_t and its items are TAG_LEVEL blocks, but they
// carry std::strings (a title, each row's text and value), so dropping them and
// letting gi.FreeTags(TAG_LEVEL) reclaim the blocks hands back the block and
// leaks whatever a string longer than the small-string buffer allocated. Only
// free_menu() -- which runs the destructors -- actually releases one.
void free_client_menus(gclient_t *client)
{
	if (!client)
		return;

	while (qmenu_t *node = remove_from_queue(nullptr, &client->menuqueue))
		free_menu(node);

	client->menuqueue = {};
	client->curmenulink = nullptr;
	client->selected = nullptr;
	client->showmenu = false;
	client->menutext.clear();
	client->menugen++;
}

void clear_menus(edict_t *ent)
{
	if (!ent || !ent->client)
		return;

	free_client_menus(ent->client);
	show_no_menu(ent);
}
