/* Bulwark engine - console variables. ISC licence, see LICENSE. */
#ifndef BW_CVAR_H
#define BW_CVAR_H

#include "common.h"

enum {
	CVAR_ARCHIVE = 1 << 0,    /* saved to config */
	CVAR_SERVERINFO = 1 << 1, /* mirrored into serverinfo */
	CVAR_USERINFO = 1 << 2,
	CVAR_READONLY = 1 << 3,
	CVAR_USERCREATED = 1 << 4, /* made by "set" or by QC, not by engine code */
};

typedef struct cvar_s {
	char *name;
	char *string;
	char *defstring;
	char *description;
	float value;
	int ivalue;
	int flags;
	int modified;
	struct cvar_s *next;
} cvar_t;

/* bumped by every change to any cvar */
extern int cvar_generation;

cvar_t *Cvar_Get(const char *name, const char *defvalue, int flags, const char *description);
cvar_t *Cvar_Find(const char *name);
void Cvar_Set(const char *name, const char *value);
void Cvar_SetValue(const char *name, float value);
void Cvar_SetDirect(cvar_t *var, const char *value);
float Cvar_Value(const char *name);
const char *Cvar_String(const char *name);
cvar_t *Cvar_First(void);
/* handles "name" and "name value" typed at the console; returns false if no such cvar */
bool Cvar_Command(int argc, const char **argv);
void Cvar_Init(void);

#endif
