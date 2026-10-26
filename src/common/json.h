/* Bulwark engine - JSON document tree. ISC licence, see LICENSE.
 * Objects keep their members in file order and numbers keep their source
 * text, so a file that is loaded and written back changes only where the
 * caller changed it. */
#ifndef BW_JSON_H
#define BW_JSON_H

#include "common.h"

typedef enum {
	JSON_NULL,
	JSON_FALSE,
	JSON_TRUE,
	JSON_NUMBER,
	JSON_STRING,
	JSON_ARRAY,
	JSON_OBJECT
} json_type_t;

typedef struct json_s json_t;
struct json_s {
	json_type_t type;
	char *key;      /* member name when the value sits in an object */
	char *str;      /* string value, or a number's text */
	double num;
	json_t *child;  /* first element or member */
	json_t *last;
	json_t *next;
	int count;
};

/* NULL on error, with a message (including the line) in err */
json_t *JSON_Parse(const char *text, char *err, size_t errsize);
void JSON_Free(json_t *j);

json_t *JSON_New(json_type_t type);
json_t *JSON_NewString(const char *s);
json_t *JSON_NewNumber(double n);
json_t *JSON_NewNumberText(const char *text); /* keeps text as written, e.g. a 64-bit integer */
json_t *JSON_NewBool(bool b);
json_t *JSON_Clone(const json_t *j);
bool JSON_Equal(const json_t *a, const json_t *b);

/* object access; keys are case-sensitive like the format they come from */
json_t *JSON_Get(const json_t *obj, const char *key);
const char *JSON_GetString(const json_t *obj, const char *key, const char *def);
double JSON_GetNumber(const json_t *obj, const char *key, double def);
bool JSON_GetBool(const json_t *obj, const char *key, bool def);
/* replaces the member in place if it exists, otherwise appends; takes ownership of val */
void JSON_Set(json_t *obj, const char *key, json_t *val);
void JSON_SetString(json_t *obj, const char *key, const char *s);
void JSON_SetNumber(json_t *obj, const char *key, double n);
void JSON_SetBool(json_t *obj, const char *key, bool b);
bool JSON_Remove(json_t *obj, const char *key);
/* renames a member, keeping its position */
bool JSON_Rename(json_t *obj, const char *from, const char *to);

/* array access */
json_t *JSON_At(const json_t *arr, int index);
void JSON_Append(json_t *arr, json_t *val);
void JSON_InsertAfter(json_t *arr, json_t *after, json_t *val); /* after NULL inserts first */
/* unlinks child from its array or object without freeing it */
bool JSON_Detach(json_t *parent, json_t *child);

#define JSON_FOREACH(it, parent) for (json_t *it = (parent) ? (parent)->child : NULL; it; it = it->next)

bool JSON_IsString(const json_t *j);
bool JSON_IsNumber(const json_t *j);
bool JSON_IsObject(const json_t *j);
bool JSON_IsArray(const json_t *j);

/* indent < 0 writes compact text; otherwise that many spaces per level */
void JSON_Write(strbuf_t *sb, const json_t *j, int indent);

#endif
