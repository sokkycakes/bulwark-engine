/* CS weapon list shared by every rules progs. The weapons themselves are
   defined in decls/def/cs_weapons.def (written by
   tools/assets/make_cs_weapons.py). */

string g_csWeapons = "weapon_knife weapon_usp weapon_glock18 weapon_p228 weapon_deagle weapon_fiveseven weapon_elite weapon_m3 weapon_xm1014 weapon_mac10 weapon_tmp weapon_mp5 weapon_ump45 weapon_p90 weapon_galil weapon_famas weapon_ak47 weapon_m4a1 weapon_sg552 weapon_aug weapon_scout weapon_awp weapon_g3sg1 weapon_sg550 weapon_m249 weapon_hegrenade";

/* precache every CS weapon so any mode, pickup or give command can hand
   one out without a "not precached" error */
void
CS_PrecacheWeapons(void)
{
	int c = (int)tokenize(g_csWeapons);

	for (int i = 0i; i < c; i++) {
		/* precaching parses decls, which re-tokenizes */
		tokenize(g_csWeapons);
		precache.Entity(argv(i));
	}
}

/* every CS ammo type topped up to its maximum (decls/def/ammo.def) */
string g_csAmmo = "ammo_9mm ammo_45acp ammo_50ae ammo_357sig ammo_57mm ammo_buckshot ammo_556nato ammo_556natobox ammo_762nato ammo_338magnum";

void
CS_FillAmmo(entity p)
{
	int c = (int)tokenize(g_csAmmo);

	for (int i = 0i; i < c; i++) {
		tokenize(g_csAmmo);
		string a = argv(i);
		ents.Input(p, "GiveAmmo", strcat(a, " 999"), world);
	}
}
