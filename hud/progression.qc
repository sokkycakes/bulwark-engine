/* progression: HUD overlay for player progression (level, rank, money, XP bar).
 *
 * Reads from userinfo keys set by csnz_progression.qc:
 *   csnz_level, csnz_rank, csnz_money, csnz_ampoules, csnz_virus,
 *   csnz_score, csnz_team
 *
 * Original code (ISC). DESIGN layout. */

/* rank names must match the server-side PROG_RankName(). DESIGN
   placeholders: no level-class names were found in this build's
   cso_na_en.txt (its CSO_Rank_* strings are league tiers and ranking
   labels, not player ranks). */
string
Prog_RankName(int rank)
{
	switch (rank) {
	case 0: return "Trainee";
	case 1: return "Amateur";
	case 2: return "Regular";
	case 3: return "Semi-Pro";
	case 4: return "Professional";
	case 5: return "Veteran";
	case 6: return "Expert";
	case 7: return "Elite";
	case 8: return "Master";
	case 9: return "Grand Master";
	case 10: return "Champion";
	case 11: return "Hero";
	case 12: return "Legend";
	case 13: return "Mythic";
	case 14: return "Immortal";
	case 15: return "Titan";
	case 16: return "God";
	case 17: return "Transcendent";
	default: return "???";
	}
}

/* rank color (DESIGN) */
vector
Prog_RankColor(int rank)
{
	if (rank <= 2) return [0.7, 0.7, 0.7];		/* grey */
	if (rank <= 5) return [0.3, 0.8, 0.3];		/* green */
	if (rank <= 8) return [0.3, 0.5, 1.0];		/* blue */
	if (rank <= 11) return [0.8, 0.3, 0.8];	/* purple */
	if (rank <= 14) return [1.0, 0.8, 0.2];	/* gold */
	return [1.0, 0.3, 0.3];			/* red */
}

void
Prog_Draw(font_s fnt)
{
	/* read player progression from userinfo */
	float level = floor(stof(getplayerkeyvalue(player_localnum, "csnz_level")));
	int rank = (int)stof(getplayerkeyvalue(player_localnum, "csnz_rank"));
	float money = floor(stof(getplayerkeyvalue(player_localnum, "csnz_money")));

	if (level <= 0) return;	/* progression not loaded yet */

	string rankStr = Prog_RankName(rank);
	vector rankCol = Prog_RankColor(rank);

	/* draw rank + level in top-right corner */
	float x = HUD_W - 10;
	float y = 8;

	/* rank badge */
	draw.RText(Art_Pos([x, y]), sprintf("Lv.%d  %s", level, rankStr), fnt);

	/* money below rank */
	y += 16;
	draw.RText(Art_Pos([x, y]), sprintf("$%d", money), fnt);

	/* mode-specific displays */
	/* ampoules for Snowman */
	float ampoules = floor(stof(getplayerkeyvalue(player_localnum, "csnz_ampoules")));
	if (ampoules > 0) {
		y += 16;
		draw.RText(Art_Pos([x, y]), sprintf("Ampoules: %d/3", ampoules), fnt);
	}

	/* virus points for Zombi 5 */
	float virus = floor(stof(getplayerkeyvalue(player_localnum, "csnz_virus")));
	if (virus > 0) {
		y += 16;
		draw.RText(Art_Pos([x, y]), sprintf("Virus: %d", virus), fnt);
	}
}

/* shelter HP display */
void
Prog_DrawShelter(font_s fnt)
{
	float shp = floor(stof(serverkey("csnz_shelter_hp")));
	float shmax = floor(stof(serverkey("csnz_shelter_max")));

	if (shmax <= 0) return;

	float pct = shp / shmax;
	vector col;
	if (pct > 0.5) col = [0.3, 1.0, 0.3];
	else if (pct > 0.25) col = [1.0, 1.0, 0.3];
	else col = [1.0, 0.3, 0.3];

	/* center top */
	float cx = HUD_W * 0.5;
	draw.Text(Art_Pos([cx - 60, 40]), sprintf("Shelter: %d / %d", shp, shmax), fnt);

	/* health bar */
	float barW = 200;
	float barH = 8;
	float barX = cx - barW * 0.5;
	float barY = 56;
	drawfill(Art_Pos([barX, barY]), [barW, barH] * g_hs, [0.2, 0.2, 0.2], 0.5);
	drawfill(Art_Pos([barX, barY]), [barW * pct, barH] * g_hs, col, 0.8);
}

/* scenario round display */
void
Prog_DrawScenario(font_s fnt)
{
	float round = floor(stof(serverkey("csnz_sctx_round")));
	if (round <= 0) return;

	/* below the shelter bar (40-64) when a shelter is shown */
	float cx = HUD_W * 0.5;
	float y = (stof(serverkey("csnz_shelter_max")) > 0) ? 72 : 40;
	draw.Text(Art_Pos([cx - 40, y]), sprintf("Round %d", round), fnt);
}
