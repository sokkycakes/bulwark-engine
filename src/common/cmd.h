/* Bulwark engine - console commands and the command buffer. ISC licence, see LICENSE. */
#ifndef BW_CMD_H
#define BW_CMD_H

#include "common.h"

#define CMD_MAX_ARGS 256	/* QC tokenizes whole entity spawn data with this */

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
bool Cmd_Exists(const char *name);
void Cmd_Remove(const char *name);

/* handler tried for any command the engine does not know; returns true if consumed */
typedef bool (*cmd_fallback_fn)(const char *line, int argc, const char **argv);
void Cmd_SetFallback(cmd_fallback_fn fn);
/* consulted before engine commands: for commands the game modules registered */
void Cmd_SetPreHook(cmd_fallback_fn fn);

void Cbuf_AddText(const char *text);
void Cbuf_InsertText(const char *text);
void Cbuf_Execute(void);
void Cmd_ExecuteString(const char *line);
/* the full text of the command currently executing, after argv[0] */
const char *Cmd_Args(void);

void Cmd_Init(void);

#endif
