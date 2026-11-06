/* Bulwark engine - split screen seats: more local players on one client.
 * ISC licence, see LICENSE.
 *
 * docs/spec/splitscreen.md has the design. Seat 0 is the player who
 * connected; seats 1..3 join through "seat_join" on the same connection and
 * get their own player slot on the server. The client keeps one set of
 * per-player state per seat and swaps the active seat's into cl, so code
 * that reads cl.playerent, cl.stat*, cl.cmds or cl.viewangles works on
 * whichever seat CL_SetSeat chose. Outside the places that switch (the
 * client game's views, a seat's part of a server message, building a seat's
 * command, running a seat's console command) seat 0 is active. */
#include "client.h"
#include "keys.h"
#include "../common/cmd.h"
#include "../common/cvar.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

clseat_t cl_seats[MAX_SEATS];

static cvar_t *cl_seat_autojoin;

#define JOIN_RETRY 2.0 /* seconds before asking the server again */

static void CL_SeatsChanged(void);

static void store(int k)
{
	clseat_t *s = &cl_seats[k];
	s->playerslot = cl.playerslot;
	s->playerent = cl.playerent;
	s->clientcommandframe = cl.clientcommandframe;
	s->servercommandframe = cl.servercommandframe;
	s->cmd_start = cl.cmd_start;
	memcpy(s->viewangles, cl.viewangles, sizeof(s->viewangles));
}

static void load(int k)
{
	clseat_t *s = &cl_seats[k];
	cl.playerslot = s->playerslot;
	cl.playerent = s->playerent;
	cl.clientcommandframe = s->clientcommandframe;
	cl.servercommandframe = s->servercommandframe;
	cl.cmd_start = s->cmd_start;
	memcpy(cl.viewangles, s->viewangles, sizeof(cl.viewangles));
	cl.stattype = s->stattype;
	cl.statval = s->statval;
	cl.statstr = s->statstr;
	cl.cmds = s->cmds;
	cl.seat = k;
}

void CL_SetSeat(int k)
{
	if (k < 0 || k >= MAX_SEATS || k == cl.seat)
		return;
	store(cl.seat);
	load(k);
}

bool CL_SeatPlaying(int k)
{
	if (k == 0)
		return cl.state == ca_active;
	return k > 0 && k < MAX_SEATS && cl.state == ca_active && cl_seats[k].state == SEAT_ACTIVE;
}

int CL_NumSeats(void)
{
	int n = 0;
	if (cl.state != ca_active)
		return 0;
	for (int k = 0; k < MAX_SEATS; k++)
		if (CL_SeatPlaying(k))
			n = k + 1;
	return n;
}

int CL_SeatForDevice(int device)
{
	if (device < 0 || device > MAX_SEATS)
		return -1;
	for (int k = 0; k < MAX_SEATS; k++)
		if ((k == 0 || cl_seats[k].state != SEAT_FREE) && (cl_seats[k].device & (1u << device)))
			return k;
	return -1;
}

/* what the server allows, from its serverinfo; 1 for a server without seats */
static int server_seats(void)
{
	char v[16];
	CL_ServerKey("sv_maxseats", v, sizeof(v));
	return v[0] ? atoi(v) : 1;
}

static void send_join(int k)
{
	clseat_t *s = &cl_seats[k];
	char info[2048], line[2300];
	CL_SeatUserinfo(k, info, sizeof(info));
	snprintf(line, sizeof(line), "seat_join %d \"%s\"%s", k, info, s->guest ? " guest" : "");
	CL_SendStringCmd(line);
	s->join_time = cl.realtime;
}

static void reset_seat_state(int k)
{
	int active = cl.seat;
	CL_SetSeat(k);
	cl.playerslot = cl.playerent = 0;
	cl.servercommandframe = 0;
	cl.clientcommandframe = 1;
	memset(cl.cmds, 0, sizeof(cl_seats[k].cmds));
	CL_SetSeat(active);
}

void CL_InitSeats(void)
{
	memset(cl_seats, 0, sizeof(cl_seats));
	for (int k = 0; k < MAX_SEATS; k++)
		cl_seats[k].device = 0;
	/* the keyboard, the mouse and the first controller are the first player's */
	cl_seats[0].device = 1u << 0 | 1u << 1;
	cl_seats[0].state = SEAT_ACTIVE;
	load(0);
}

