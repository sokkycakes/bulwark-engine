/* Bulwark engine - dedicated server entry point. The host itself is
 * src/server/sv_ded.c, which "bulwark -dedicated" runs too. ISC licence,
 * see LICENSE. */
#include "common/common.h"
#include "server/sv_local.h"

int main(int argc, char **argv) { return Ded_Main(argc, argv); }
