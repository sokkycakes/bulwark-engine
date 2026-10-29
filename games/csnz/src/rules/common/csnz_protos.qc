/* csnz_protos: forward declarations for the progression, inventory, ability,
 * achievement, item box, hook and new mode files, so they can call each
 * other (and csnz_common can call the hooks) regardless of include order.
 * Keep in step with the definitions. */

.float zv_lastHealth;	/* health before this frame's hits, for the damage hooks */

/* csnz_progression.qc */
float PROG_XPForLevel(int level);
string PROG_RankName(int rank);
int PROG_RankForLevel(int level);
string PROG_SavePath(entity p);
void PROG_Load(entity p);
void PROG_Save(entity p);
void PROG_AwardXP(entity p, float amount);
void PROG_OnKill(entity killer, entity victim);
void PROG_OnRoundEnd(float winningTeam);
void PROG_OnMatchEnd(float winningTeam);
void PROG_AwardMoney(entity p, float amount);
bool PROG_SpendMoney(entity p, float amount);
bool PROG_Command(entity p, string cmd);
/* csnz_inventory.qc */
string INV_SavePath(entity p);
void INV_Load(entity p);
void INV_Save(entity p);
void INV_PublishChars(entity p);
bool INV_HasItem(entity p, string itemId);
int INV_Count(entity p);
bool INV_AddItem(entity p, string itemId);
bool INV_RemoveItem(entity p, string itemId);
float INV_WeaponPrice(string weaponId);
float INV_CharacterPrice(string charId);
bool INV_BuyItem(entity p, string itemId);
bool INV_EquipCharacter(entity p, string charId);
bool INV_EquipWeapon(entity p, string weaponId, bool primary);
bool INV_Command(entity p, string cmd);
void INV_PlayerConnect(entity p);
void INV_PlayerDisconnect(entity p);
/* csnz_char_abilities.qc */
void CA_PassiveTick(entity p);
bool CA_UseAbility(entity p);
float CA_DamageModifier(entity attacker, entity victim);
void CA_OnKill(entity killer);
void CA_SetCharacter(entity p, string charDef);
void CA_ApplyLoadout(entity p);
void CA_Frame(void);
bool CA_Command(entity p, string cmd);
/* csnz_zombie_abilities.qc */
float ZA_TypeForClass(string cls);
void ZA_Init(entity p, string cls);
string ZA_ClassFor(entity p);
void ZA_BecomeZombie(entity p, string baseClass);
void ZA_CloudThink(void);
void ZA_SpawnCloud(vector where, float teamNum, float damage, float duration, float radius);
void ZA_VoodooAura(entity p);
void ZA_GanymedeGlide(entity p);
void ZA_StamperCheck(entity p);
void ZA_BansheeScream(entity p);
void ZA_VenomguardApply(entity victim, entity attacker);
void ZA_PoisonTick(entity p);
void ZA_StingfingerBurst(entity p);
void ZA_SprintTick(entity p);
void ZA_MetatronAura(entity p);
float ZA_MetatronDamageBoost(entity attacker);
void ZA_LilithStealth(entity p);
void ZA_LilithReveal(entity p);
void ZA_ChaserSprint(entity p);
void ZA_BlotterDeath(entity p);
void ZA_RustywingGlide(entity p);
void ZA_AkshaApply(entity victim, entity attacker);
void ZA_FireTick(entity p);
float ZA_SoldierDamageReduction(entity victim, entity attacker);
void ZA_SoldierPublish(entity p);
bool ZA_UseAbility(entity p);
void ZA_Frame(void);
void ZA_Damaged(entity victim, entity attacker);
void ZA_Killed(entity victim, entity inflictor, entity attacker, string weapon);
bool ZA_Command(entity p, string cmd);
/* csnz_buymenu.qc */
void BM_ShowCategories(entity p);
void BM_ShowList(entity p, string list, string catName);
bool BM_Command(entity p, string cmd);
/* csnz_achievements.qc */
string ACH_Name(float id);
float ACH_Threshold(float id);
float ACH_XPReward(float id);
string ACH_Category(float id);
string ACH_SavePath(entity p);
void ACH_Load(entity p);
void ACH_Save(entity p);
void ACH_Award(entity p, float achId);
void ACH_Check(entity p, float achId, float progress);
void ACH_SetProgress(entity p, float achId, float value);
void ACH_OnKill(entity killer, entity victim);
void ACH_OnHeadshot(entity killer);
void ACH_OnDeath(entity victim);
void ACH_OnZombieKill(entity killer);
void ACH_OnBombPlant(entity planter);
void ACH_OnBombDefuse(entity defuser);
void ACH_OnHostageRescue(entity rescuer);
void ACH_OnDamageDealt(entity attacker, float amount);
void ACH_OnDamageTaken(entity victim, float amount);
void ACH_OnHeal(entity healer, float amount);
void ACH_OnShot(entity shooter, bool hit);
void ACH_OnShots(entity shooter, float fired, float hit);
void ACH_OnMVP(entity player);
void ACH_OnRoundSurvived(entity player);
void ACH_OnRoundStart(void);
void ACH_OnZombieRoundSurvived(entity player);
void ACH_OnLastStandWin(entity player);
void ACH_OnMatchEnd(entity player, float mode, bool won);
bool ACH_Command(entity p, string cmd);
void ACH_PlayerConnect(entity p);
void ACH_PlayerDisconnect(entity p);
/* csnz_itembox.qc */
string IB_RandomFromPool(string pool);
int IB_RollGrade(float tierBonus);
string IB_GradeName(int grade);
string IB_GradePool(int grade);
bool IB_OpenBox(entity p, string tier);
bool IB_DailyBox(entity p);
bool IB_Command(entity p, string cmd);
/* csnz_hooks.qc */
void CSNZ_PlayerSpawned(entity p);
void CSNZ_Disconnect(entity p);
bool CSNZ_Command(entity p, string cmd);
void CSNZ_PreEquip(entity p);
void CSNZ_PostEquip(entity p);
void CSNZ_Damaged(entity victim, entity attacker);
void CSNZ_Killed(entity p, entity inflictor, entity attacker, string weapon);
void CSNZ_Frame(void);
void CSNZ_RoundEnded(float winner);
float CSNZ_MatchCategory(void);
void CSNZ_MatchEnded(float winner, entity ffaWinner);
/* shelter.qc */
void SH_EquipHuman(entity p);
void SH_EquipZombie(entity p);
void SH_Equip(entity p);
float SH_PickTeam(entity p);
void SH_RoundStart(void);
void SH_DamageShelter(float amount, float whichTeam);
void SH_Killed(entity victim, entity inflictor, entity attacker, string weapon);
void SH_Damaged(entity victim, entity attacker);
float SH_CheckRound(void);
bool SH_Command(entity p, string cmd);
void SH_Frame(void);
void CSM_Shelter(bool isTeam);
/* zombi5.qc */
bool Z5_IsZombie(entity p);
void Z5_MakeZombie(entity p, bool isOrigin, vector where, vector ang);
void Z5_EquipHuman(entity p);
void Z5_Equip(entity p);
float Z5_PickTeam(entity p);
void Z5_SelectZombies(void);
void Z5_RoundStart(void);
void Z5_Live(void);
void Z5_Killed(entity victim, entity inflictor, entity attacker, string weapon);
void Z5_Damaged(entity victim, entity attacker);
float Z5_CheckRound(void);
void Z5_Frame(void);
bool Z5_Command(entity p, string cmd);
void CSM_Zombi5(bool randomWeapon);
/* scenario.qc */
void SCTX_SpawnMonster(void);
void SCTX_Equip(entity p);
void SCTX_RoundStart(void);
void SCTX_Killed(entity victim, entity inflictor, entity attacker, string weapon);
float SCTX_CheckRound(void);
void SCTX_Frame(void);
bool SCTX_Command(entity p, string cmd);
void CSM_ScenarioTX(void);
void CSM_HumanScenario(void);
void CSM_ZombieHorrorEscape(void);
void CSM_SuperSoldier(void);
void CSM_BattleRush(void);
void CSM_ZombiConquest(void);
void CSM_ZSRift(void);
void CSM_ZombieScenario(void);
void CSM_ZombiTeamAnn(void);