/* the guest's default name, as a console's: the sponsor's with a number */
static void default_name(int k, char *out, size_t size)
{
	snprintf(out, size, "%s (%d)", Cvar_String("name")[0] ? Cvar_String("name") : "player", k);
}

/* starts seat k for a new local player; false if there's no room */
static bool add_seat(int k, bool guest, const char *name, int device)
{
	clseat_t *s = &cl_seats[k];
	if (k < 1 || k >= MAX_SEATS || s->state != SEAT_FREE)
		return false;
	s->state = SEAT_JOINING;
	s->guest = guest;
	s->join_time = -JOIN_RETRY;
	s->device = 0;
	if (device >= 0 && device <= MAX_SEATS) {
		/* a device belongs to one player: take it from whoever had it */
		for (int i = 0; i < MAX_SEATS; i++)
			cl_seats[i].device &= ~(1u << device);
		s->device = 1u << device;
	}
	if (name && name[0])
		Str_Copy(s->name, name, sizeof(s->name));
	else
		default_name(k, s->name, sizeof(s->name));
	reset_seat_state(k);
	Con_Printf("%s joins on seat %d\n", s->name, k + 1);
	if (cl.state >= ca_loading && server_seats() <= k)
		Con_Printf("^3this server allows %d player%s per screen\n", server_seats(), server_seats() > 1 ? "s" : "");
	CL_SeatsChanged();
	return true;
}

static void remove_seat(int k)
{
	clseat_t *s = &cl_seats[k];
	if (k < 1 || k >= MAX_SEATS || s->state == SEAT_FREE)
		return;
	if (cl.state >= ca_loading && s->state != SEAT_JOINING) {
		char line[64];
		snprintf(line, sizeof(line), "seat_leave %d", k);
		CL_SendStringCmd(line);
	}
	Con_Printf("%s left seat %d\n", s->name, k + 1);
	IN_ReleaseSeat(k);
	s->state = SEAT_FREE;
	s->device = 0;
	reset_seat_state(k);
	CL_SeatsChanged();
}

void CL_SeatInfo(int k, int slot, int ent)
{
	clseat_t *s;
	int active = cl.seat;
	if (k < 1 || k >= MAX_SEATS)
		return;
	s = &cl_seats[k];
	if (slot == 255) {
		if (s->state != SEAT_FREE) {
			Con_Printf("%s isn't playing any more\n", s->name);
			s->state = SEAT_FREE;
			IN_ReleaseSeat(k);
			s->device = 0;
			CL_SeatsChanged();
		}
		return;
	}
	if (s->state == SEAT_FREE) {
		/* the server has a seat we don't want (left while the join was on
		 * its way): tell it again */
		char line[64];
		snprintf(line, sizeof(line), "seat_leave %d", k);
		CL_SendStringCmd(line);
		return;
	}
	CL_SetSeat(k);
	cl.playerslot = slot;
	cl.playerent = ent;
	CL_SetSeat(active);
	if (s->state == SEAT_JOINING)
		s->state = SEAT_LOADING;
}

void CL_SeatSpawned(int k)
{
	int active = cl.seat;
	if (k < 1 || k >= MAX_SEATS || cl_seats[k].state == SEAT_FREE)
		return;
	cl_seats[k].state = SEAT_ACTIVE;
	CL_SetSeat(k);
	cl.cmd_start = cl.realtime;
	CL_SetSeat(active);
}

/* a new connection, or none: seats ask again once the next one is up */
void CL_SeatsDisconnected(void)
{
	for (int k = 1; k < MAX_SEATS; k++)
		if (cl_seats[k].state != SEAT_FREE) {
			cl_seats[k].state = SEAT_JOINING;
			cl_seats[k].join_time = -JOIN_RETRY;
			reset_seat_state(k);
		}
}

void CL_SeatsConnected(void) { CL_SeatsDisconnected(); }

void CL_SeatsFrame(void)
{
	if (cl.state < ca_loading || cl.demoplayback)
		return;
	for (int k = 1; k < MAX_SEATS; k++) {
		clseat_t *s = &cl_seats[k];
		if (s->state == SEAT_JOINING && cl.realtime - s->join_time >= JOIN_RETRY && server_seats() > k)
			send_join(k);
	}
}

/* Press Start to join: a controller nobody has, on its Start button, adds a
 * guest with it, as a console does */
void CL_SeatDevicePressed(int device, int key)
{
	if (key != K_GP_MENU || !cl_seat_autojoin->ivalue || CL_SeatForDevice(device) >= 0)
		return;
	for (int k = 1; k < MAX_SEATS; k++)
		if (cl_seats[k].state == SEAT_FREE) {
			add_seat(k, true, NULL, device);
			return;
		}
}

