/* Bulwark engine - alloca on the Xbox build. ISC licence, see LICENSE. */
#ifndef BW_XBOX_ALLOCA_H
#define BW_XBOX_ALLOCA_H
#define alloca(n) __builtin_alloca(n)
#endif
