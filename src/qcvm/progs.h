/* Bulwark engine - progs.dat on-disk layout.
 * See docs/spec/progs-format.md for how each part was established.
 * ISC licence, see LICENSE. */
#ifndef BW_PROGS_H
#define BW_PROGS_H

#include <stdint.h>

#define PROGS_VERSION_QUAKE 6
#define PROGS_VERSION_EXTENDED 7

/* the common header shared by version 6 and 7 */
typedef struct {
	int32_t version;
	int32_t crc;
	int32_t ofs_statements, numstatements;
	int32_t ofs_globaldefs, numglobaldefs;
	int32_t ofs_fielddefs, numfielddefs;
	int32_t ofs_functions, numfunctions;
	int32_t ofs_strings, numstrings; /* numstrings is the string table size in bytes */
	int32_t ofs_globals, numglobals; /* numglobals counts 32-bit words */
	/* version 7 only from here on */
	int32_t entityfields; /* entity size in 32-bit words, including expanded vectors */
} progs_header_t;

#define PROGS_HEADER_V6_SIZE 56

typedef struct {
	uint16_t op;
	uint16_t a, b, c;
} dstatement16_t;

typedef struct {
	uint32_t op;
	uint32_t a, b, c;
} dstatement32_t;

/* global and field definitions */
typedef struct {
	uint16_t type; /* low bits are the etype, DEF_SAVEGLOBAL marks saved globals */
	uint16_t ofs;
	int32_t s_name;
} ddef16_t;

typedef struct {
	uint32_t type;
	uint32_t ofs;
	int32_t s_name;
} ddef32_t;

#define DEF_SAVEGLOBAL (1 << 15)

typedef struct {
	int32_t first_statement; /* negative: builtin number; zero: builtin looked up by name */
	int32_t parm_start;
	int32_t locals; /* total words of parameters plus locals */
	int32_t profile;
	int32_t s_name;
	int32_t s_file;
	int32_t numparms;
	uint8_t parm_size[8];
} dfunction_t;

/* value types as they appear in ddef type fields */
typedef enum {
	ev_void,
	ev_string,
	ev_float,
	ev_vector,
	ev_entity,
	ev_field,
	ev_function,
	ev_pointer,
	ev_integer,
	ev_uint,
	ev_int64,
	ev_uint64,
	ev_double,
	ev_variant,
	ev_struct,
	ev_union,
	ev_accessor,
	ev_enum,
	ev_bool,
} etype_t;

/* fixed global offsets used by the calling convention */
#define OFS_NULL 0
#define OFS_RETURN 1
#define OFS_PARM0 4
#define OFS_PARM(n) (OFS_PARM0 + (n) * 3)
#define RESERVED_OFS 28
#define MAX_PARMS 8

#endif