/* the online party wants the local players' names (cl_live.c, when built in) */
static void CL_SeatsChanged(void)
{
	char line[512] = "live_guests";
	bool any = false;
	if (!Cmd_Exists("live_guests"))
		return;
	for (int k = 1; k < MAX_SEATS; k++)
		if (cl_seats[k].state != SEAT_FREE) {
			snprintf(line + strlen(line), sizeof(line) - strlen(line), " \"%s\"", cl_seats[k].name);
			any = true;
		}
	Cbuf_AddText(va("%s%s\n", line, any ? "" : " -"));
}

/* --- commands --------------------------------------------------------------- */

static int first_free(void)
{
	for (int k = 1; k < MAX_SEATS; k++)
		if (cl_seats[k].state == SEAT_FREE)
			return k;
	return -1;
}

/* seat_add [guest|<name>] [device]: another local player */
static void SeatAdd_f(int argc, const char **argv)
{
	int k = first_free(), device = argc > 2 ? atoi(argv[2]) : -1;
	bool guest = argc < 2 || !Str_ICmp(argv[1], "guest");
	if (k < 0) {
		Con_Printf("all %d seats are taken\n", MAX_SEATS);
		return;
	}
	add_seat(k, guest, guest ? NULL : argv[1], device);
}

/* seat_remove <seat>: seats are numbered from 1 here, as players see them */
static void SeatRemove_f(int argc, const char **argv)
{
	int k = argc > 1 ? atoi(argv[1]) - 1 : -1;
	if (k < 1 || k >= MAX_SEATS) {
		Con_Printf("seat_remove <2..%d>: seat 1 is the player who connected\n", MAX_SEATS);
		return;
	}
	remove_seat(k);
}

static void SeatList_f(int argc, const char **argv)
{
	static const char *states[] = {"free", "joining", "loading", "playing"};
	for (int k = 0; k < MAX_SEATS; k++) {
		const clseat_t *s = &cl_seats[k];
		char devs[64] = "";
		int slot = k == cl.seat ? cl.playerslot : s->playerslot;
		int ent = k == cl.seat ? cl.playerent : s->playerent;
		if (k && s->state == SEAT_FREE)
			continue;
		for (int d = 0; d <= MAX_SEATS; d++)
			if (s->device & (1u << d))
				snprintf(devs + strlen(devs), sizeof(devs) - strlen(devs), "%s%s", devs[0] ? "," : "",
				         d ? va("pad%d", d) : "keyboard");
		Con_Printf("seat %d: %-20s %-8s slot %d entity %d %s%s\n", k + 1, k ? s->name : Cvar_String("name"),
		           k ? states[s->state] : (cl.state == ca_active ? "playing" : "-"), slot, ent, devs[0] ? devs : "no input",
		           s->guest ? " (guest)" : "");
	}
}

/* seat_device <seat> <device>: 0 is the keyboard and mouse, 1..4 the controllers */
static void SeatDevice_f(int argc, const char **argv)
{
	int k = argc > 2 ? atoi(argv[1]) - 1 : -1, d = argc > 2 ? atoi(argv[2]) : -1;
	if (k < 0 || k >= MAX_SEATS || d < 0 || d > MAX_SEATS || (k && cl_seats[k].state == SEAT_FREE)) {
		Con_Printf("seat_device <seat> <0 keyboard and mouse | 1..%d controller>\n", MAX_SEATS);
		return;
	}
	for (int i = 0; i < MAX_SEATS; i++)
		cl_seats[i].device &= ~(1u << d);
	cl_seats[k].device |= 1u << d;
}

void CL_InitSeatCommands(void)
{
	cl_seat_autojoin = Cvar_Get("cl_seat_autojoin", "1", CVAR_ARCHIVE,
	                            "Start on a controller nobody is using adds a split screen guest");
	Cmd_AddD("seat_add", SeatAdd_f, "seat_add [guest|<name>] [device]: another local player (split screen)");
	Cmd_AddD("seat_remove", SeatRemove_f, "seat_remove <seat>: a split screen player leaves");
	Cmd_AddD("seat_list", SeatList_f, "The local players and their controllers");
	Cmd_AddD("seat_device", SeatDevice_f, "seat_device <seat> <device>: give a player the keyboard (0) or a controller (1..4)");
}
