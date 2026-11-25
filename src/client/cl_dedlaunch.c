/* Bulwark engine - starting a dedicated server on this machine from the
 * client (docs/spec/lobby.md, "Dedicated hosting").
 *
 * The lobby's "Dedicated server" type runs the dedicated server program in
 * the background instead of a listen server: the same base and game
 * directories, a free UDP port, and the lobby's settings as a config it
 * writes to the player's game directory (lobby_dedicated.cfg, run with
 * +exec after the program's own start-up). The program's output goes to
 * dedicated.log beside it. The server keeps running when this client
 * leaves, as a dedicated server does; "ded_stop" stops the one this client
 * started, and "rcon" reaches it (the lobby sets rcon_password and
 * rcon_address for the host).
 *
 * Which program: ded_exe when set, else bulwark-ded beside this program,
 * else this program with -dedicated. ISC licence, see LICENSE. */
#include "dedlaunch.h"
#include "../common/cmd.h"
#include "../common/cvar.h"
#include "../common/fs.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(__3DS__) || defined(__NDS__) || defined(__EMSCRIPTEN__)
#define DED_NO_PROCESSES
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

static cvar_t *ded_exe, *ded_port;

#ifdef DED_NO_PROCESSES

int DedLaunch_Start(const char *commands, char *err, size_t errsize)
{
	(void)commands;
	Str_Copy(err, "This platform can't start a dedicated server; host a listen server instead", errsize);
	return 0;
}
bool DedLaunch_Supported(void) { return false; }
bool DedLaunch_Running(void) { return false; }
int DedLaunch_Port(void) { return 0; }
void DedLaunch_Stop(void) {}

#else

bool DedLaunch_Supported(void) { return true; }

static pid_t child = -1;
static int child_port;

static bool executable(const char *path)
{
	struct stat st;
	return path[0] && stat(path, &st) == 0 && S_ISREG(st.st_mode) && access(path, X_OK) == 0;
}

/* the directory this program is in */
static bool self_dir(char *out, size_t size)
{
	char self[1024];
	ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
	char *slash;
	if (n <= 0)
		return false;
	self[n] = 0;
	slash = strrchr(self, '/');
	if (!slash)
		return false;
	*slash = 0;
	Str_Copy(out, self, size);
	return true;
}

/* the program and whether it needs -dedicated */
static bool find_program(char *out, size_t size, bool *needflag)
{
	char dir[1024];
	*needflag = false;
	if (ded_exe->string[0]) {
		Str_Copy(out, ded_exe->string, size);
		return executable(out);
	}
	if (!self_dir(dir, sizeof(dir)))
		return false;
	snprintf(out, size, "%.900s/bulwark-ded", dir);
	if (executable(out))
		return true;
	{
		char self[1024];
		ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
		if (n <= 0)
			return false;
		self[n] = 0;
		Str_Copy(out, self, size);
		*needflag = true;
		return true;
	}
}

static bool port_free(int port)
{
	struct sockaddr_in a;
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	bool ok;
	if (s < 0)
		return false;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_ANY);
	a.sin_port = htons((uint16_t)port);
	ok = bind(s, (struct sockaddr *)&a, sizeof(a)) == 0;
	close(s);
	return ok;
}

bool DedLaunch_Running(void)
{
	int status;
	if (child <= 0)
		return false;
	if (waitpid(child, &status, WNOHANG) == child) {
		Con_Printf("dedicated server (pid %d) stopped%s\n", (int)child,
		           WIFEXITED(status) ? va(", exit code %d", WEXITSTATUS(status)) : "");
		child = -1;
		return false;
	}
	return true;
}

int DedLaunch_Port(void) { return DedLaunch_Running() ? child_port : 0; }

void DedLaunch_Stop(void)
{
	if (!DedLaunch_Running())
		return;
	kill(child, SIGTERM);
	for (int i = 0; i < 50 && waitpid(child, NULL, WNOHANG) == 0; i++)
		usleep(20000);
	if (waitpid(child, NULL, WNOHANG) == 0) {
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
	}
	Con_Printf("dedicated server (pid %d) stopped\n", (int)child);
	child = -1;
}

