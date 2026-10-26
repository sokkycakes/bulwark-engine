/* Bulwark engine - console commands and the command buffer. ISC licence, see LICENSE. */
#ifndef BW_CMD_H
#define BW_CMD_H

#include "common.h"

#define CMD_MAX_ARGS 80

typedef void (*cmd_fn)(int argc, const char **argv);

/* tokenised command line; argv strings live inside the struct */
typedef struct {
	int argc;
	const char *argv[CMD_MAX_ARGS];
	int argstart[CMD_MAX_ARGS]; /* byte offset of each token in the source text */
	int argend[CMD_MAX_ARGS];
	char store[8192];
	char source[8192];
} cmd_tokens_t;

/* console-style split: whitespace separated, double quotes group, // starts a comment */
void Cmd_TokenizeInto(cmd_tokens_t *t, const char *text);

void Cmd_Add(const char *name, cmd_fn fn);
/* the same, with a line of help for "help", "find" and "apropos" */
void Cmd_AddD(const char *name, cmd_fn fn, const char *description);
const char *Cmd_Description(const char *name);
void Cmd_Describe(const char *name, const char *description); /* only if it has none */
void Cmd_DescribeAll(void);
/* native plugins (plugin.c) */
void Plug_Init(void);
/* terrain.c: heightmap maps, built into maps/<name>.bsp before it loads */
void Terrain_Init(void);
bool Terrain_PrepareMap(const char *name);
void Plug_Frame(void);
void Plug_Shutdown(void);
void Cmd_InitCompat(void); /* cmd_compat.c */ /* help text for commands registered without any (cmd_extra.c) */
/* walks the registered commands and aliases */
typedef void (*cmd_walk_fn)(const char *name, const char *text, void *ctx);
void Cmd_ForEach(cmd_walk_fn fn, void *ctx);
void Cmd_ForEachAlias(cmd_walk_fn fn, void *ctx);
const char *Cmd_AliasText(const char *name);
void Cmd_SetAlias(const char *name, const char *text); /* creates or replaces */
/* writes the aliases as "alias" lines, for a config file */
void Cmd_WriteAliases(strbuf_t *sb);
bool Cmd_Exists(const char *name);
void Cmd_Remove(const char *name);

/* handler tried for any command the engine does not know; returns true if consumed */
typedef bool (*cmd_fallback_fn)(const char *line, int argc, const char **argv);
void Cmd_SetFallback(cmd_fallback_fn fn);
/* consulted before engine commands: for commands the game modules registered */
void Cmd_SetPreHook(cmd_fallback_fn fn);

/* execution levels: what a command, alias or cvar needs ("restrict"), and
 * what the running text has. The local console has RESTRICT_MAX. */
#define RESTRICT_MAX 50
int Cmd_ExecLevel(void);
void Cmd_ExecuteStringAtLevel(const char *line, int level); /* never above the current level */
int Cmd_Restriction(const char *name);                      /* -1 if no such command */
bool Cmd_SetRestriction(const char *name, int level);
bool Cmd_AliasLevel(const char *name, int *level);          /* -1: runs at its caller's level */
bool Cmd_SetAliasLevel(const char *name, int level);

void Cbuf_AddText(const char *text);
void Cbuf_AddTextAtLevel(const char *text, int level); /* never above the current level */
void Cbuf_InsertText(const char *text);
void Cbuf_Execute(void);
/* runs text once the given number of seconds have passed ("defer") */
void Cbuf_AddDeferred(double seconds, const char *text);
/* expands $cvar and ${cvar} references in a line; returns false if none */
bool Cmd_ExpandMacros(const char *in, char *out, size_t outsize);
void Cmd_ExecuteString(const char *line);
/* the full text of the command currently executing, after argv[0] */
const char *Cmd_Args(void);

/* evaluates an "if"/"set_calc" expression; false if it didn't fully parse */
bool Cmd_Evaluate(const char *text, char *out, size_t outsize, bool *truth);

/* config files: the client adds its binds through the write hook and
 * clears them through the reset hook */
typedef void (*cfg_write_fn)(strbuf_t *sb);
typedef void (*cfg_reset_fn)(void);
extern cfg_write_fn cfg_write_hook;
extern cfg_reset_fn cfg_reset_hook;
bool Cfg_Save(const char *name);
/* on quit: saves when cfg_save_auto is on and something changed */
void Cfg_SaveIfModified(void);
/* archived cvars and binds back to their defaults */
void Cfg_ResetDefaults(void);
/* opens or closes the console log when con_logfile changed */
void Cmd_CheckLogfile(void);

void Cmd_Init(void);
/* the console utility commands (cmd_extra.c) */
void Cmd_InitExtra(void);

/* teamplay-style $macros ($health, $location...): returns the text for a
 * name that isn't a cvar, or NULL */
typedef const char *(*cmd_macro_fn)(const char *name);
void Cmd_SetMacroProvider(cmd_macro_fn fn);

#endif
