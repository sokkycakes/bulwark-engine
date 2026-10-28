/* Shelter Team (mode 28) is a regular mode of csnz.dat, not a Studio mode.
 * studio.dat only needs the name the mode registry calls; leaving
 * modes/sheltert.qc out keeps studio.dat inside the engine's 256 spare
 * entity words for an added progs. Original code (ISC). */
void
CSM_ShelterTeam(void)
{
	print("studio: mode 28 (Shelter Team) runs in csnz.dat, not in a Studio map\n");
	CSM_Shelter(false);
}