int DedLaunch_Start(const char *commands, char *err, size_t errsize)
{
	char program[1024], cfgpath[1024], logpath[1024], portstr[16];
	const char *args[64];
	int n = 0, port = 0;
	bool needflag;
	FILE *f;
	pid_t pid;

	if (!find_program(program, sizeof(program), &needflag)) {
		snprintf(err, errsize, "No dedicated server program (bulwark-ded) was found%s%s",
		         ded_exe->string[0] ? " at " : "", ded_exe->string);
		return 0;
	}
	for (int p = BW_MAX(1, ded_port->ivalue); p < BW_MAX(1, ded_port->ivalue) + 32 && p < 65536; p++)
		if (port_free(p)) {
			port = p;
			break;
		}
	if (!port) {
		snprintf(err, errsize, "No free UDP port from %d for the dedicated server", ded_port->ivalue);
		return 0;
	}
	/* the settings, as a config the server runs after its own start-up */
	if (!FS_WritePath("lobby_dedicated.cfg", cfgpath, sizeof(cfgpath)) || !FS_CreatePath(cfgpath) ||
	    !(f = fopen(cfgpath, "w"))) {
		snprintf(err, errsize, "Couldn't write lobby_dedicated.cfg");
		return 0;
	}
	fprintf(f, "// written by the lobby for the dedicated server it starts\n%s\nset sv_port %d\n", commands, port);
	fclose(f);
	if (!FS_WritePath("dedicated.log", logpath, sizeof(logpath)))
		Str_Copy(logpath, "/dev/null", sizeof(logpath));

	args[n++] = program;
	if (needflag)
		args[n++] = "-dedicated";
	args[n++] = "-basedir";
	args[n++] = FS_BaseDir();
	for (int i = 0; i < FS_NumGameDirs() && n < 48; i++) {
		args[n++] = "-game";
		args[n++] = FS_GameDirAt(i);
	}
	if (FS_UserDir()[0]) {
		args[n++] = "-userdir";
		args[n++] = FS_UserDir();
	}
	if (FS_EngineDir() && FS_EngineDir()[0]) {
		args[n++] = "-enginedata";
		args[n++] = FS_EngineDir();
	}
	args[n++] = "+set";
	args[n++] = "sv_port";
	snprintf(portstr, sizeof(portstr), "%d", port);
	args[n++] = portstr;
	args[n++] = "+exec";
	args[n++] = "lobby_dedicated.cfg";
	args[n] = NULL;

	if (DedLaunch_Running())
		Con_Printf("^3a dedicated server this game started (pid %d, port %d) is still running; starting another\n",
		           (int)child, child_port);

	pid = fork();
	if (pid == 0) {
		int nul = open("/dev/null", O_RDONLY);
		int log = open(logpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (nul >= 0)
			dup2(nul, 0);
		if (log >= 0) {
			dup2(log, 1);
			dup2(log, 2);
		}
		/* its own process group: a ctrl-C in this terminal leaves it running */
		setsid();
		for (int fd = 3; fd < 1024; fd++)
			close(fd);
		execv(program, (char *const *)args);
		_exit(127);
	}
	if (pid < 0) {
		snprintf(err, errsize, "Couldn't start %s: %s", program, strerror(errno));
		return 0;
	}
	child = pid;
	child_port = port;
	Con_Printf("dedicated server: started %s%s (pid %d) on port %d; its output is in %s\n", program,
	           needflag ? " -dedicated" : "", (int)pid, port, logpath);
	return port;
}

#endif

static void DedStop_f(int argc, const char **argv)
{
	(void)argc, (void)argv;
	if (!DedLaunch_Running())
		Con_Printf("no dedicated server started by this game is running\n");
	DedLaunch_Stop();
}

static void DedStatus_f(int argc, const char **argv)
{
	(void)argc, (void)argv;
	if (DedLaunch_Running())
		Con_Printf("dedicated server running on port %d\n", DedLaunch_Port());
	else
		Con_Printf("no dedicated server started by this game is running\n");
}

void DedLaunch_Init(void)
{
	ded_exe = Cvar_Get("ded_exe", "", CVAR_ARCHIVE,
	                   "The dedicated server program the lobby starts; empty: bulwark-ded beside this program");
	ded_port = Cvar_Get("ded_port", "27500", 0, "First UDP port tried for a dedicated server the lobby starts");
	Cmd_AddD("ded_stop", DedStop_f, "Stop the dedicated server this game started");
	Cmd_AddD("ded_status", DedStatus_f, "Whether the dedicated server this game started is running");
}
