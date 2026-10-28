/* Facts the HUD needs that it cannot read from the client's entities.

   csnz_model: the player model's name ("gign", "zombi_origin"). The HUD
   builds the character portrait resource/Hud/Portrait/hud_<name> from it
   (hud/panels.qc). The client-side player entity carries no usable model
   name for the HUD progs (OBSERVED: modelindex resolves to
   models/error.vvm), so the server publishes it. */
void
CSNZ_PublishModel(entity p)
{
	string m = p.model;
	int n;

	if (m == "")
		return;
	n = tokenizebyseparator(m, "/");
	m = argv(n - 1i);
	if (substring(m, -4, 4) == ".mdl")
		m = substring(m, 0, strlen(m) - 4);
	if (userinfo.GetString(p, "csnz_model") != m)
		userinfo.SetString(p, "csnz_model", m);
}

void
CSNZ_PublishHudInfo(void)
{
	for (entity p = world; (p = next.Player(p));)
		CSNZ_PublishModel(p);
}
