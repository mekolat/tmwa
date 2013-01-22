#include "mob.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "../common/cxxstdio.hpp"
#include "../common/mt_rand.hpp"
#include "../common/nullpo.hpp"
#include "../common/socket.hpp"

#include "battle.hpp"
#include "clif.hpp"
#include "itemdb.hpp"
#include "map.hpp"
#include "npc.hpp"
#include "party.hpp"
#include "pc.hpp"
#include "skill.hpp"

#include "../poison.hpp"

#define MIN_MOBTHINKTIME 100

#define MOB_LAZYMOVEPERC 50     // Move probability in the negligent mode MOB (rate of 1000 minute)
#define MOB_LAZYWARPPERC 20     // Warp probability in the negligent mode MOB (rate of 1000 minute)

struct mob_db mob_db[2001];

/*==========================================
 * Local prototype declaration   (only required thing)
 *------------------------------------------
 */
static
int distance(int, int, int, int);
static
int mob_makedummymobdb(int);
static
void mob_timer(timer_id, tick_t, custom_id_t, custom_data_t);
static
int mob_skillid2skillidx(int mob_class, SkillID skillid);
static
int mobskill_use_id(struct mob_data *md, struct block_list *target,
                      int skill_idx);
static
int mob_unlocktarget(struct mob_data *md, int tick);

/*==========================================
 * Mob is searched with a name.
 *------------------------------------------
 */
int mobdb_searchname(const char *str)
{
    int i;

    for (i = 0; i < sizeof(mob_db) / sizeof(mob_db[0]); i++)
    {
        if (strcasecmp(mob_db[i].name, str) == 0
            || strcmp(mob_db[i].jname, str) == 0
            || memcmp(mob_db[i].name, str, 24) == 0
            || memcmp(mob_db[i].jname, str, 24) == 0)
            return i;
    }

    return 0;
}

/*==========================================
 * Id Mob is checked.
 *------------------------------------------
 */
int mobdb_checkid(const int id)
{
    if (id <= 0 || id >= (sizeof(mob_db) / sizeof(mob_db[0]))
        || mob_db[id].name[0] == '\0')
        return 0;

    return id;
}

static
void mob_init(struct mob_data *md);

/*==========================================
 * The minimum data set for MOB spawning
 *------------------------------------------
 */
static
int mob_spawn_dataset(struct mob_data *md, const char *mobname, int mob_class)
{
    nullpo_ret(md);

    if (strcmp(mobname, "--en--") == 0)
        memcpy(md->name, mob_db[mob_class].name, 24);
    else if (strcmp(mobname, "--ja--") == 0)
        memcpy(md->name, mob_db[mob_class].jname, 24);
    else
        memcpy(md->name, mobname, 24);

    md->bl.prev = NULL;
    md->bl.next = NULL;
    md->n = 0;
    md->mob_class = mob_class;
    md->bl.id = npc_get_new_npc_id();

    memset(&md->state, 0, sizeof(md->state));
    md->timer = -1;
    md->target_id = 0;
    md->attacked_id = 0;

    mob_init(md);

    return 0;
}

// Mutation values indicate how `valuable' a change to each stat is, XP wise.
// For one 256th of change, we give out that many 1024th fractions of XP change
// (i.e., 1024 means a 100% XP increase for a single point of adjustment, 4 means 100% XP bonus for doubling the value)
static
earray<int, mob_stat, MOB_XP_BONUS> mutation_value //=
{{
    2,                          // MOB_LV
    3,                          // MOB_MAX_HP
    1,                          // MOB_STR
    2,                          // MOB_AGI
    1,                          // MOB_VIT
    0,                          // MOB_INT
    2,                          // MOB_DEX
    2,                          // MOB_LUK
    1,                          // MOB_ATK1
    1,                          // MOB_ATK2
    2,                          // MOB_ADELAY
    2,                          // MOB_DEF
    2,                          // MOB_MDEF
    2,                          // MOB_SPEED
}};

// The mutation scale indicates how far `up' we can go, with 256 indicating 100%  Note that this may stack with multiple
// calls to `mutate'.
static
earray<int, mob_stat, MOB_XP_BONUS> mutation_scale //=
{{
    16,                         // MOB_LV
    256,                        // MOB_MAX_HP
    32,                         // MOB_STR
    48,                         // MOB_AGI
    48,                         // MOB_VIT
    48,                         // MOB_INT
    48,                         // MOB_DEX
    64,                         // MOB_LUK
    48,                         // MOB_ATK1
    48,                         // MOB_ATK2
    80,                         // MOB_ADELAY
    48,                         // MOB_DEF
    48,                         // MOB_MDEF
    80,                         // MOB_SPEED
}};

// The table below indicates the `average' value for each of the statistics, or -1 if there is none.
// This average is used to determine XP modifications for mutations.  The experience point bonus is
// based on mutation_value and mutation_base as follows:
// (1) first, compute the percentage change of the attribute (p0)
// (2) second, determine the absolute stat change
// (3) third, compute the percentage stat change relative to mutation_base (p1)
// (4) fourth, compute the XP mofication based on the smaller of (p0, p1).
static
earray<int, mob_stat, MOB_XP_BONUS> mutation_base //=
{{
    30,                         // MOB_LV
    -1,                         // MOB_MAX_HP
    20,                         // MOB_STR
    20,                         // MOB_AGI
    20,                         // MOB_VIT
    20,                         // MOB_INT
    20,                         // MOB_DEX
    20,                         // MOB_LUK
    -1,                         // MOB_ATK1
    -1,                         // MOB_ATK2
    -1,                         // MOB_ADELAY
    -1,                         // MOB_DEF
    20,                         // MOB_MDEF
    -1,                         // MOB_SPEED
}};

/*========================================
 * Mutates a MOB.  For large `direction' values, calling this multiple times will give bigger XP boni.
 *----------------------------------------
 */
// intensity: positive: strengthen, negative: weaken.  256 = 100%.
static
void mob_mutate(struct mob_data *md, mob_stat stat, int intensity)
{
    int old_stat;
    int new_stat;
    int real_intensity;        // relative intensity
    const int mut_base = mutation_base[stat];
    int sign = 1;

    if (!md || stat >= MOB_XP_BONUS || intensity == 0)
        return;

    while (intensity > mutation_scale[stat])
    {
        mob_mutate(md, stat, mutation_scale[stat]);    // give better XP assignments
        intensity -= mutation_scale[stat];
    }
    while (intensity < -mutation_scale[stat])
    {
        mob_mutate(md, stat, mutation_scale[stat]);    // give better XP assignments
        intensity += mutation_scale[stat];
    }

    if (!intensity)
        return;

    // MOB_ADELAY and MOB_SPEED are special because going DOWN is good here.
    if (stat == MOB_ADELAY || stat == MOB_SPEED)
        sign = -1;

    // Now compute the new stat
    old_stat = md->stats[stat];
    new_stat = old_stat + ((old_stat * sign * intensity) / 256);

    if (new_stat < 0)
        new_stat = 0;

    if (old_stat == 0)
        real_intensity = 0;
    else
        real_intensity = (((new_stat - old_stat) << 8) / old_stat);

    if (mut_base != -1)
    {
        // Now compute the mutation intensity relative to an absolute value.
        // Take the lesser of the two effects.
        int real_intensity2 = (((new_stat - old_stat) << 8) / mut_base);

        if (real_intensity < 0)
            if (real_intensity2 > real_intensity)
                real_intensity = real_intensity2;

        if (real_intensity > 0)
            if (real_intensity2 < real_intensity)
                real_intensity = real_intensity2;
    }

    real_intensity *= sign;

    md->stats[stat] = new_stat;

    // Adjust XP value
    md->stats[MOB_XP_BONUS] += mutation_value[stat] * real_intensity;
    if (md->stats[MOB_XP_BONUS] <= 0)
        md->stats[MOB_XP_BONUS] = 1;

    // Sanitise
    if (md->stats[MOB_ATK1] > md->stats[MOB_ATK2])
    {
        int swap = md->stats[MOB_ATK2];
        md->stats[MOB_ATK2] = md->stats[MOB_ATK1];
        md->stats[MOB_ATK1] = swap;
    }
}

// This calculates the exp of a given mob
static
int mob_gen_exp(struct mob_db *mob)
{
    if (mob->max_hp <= 1)
        return 1;
    double mod_def = 100 - mob->def;
    if (100 == mob->def)
        mod_def = 1;
    double effective_hp =
        ((50 - mob->attrs[ATTR::LUK]) * mob->max_hp / 50.0) +
        (2 * mob->attrs[ATTR::LUK] * mob->max_hp / mod_def);
    double attack_factor =
        (mob->atk1 + mob->atk2 + mob->attrs[ATTR::STR] / 3.0 + mob->attrs[ATTR::DEX] / 2.0 +
         mob->attrs[ATTR::LUK]) * (1872.0 / mob->adelay) / 4;
    double dodge_factor =
        pow(mob->lv + mob->attrs[ATTR::AGI] + mob->attrs[ATTR::LUK] / 2.0, 4.0 / 3.0);
    // TODO s/persuit/pursuit/g sometime when I'm not worried about diffs
    double persuit_factor =
        (3 + mob->range) * bool(mob->mode & MobMode::CAN_MOVE) * 1000 / mob->speed;
    double aggression_factor =
        bool(mob->mode & MobMode::AGGRESSIVE)
        ? 10.0 / 9.0
        : 1.0;
    int xp =
        (int) floor(effective_hp *
                     pow(sqrt(attack_factor) + sqrt(dodge_factor) +
                          sqrt(persuit_factor) + 55,
                          3) * aggression_factor / 2000000.0 *
                     (double) battle_config.base_exp_rate / 100.);
    if (xp < 1)
        xp = 1;
    PRINTF("Exp for mob '%s' generated: %d\n", mob->name, xp);
    return xp;
}

static
void mob_init(struct mob_data *md)
{
    int i;
    const int mob_class = md->mob_class;
    const int mutations_nr = mob_db[mob_class].mutations_nr;
    const int mutation_power = mob_db[mob_class].mutation_power;

    md->stats[MOB_LV] = mob_db[mob_class].lv;
    md->stats[MOB_MAX_HP] = mob_db[mob_class].max_hp;
    md->stats[MOB_STR] = mob_db[mob_class].attrs[ATTR::STR];
    md->stats[MOB_AGI] = mob_db[mob_class].attrs[ATTR::AGI];
    md->stats[MOB_VIT] = mob_db[mob_class].attrs[ATTR::VIT];
    md->stats[MOB_INT] = mob_db[mob_class].attrs[ATTR::INT];
    md->stats[MOB_DEX] = mob_db[mob_class].attrs[ATTR::DEX];
    md->stats[MOB_LUK] = mob_db[mob_class].attrs[ATTR::LUK];
    md->stats[MOB_ATK1] = mob_db[mob_class].atk1;
    md->stats[MOB_ATK2] = mob_db[mob_class].atk2;
    md->stats[MOB_ADELAY] = mob_db[mob_class].adelay;
    md->stats[MOB_DEF] = mob_db[mob_class].def;
    md->stats[MOB_MDEF] = mob_db[mob_class].mdef;
    md->stats[MOB_SPEED] = mob_db[mob_class].speed;
    md->stats[MOB_XP_BONUS] = MOB_XP_BONUS_BASE;

    for (i = 0; i < mutations_nr; i++)
    {
        mob_stat stat_nr = mob_stat(MRAND(unsigned(MOB_LAST)));
        int strength;

        // double chance to modify hp
        if (stat_nr == MOB_XP_BONUS)
            stat_nr = MOB_MAX_HP;

        strength =
            ((MRAND((mutation_power >> 1)) +
              (MRAND((mutation_power >> 1))) +
              2) * mutation_scale[stat_nr]) / 100;

        strength = MRAND(2) ? strength : -strength;

        if (strength < -240)
            strength = -240;    /* Don't go too close to zero */

        mob_mutate(md, stat_nr, strength);
    }
}

/*==========================================
 * The MOB appearance for one time (for scripts)
 *------------------------------------------
 */
int mob_once_spawn(struct map_session_data *sd, const char *mapname,
                    int x, int y, const char *mobname, int mob_class, int amount,
                    const char *event)
{
    struct mob_data *md = NULL;
    int m, count, lv = 255, r = mob_class;

    if (sd)
        lv = sd->status.base_level;

    if (sd && strcmp(mapname, "this") == 0)
        m = sd->bl.m;
    else
        m = map_mapname2mapid(mapname);

    if (m < 0 || amount <= 0 || (mob_class >= 0 && mob_class <= 1000) || mob_class > 2000)  // 値が異常なら召喚を止める
        return 0;

    if (mob_class < 0)
    {                           // ランダムに召喚
        int i = 0;
        int j = -mob_class - 1;
        int k;
        if (j >= 0 && j < MAX_RANDOMMONSTER)
        {
            do
            {
                mob_class = MPRAND(1001, 1000);
                k = MRAND(1000000);
            }
            while ((mob_db[mob_class].max_hp <= 0
                    || mob_db[mob_class].summonper[j] <= k
                    || (lv < mob_db[mob_class].lv
                        && battle_config.random_monster_checklv == 1))
                   && (i++) < 2000);
            if (i >= 2000)
            {
                mob_class = mob_db[0].summonper[j];
            }
        }
        else
        {
            return 0;
        }
//      if(battle_config.etc_log==1)
//          PRINTF("mobmob_class=%d try=%d\n",mob_class,i);
    }
    if (sd)
    {
        if (x <= 0)
            x = sd->bl.x;
        if (y <= 0)
            y = sd->bl.y;
    }
    else if (x <= 0 || y <= 0)
    {
        PRINTF("mob_once_spawn: ??\n");
    }

    for (count = 0; count < amount; count++)
    {
        md = (struct mob_data *) calloc(1, sizeof(struct mob_data));
        if (bool(mob_db[mob_class].mode & MobMode::LOOTER))
            md->lootitem =
                (struct item *) calloc(LOOTITEM_SIZE, sizeof(struct item));
        else
            md->lootitem = NULL;

        mob_spawn_dataset(md, mobname, mob_class);
        md->bl.m = m;
        md->bl.x = x;
        md->bl.y = y;
        if (r < 0 && battle_config.dead_branch_active == 1)
            //移動してアクティブで反撃する
            md->mode = MobMode::war;
        md->m = m;
        md->x0 = x;
        md->y0 = y;
        md->xs = 0;
        md->ys = 0;
        md->spawndelay1 = -1;   // Only once is a flag.
        md->spawndelay2 = -1;   // Only once is a flag.

        memcpy(md->npc_event, event, sizeof(md->npc_event));

        md->bl.type = BL_MOB;
        map_addiddb(&md->bl);
        mob_spawn(md->bl.id);
    }
    return (amount > 0) ? md->bl.id : 0;
}

/*==========================================
 * The MOB appearance for one time (& area specification for scripts)
 *------------------------------------------
 */
int mob_once_spawn_area(struct map_session_data *sd, const char *mapname,
                         int x0, int y0, int x1, int y1,
                         const char *mobname, int mob_class, int amount,
                         const char *event)
{
    int x, y, i, c, max, lx = -1, ly = -1, id = 0;
    int m;

    if (strcmp(mapname, "this") == 0)
        m = sd->bl.m;
    else
        m = map_mapname2mapid(mapname);

    max = (y1 - y0 + 1) * (x1 - x0 + 1) * 3;
    if (max > 1000)
        max = 1000;

    if (m < 0 || amount <= 0 || (mob_class >= 0 && mob_class <= 1000) || mob_class > 2000)  // A summon is stopped if a value is unusual
        return 0;

    for (i = 0; i < amount; i++)
    {
        int j = 0;
        do
        {
            x = MPRAND(x0, (x1 - x0 + 1));
            y = MPRAND(y0, (y1 - y0 + 1));
        }
        while (((c = map_getcell(m, x, y)) == 1 || c == 5) && (++j) < max);
        if (j >= max)
        {
            if (lx >= 0)
            {                   // Since reference went wrong, the place which boiled before is used.
                x = lx;
                y = ly;
            }
            else
                return 0;       // Since reference of the place which boils first went wrong, it stops.
        }
        id = mob_once_spawn(sd, mapname, x, y, mobname, mob_class, 1, event);
        lx = x;
        ly = y;
    }
    return id;
}

/*==========================================
 * Summoning Guardians [Valaris]
 *------------------------------------------
 */
int mob_spawn_guardian(struct map_session_data *sd, const char *mapname,
                        int x, int y, const char *mobname, int mob_class,
                        int amount, const char *event, int)
{
    struct mob_data *md = NULL;
    int m, count = 1;

    if (sd && strcmp(mapname, "this") == 0)
        m = sd->bl.m;
    else
        m = map_mapname2mapid(mapname);

    if (m < 0 || amount <= 0 || (mob_class >= 0 && mob_class <= 1000) || mob_class > 2000)  // 値が異常なら召喚を止める
        return 0;

    if (mob_class < 0)
        return 0;

    if (sd)
    {
        if (x <= 0)
            x = sd->bl.x;
        if (y <= 0)
            y = sd->bl.y;
    }

    else if (x <= 0 || y <= 0)
        PRINTF("mob_spawn_guardian: ??\n");

    for (count = 0; count < amount; count++)
    {
        CREATE(md, struct mob_data, 1);

        mob_spawn_dataset(md, mobname, mob_class);
        md->bl.m = m;
        md->bl.x = x;
        md->bl.y = y;
        md->m = m;
        md->x0 = x;
        md->y0 = y;
        md->xs = 0;
        md->ys = 0;
        md->spawndelay1 = -1;   // Only once is a flag.
        md->spawndelay2 = -1;   // Only once is a flag.

        memcpy(md->npc_event, event, sizeof(md->npc_event));

        md->bl.type = BL_MOB;
        map_addiddb(&md->bl);
        mob_spawn(md->bl.id);
    }

    return (amount > 0) ? md->bl.id : 0;
}

// TODO: deprecate these
int mob_get_sex(int mob_class)
{
    return mob_db[mob_class].sex;
}

short mob_get_hair(int mob_class)
{
    return mob_db[mob_class].hair;
}

short mob_get_hair_color(int mob_class)
{
    return mob_db[mob_class].hair_color;
}

short mob_get_weapon(int mob_class)
{
    return mob_db[mob_class].weapon;
}

short mob_get_shield(int mob_class)
{
    return mob_db[mob_class].shield;
}

short mob_get_head_top(int mob_class)
{
    return mob_db[mob_class].head_top;
}

short mob_get_head_mid(int mob_class)
{
    return mob_db[mob_class].head_mid;
}

short mob_get_head_buttom(int mob_class)
{
    return mob_db[mob_class].head_buttom;
}

short mob_get_clothes_color(int mob_class) // Add for player monster dye - Valaris
{
    return mob_db[mob_class].clothes_color; // End
}

int mob_get_equip(int mob_class)   // mob equip [Valaris]
{
    return mob_db[mob_class].equip;
}

/*==========================================
 * Is MOB in the state in which the present movement is possible or not?
 *------------------------------------------
 */
static
int mob_can_move(struct mob_data *md)
{
    nullpo_ret(md);

    if (md->canmove_tick > gettick()
        || (bool(md->opt1) && md->opt1 != Opt1::_stone6)
        || bool(md->option & Option::HIDE2))
        return 0;
    // アンクル中で動けないとか
    if (md->sc_data[SC_ANKLE].timer != -1 ||    //アンクルスネア
        md->sc_data[SC_SPIDERWEB].timer != -1   //スパイダーウェッブ
        )
        return 0;

    return 1;
}

/*==========================================
 * Time calculation concerning one step next to mob
 *------------------------------------------
 */
static
int calc_next_walk_step(struct mob_data *md)
{
    nullpo_ret(md);

    if (md->walkpath.path_pos >= md->walkpath.path_len)
        return -1;
    if (md->walkpath.path[md->walkpath.path_pos] & 1)
        return battle_get_speed(&md->bl) * 14 / 10;
    return battle_get_speed(&md->bl);
}

static
int mob_walktoxy_sub(struct mob_data *md);

/*==========================================
 * Mob Walk processing
 *------------------------------------------
 */
static
int mob_walk(struct mob_data *md, unsigned int tick, int data)
{
    int moveblock;
    int i, ctype;
    static int dirx[8] = { 0, -1, -1, -1, 0, 1, 1, 1 };
    static int diry[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };
    int x, y, dx, dy;

    nullpo_ret(md);

    md->state.state = MS_IDLE;
    if (md->walkpath.path_pos >= md->walkpath.path_len
        || md->walkpath.path_pos != data)
        return 0;

    md->walkpath.path_half ^= 1;
    if (md->walkpath.path_half == 0)
    {
        md->walkpath.path_pos++;
        if (md->state.change_walk_target)
        {
            mob_walktoxy_sub(md);
            return 0;
        }
    }
    else
    {
        if (md->walkpath.path[md->walkpath.path_pos] >= 8)
            return 1;

        x = md->bl.x;
        y = md->bl.y;
        ctype = map_getcell(md->bl.m, x, y);
        if (ctype == 1 || ctype == 5)
        {
            mob_stop_walking(md, 1);
            return 0;
        }
        md->dir = md->walkpath.path[md->walkpath.path_pos];
        dx = dirx[md->dir];
        dy = diry[md->dir];

        ctype = map_getcell(md->bl.m, x + dx, y + dy);
        if (ctype == 1 || ctype == 5)
        {
            mob_walktoxy_sub(md);
            return 0;
        }

        moveblock = (x / BLOCK_SIZE != (x + dx) / BLOCK_SIZE
                     || y / BLOCK_SIZE != (y + dy) / BLOCK_SIZE);

        md->state.state = MS_WALK;
        map_foreachinmovearea(std::bind(clif_moboutsight, ph::_1, md),
                md->bl.m, x - AREA_SIZE, y - AREA_SIZE,
                x + AREA_SIZE, y + AREA_SIZE,
                dx, dy, BL_PC);

        x += dx;
        y += dy;
        if (md->min_chase > 13)
            md->min_chase--;

        if (moveblock)
            map_delblock(&md->bl);
        md->bl.x = x;
        md->bl.y = y;
        if (moveblock)
            map_addblock(&md->bl);

        map_foreachinmovearea(std::bind(clif_mobinsight, ph::_1, md),
                md->bl.m, x - AREA_SIZE, y - AREA_SIZE,
                x + AREA_SIZE, y + AREA_SIZE,
                -dx, -dy, BL_PC);
        md->state.state = MS_IDLE;

        if (bool(md->option & Option::CLOAK))
            skill_check_cloaking(&md->bl);

        skill_unit_move(&md->bl, tick, 1); // Inspection of a skill unit
    }
    if ((i = calc_next_walk_step(md)) > 0)
    {
        i = i >> 1;
        if (i < 1 && md->walkpath.path_half == 0)
            i = 1;
        md->timer =
            add_timer(tick + i, mob_timer, md->bl.id, md->walkpath.path_pos);
        md->state.state = MS_WALK;

        if (md->walkpath.path_pos >= md->walkpath.path_len)
            clif_fixmobpos(md);    // When mob stops, retransmission current of a position.
    }
    return 0;
}

/*==========================================
 * Check if mob should be attempting to attack
 *------------------------------------------
 */
static
int mob_check_attack(struct mob_data *md)
{
    struct block_list *tbl = NULL;
    struct map_session_data *tsd = NULL;
    struct mob_data *tmd = NULL;

    MobMode mode;
    int race, range;

    nullpo_ret(md);

    md->min_chase = 13;
    md->state.state = MS_IDLE;
    md->state.skillstate = MSS_IDLE;

    if (md->skilltimer != -1)
        return 0;

    if (bool(md->opt1)
        || bool(md->option & Option::HIDE2))
        return 0;

    if ((tbl = map_id2bl(md->target_id)) == NULL)
    {
        md->target_id = 0;
        md->state.attackable = false;
        return 0;
    }

    if (tbl->type == BL_PC)
        tsd = (struct map_session_data *) tbl;
    else if (tbl->type == BL_MOB)
        tmd = (struct mob_data *) tbl;
    else
        return 0;

    if (tsd)
    {
        if (pc_isdead(tsd) || tsd->invincible_timer != -1
            || pc_isinvisible(tsd) || md->bl.m != tbl->m || tbl->prev == NULL
            || distance(md->bl.x, md->bl.y, tbl->x, tbl->y) >= 13)
        {
            md->target_id = 0;
            md->state.attackable = false;
            return 0;
        }
    }
    if (tmd)
    {
        if (md->bl.m != tbl->m || tbl->prev == NULL
            || distance(md->bl.x, md->bl.y, tbl->x, tbl->y) >= 13)
        {
            md->target_id = 0;
            md->state.attackable = false;
            return 0;
        }
    }

    if (md->mode == MobMode::ZERO)
        mode = mob_db[md->mob_class].mode;
    else
        mode = md->mode;

    race = mob_db[md->mob_class].race;
    if (!bool(mode & MobMode::CAN_ATTACK))
    {
        md->target_id = 0;
        md->state.attackable = false;
        return 0;
    }
    if (tsd
        && !bool(mode & MobMode::BOSS)
        && (tsd->sc_data[SC_TRICKDEAD].timer != -1
            || ((pc_ishiding(tsd) || tsd->state.gangsterparadise)
                && race != 4 && race != 6)))
    {
        md->target_id = 0;
        md->state.attackable = false;
        return 0;
    }

    range = mob_db[md->mob_class].range;
    if (bool(mode & MobMode::CAN_MOVE))
        range++;
    if (distance(md->bl.x, md->bl.y, tbl->x, tbl->y) > range)
        return 0;

    return 1;
}

static
void mob_ancillary_attack(struct block_list *bl,
        struct block_list *mdbl, struct block_list *tbl, unsigned int tick)
{
    if (bl != tbl)
        battle_weapon_attack(mdbl, bl, tick, BCT_ZERO);
}

/*==========================================
 * Attack processing of mob
 *------------------------------------------
 */
static
int mob_attack(struct mob_data *md, unsigned int tick, int)
{
    struct block_list *tbl = NULL;

    nullpo_ret(md);

    if ((tbl = map_id2bl(md->target_id)) == NULL)
        return 0;

    if (!mob_check_attack(md))
        return 0;

    if (battle_config.monster_attack_direction_change)
        md->dir = map_calc_dir(&md->bl, tbl->x, tbl->y);   // 向き設定

    //clif_fixmobpos(md);

    md->state.skillstate = MSS_ATTACK;
    if (mobskill_use(md, tick, MSC::NEVER_EQUAL))
        return 0;

    md->target_lv = battle_weapon_attack(&md->bl, tbl, tick, BCT_ZERO);
    // If you are reading this, please note:
    // it is highly platform-specific that this even works at all.
    int radius = battle_config.mob_splash_radius;
    if (radius >= 0 && tbl->type == BL_PC && !map[tbl->m].flag.town)
        map_foreachinarea(std::bind(mob_ancillary_attack, ph::_1, &md->bl, tbl, tick),
            tbl->m, tbl->x - radius, tbl->y - radius,
            tbl->x + radius, tbl->y + radius, BL_PC);

    if (!(battle_config.monster_cloak_check_type & 2)
        && md->sc_data[SC_CLOAKING].timer != -1)
        skill_status_change_end(&md->bl, SC_CLOAKING, -1);

    md->attackabletime = tick + battle_get_adelay(&md->bl);

    md->timer = add_timer(md->attackabletime, mob_timer, md->bl.id, 0);
    md->state.state = MS_ATTACK;

    return 0;
}

/*==========================================
 * The attack of PC which is attacking id is stopped.
 * The callback function of clif_foreachclient
 *------------------------------------------
 */
static
void mob_stopattacked(struct map_session_data *sd, int id)
{
    nullpo_retv(sd);

    if (sd->attacktarget == id)
        pc_stopattack(sd);
}

/*==========================================
 * The timer in which the mob's states changes
 *------------------------------------------
 */
int mob_changestate(struct mob_data *md, MS state, int type)
{
    unsigned int tick;
    int i;

    nullpo_ret(md);

    if (md->timer != -1)
        delete_timer(md->timer, mob_timer);
    md->timer = -1;
    md->state.state = state;

    switch (state)
    {
        case MS_WALK:
            if ((i = calc_next_walk_step(md)) > 0)
            {
                i = i >> 2;
                md->timer =
                    add_timer(gettick() + i, mob_timer, md->bl.id, 0);
            }
            else
                md->state.state = MS_IDLE;
            break;
        case MS_ATTACK:
            tick = gettick();
            i = DIFF_TICK(md->attackabletime, tick);
            if (i > 0 && i < 2000)
                md->timer =
                    add_timer(md->attackabletime, mob_timer, md->bl.id, 0);
            else if (type)
            {
                md->attackabletime = tick + battle_get_amotion(&md->bl);
                md->timer =
                    add_timer(md->attackabletime, mob_timer, md->bl.id, 0);
            }
            else
            {
                md->attackabletime = tick + 1;
                md->timer =
                    add_timer(md->attackabletime, mob_timer, md->bl.id, 0);
            }
            break;
        case MS_DELAY:
            md->timer =
                add_timer(gettick() + type, mob_timer, md->bl.id, 0);
            break;
        case MS_DEAD:
            skill_castcancel(&md->bl, 0);
//      mobskill_deltimer(md);
            md->state.skillstate = MSS_DEAD;
            md->last_deadtime = gettick();
            // Since it died, all aggressors' attack to this mob is stopped.
            clif_foreachclient(std::bind(mob_stopattacked, ph::_1, md->bl.id));
            skill_unit_out_all(&md->bl, gettick(), 1);
            skill_status_change_clear(&md->bl, 2); // The abnormalities in status are canceled.
            skill_clear_unitgroup(&md->bl);    // All skill unit groups are deleted.
            skill_cleartimerskill(&md->bl);
            if (md->deletetimer != -1)
                delete_timer(md->deletetimer, mob_timer_delete);
            md->deletetimer = -1;
            md->hp = md->target_id = md->attacked_id = 0;
            md->state.attackable = false;
            break;
    }

    return 0;
}

/*==========================================
 * timer processing of mob (timer function)
 * It branches to a walk and an attack.
 *------------------------------------------
 */
static
void mob_timer(timer_id tid, tick_t tick, custom_id_t id, custom_data_t data)
{
    struct mob_data *md;
    struct block_list *bl;

    if ((bl = map_id2bl(id)) == NULL)
    {                           //攻撃してきた敵がもういないのは正常のようだ
        return;
    }

    if (!bl || bl->type == BL_NUL || bl->type != BL_MOB)
        return;

    nullpo_retv(md = (struct mob_data *) bl);

    if (md->bl.type == BL_NUL || md->bl.type != BL_MOB)
        return;

    if (md->timer != tid)
    {
        if (battle_config.error_log == 1)
            PRINTF("mob_timer %d != %d\n", md->timer, tid);
        return;
    }
    md->timer = -1;
    if (md->bl.prev == NULL || md->state.state == MS_DEAD)
        return;

    map_freeblock_lock();
    switch (md->state.state)
    {
        case MS_WALK:
            mob_check_attack(md);
            mob_walk(md, tick, data);
            break;
        case MS_ATTACK:
            mob_attack(md, tick, data);
            break;
        case MS_DELAY:
            mob_changestate(md, MS_IDLE, 0);
            break;
        default:
            if (battle_config.error_log == 1)
                PRINTF("mob_timer : %d ?\n",
                        md->state.state);
            break;
    }
    map_freeblock_unlock();
    return;
}

/*==========================================
 *
 *------------------------------------------
 */
static
int mob_walktoxy_sub(struct mob_data *md)
{
    struct walkpath_data wpd;

    nullpo_ret(md);

    if (path_search(&wpd, md->bl.m, md->bl.x, md->bl.y, md->to_x, md->to_y,
         md->state.walk_easy))
        return 1;
    memcpy(&md->walkpath, &wpd, sizeof(wpd));

    md->state.change_walk_target = 0;
    mob_changestate(md, MS_WALK, 0);
    clif_movemob(md);

    return 0;
}

/*==========================================
 * mob move start
 *------------------------------------------
 */
static
int mob_walktoxy(struct mob_data *md, int x, int y, int easy)
{
    struct walkpath_data wpd;

    nullpo_ret(md);

    if (md->state.state == MS_WALK
        && path_search(&wpd, md->bl.m, md->bl.x, md->bl.y, x, y, easy))
        return 1;

    md->state.walk_easy = easy;
    md->to_x = x;
    md->to_y = y;
    if (md->state.state == MS_WALK)
    {
        md->state.change_walk_target = 1;
    }
    else
    {
        return mob_walktoxy_sub(md);
    }

    return 0;
}

/*==========================================
 * mob spawn with delay (timer function)
 *------------------------------------------
 */
static
void mob_delayspawn(timer_id, tick_t, custom_id_t m, custom_data_t)
{
    mob_spawn(m);
}

/*==========================================
 * spawn timing calculation
 *------------------------------------------
 */
static
int mob_setdelayspawn(int id)
{
    unsigned int spawntime, spawntime1, spawntime2, spawntime3;
    struct mob_data *md;
    struct block_list *bl;

    if ((bl = map_id2bl(id)) == NULL)
        return -1;

    if (!bl || bl->type == BL_NUL || bl->type != BL_MOB)
        return -1;

    nullpo_retr(-1, md = (struct mob_data *) bl);

    if (!md || md->bl.type != BL_MOB)
        return -1;

    // Processing of MOB which is not revitalized
    if (md->spawndelay1 == -1 && md->spawndelay2 == -1 && md->n == 0)
    {
        map_deliddb(&md->bl);
        if (md->lootitem)
        {
            map_freeblock(md->lootitem);
            md->lootitem = NULL;
        }
        map_freeblock(md);     // Instead of [ of free ]
        return 0;
    }

    spawntime1 = md->last_spawntime + md->spawndelay1;
    spawntime2 = md->last_deadtime + md->spawndelay2;
    spawntime3 = gettick() + 5000;
    // spawntime = max(spawntime1,spawntime2,spawntime3);
    if (DIFF_TICK(spawntime1, spawntime2) > 0)
    {
        spawntime = spawntime1;
    }
    else
    {
        spawntime = spawntime2;
    }
    if (DIFF_TICK(spawntime3, spawntime) > 0)
    {
        spawntime = spawntime3;
    }

    add_timer(spawntime, mob_delayspawn, id, 0);
    return 0;
}

/*==========================================
 * Mob spawning. Initialization is also variously here.
 *------------------------------------------
 */
int mob_spawn(int id)
{
    int x = 0, y = 0, c;
    unsigned int tick = gettick();
    struct mob_data *md;
    struct block_list *bl;

    nullpo_retr(-1, bl = map_id2bl(id));

    if (!bl || bl->type == BL_NUL || bl->type != BL_MOB)
        return -1;

    nullpo_retr(-1, md = (struct mob_data *) bl);

    if (!md || md->bl.type == BL_NUL || md->bl.type != BL_MOB)
        return -1;

    md->last_spawntime = tick;
    if (md->bl.prev != NULL)
    {
//      clif_clearchar_area(&md->bl,3);
        skill_unit_out_all(&md->bl, gettick(), 1);
        map_delblock(&md->bl);
    }

    md->bl.m = md->m;
    {
        int i = 0;
        do
        {
            if (md->x0 == 0 && md->y0 == 0)
            {
                x = MPRAND(1, (map[md->bl.m].xs - 2));
                y = MPRAND(1, (map[md->bl.m].ys - 2));
            }
            else
            {
                x = MPRAND(md->x0, (md->xs + 1)) - md->xs / 2;
                y = MPRAND(md->y0, (md->ys + 1)) - md->ys / 2;
            }
            i++;
        }
        while (((c = map_getcell(md->bl.m, x, y)) == 1 || c == 5) && i < 50);

        if (i >= 50)
        {
    //      if(battle_config.error_log==1)
    //          PRINTF("MOB spawn error %d @ %s\n",id,map[md->bl.m].name);
            add_timer(tick + 5000, mob_delayspawn, id, 0);
            return 1;
        }
    }

    md->to_x = md->bl.x = x;
    md->to_y = md->bl.y = y;
    md->dir = 0;

    map_addblock(&md->bl);

    memset(&md->state, 0, sizeof(md->state));
    md->attacked_id = 0;
    md->target_id = 0;
    md->move_fail_count = 0;
    mob_init(md);

    if (!md->stats[MOB_SPEED])
        md->stats[MOB_SPEED] = mob_db[md->mob_class].speed;
    md->def_ele = mob_db[md->mob_class].element;
    md->master_id = 0;
    md->master_dist = 0;

    md->state.state = MS_IDLE;
    md->state.skillstate = MSS_IDLE;
    md->timer = -1;
    md->last_thinktime = tick;
    md->next_walktime = tick + MPRAND(5000, 50);
    md->attackabletime = tick;
    md->canmove_tick = tick;

    md->sg_count = 0;
    md->deletetimer = -1;

    md->skilltimer = -1;
    for (int i = 0; i < MAX_MOBSKILL; i++)
        md->skilldelay[i] = tick - 1000 * 3600 * 10;
    md->skillid = SkillID();
    md->skilllv = 0;

    memset(md->dmglog, 0, sizeof(md->dmglog));
    if (md->lootitem)
        memset(md->lootitem, 0, sizeof(*md->lootitem));
    md->lootitem_count = 0;

    for (int i = 0; i < MAX_MOBSKILLTIMERSKILL; i++)
        md->skilltimerskill[i].timer = -1;

    for (StatusChange i : erange(StatusChange(), MAX_STATUSCHANGE))
    {
        md->sc_data[i].timer = -1;
        md->sc_data[i].val1 = md->sc_data[i].val2 = md->sc_data[i].val3 =
            md->sc_data[i].val4 = 0;
    }
    md->sc_count = 0;
    md->opt1 = Opt1::ZERO;
    md->opt2 = Opt2::ZERO;
    md->opt3 = Opt3::ZERO;
    md->option = Option::ZERO;

    memset(md->skillunit, 0, sizeof(md->skillunit));
    memset(md->skillunittick, 0, sizeof(md->skillunittick));

    md->hp = battle_get_max_hp(&md->bl);
    if (md->hp <= 0)
    {
        mob_makedummymobdb(md->mob_class);
        md->hp = battle_get_max_hp(&md->bl);
    }

    clif_spawnmob(md);

    return 0;
}

/*==========================================
 * Distance calculation between two points
 *------------------------------------------
 */
static
int distance(int x0, int y0, int x1, int y1)
{
    int dx, dy;

    dx = abs(x0 - x1);
    dy = abs(y0 - y1);
    return dx > dy ? dx : dy;
}

/*==========================================
 * The stop of MOB's attack
 *------------------------------------------
 */
int mob_stopattack(struct mob_data *md)
{
    md->target_id = 0;
    md->state.attackable = false;
    md->attacked_id = 0;
    return 0;
}

/*==========================================
 * The stop of MOB's walking
 *------------------------------------------
 */
int mob_stop_walking(struct mob_data *md, int type)
{
    nullpo_ret(md);

    if (md->state.state == MS_WALK || md->state.state == MS_IDLE)
    {
        int dx = 0, dy = 0;

        md->walkpath.path_len = 0;
        if (type & 4)
        {
            dx = md->to_x - md->bl.x;
            if (dx < 0)
                dx = -1;
            else if (dx > 0)
                dx = 1;
            dy = md->to_y - md->bl.y;
            if (dy < 0)
                dy = -1;
            else if (dy > 0)
                dy = 1;
        }
        md->to_x = md->bl.x + dx;
        md->to_y = md->bl.y + dy;
        if (dx != 0 || dy != 0)
        {
            mob_walktoxy_sub(md);
            return 0;
        }
        mob_changestate(md, MS_IDLE, 0);
    }
    if (type & 0x01)
        clif_fixmobpos(md);
    if (type & 0x02)
    {
        int delay = battle_get_dmotion(&md->bl);
        unsigned int tick = gettick();
        if (md->canmove_tick < tick)
            md->canmove_tick = tick + delay;
    }

    return 0;
}

/*==========================================
 * Reachability to a Specification ID existence place
 *------------------------------------------
 */
static
int mob_can_reach(struct mob_data *md, struct block_list *bl, int range)
{
    int dx, dy;
    struct walkpath_data wpd;
    int i;

    nullpo_ret(md);
    nullpo_ret(bl);

    dx = abs(bl->x - md->bl.x);
    dy = abs(bl->y - md->bl.y);

    if (bl && bl->type == BL_PC && battle_config.monsters_ignore_gm == 1)
    {                           // option to have monsters ignore GMs [Valaris]
        struct map_session_data *sd;
        if ((sd = (struct map_session_data *) bl) != NULL && pc_isGM(sd))
            return 0;
    }

    if (md->bl.m != bl->m)      // 違うャbプ
        return 0;

    if (range > 0 && range < ((dx > dy) ? dx : dy)) // 遠すぎる
        return 0;

    if (md->bl.x == bl->x && md->bl.y == bl->y) // 同じャX
        return 1;

    // Obstacle judging
    wpd.path_len = 0;
    wpd.path_pos = 0;
    wpd.path_half = 0;
    if (path_search(&wpd, md->bl.m, md->bl.x, md->bl.y, bl->x, bl->y, 0) !=
        -1)
        return 1;

    if (bl->type != BL_PC && bl->type != BL_MOB)
        return 0;

    // It judges whether it can adjoin or not.
    dx = (dx > 0) ? 1 : ((dx < 0) ? -1 : 0);
    dy = (dy > 0) ? 1 : ((dy < 0) ? -1 : 0);
    if (path_search(&wpd, md->bl.m, md->bl.x, md->bl.y, bl->x - dx, bl->y - dy, 0) != -1)
        return 1;
    for (i = 0; i < 9; i++)
    {
        if (path_search(&wpd, md->bl.m, md->bl.x, md->bl.y, bl->x - 1 + i / 3,
             bl->y - 1 + i % 3, 0) != -1)
            return 1;
    }
    return 0;
}

/*==========================================
 * Determination for an attack of a monster
 *------------------------------------------
 */
int mob_target(struct mob_data *md, struct block_list *bl, int dist)
{
    struct map_session_data *sd;
    eptr<struct status_change, StatusChange> sc_data;
    MobMode mode;
    int race;

    nullpo_ret(md);
    nullpo_ret(bl);

    sc_data = battle_get_sc_data(bl);
    Option *option = battle_get_option(bl);
    race = mob_db[md->mob_class].race;

    if (md->mode == MobMode::ZERO)
    {
        mode = mob_db[md->mob_class].mode;
    }
    else
    {
        mode = md->mode;
    }
    if (!bool(mode & MobMode::CAN_ATTACK))
    {
        md->target_id = 0;
        return 0;
    }
    // Nothing will be carried out if there is no mind of changing TAGE by TAGE ending.
    if ((md->target_id > 0 && md->state.attackable)
        && (!bool(mode & MobMode::AGGRESSIVE) || MRAND(100) > 25))
        return 0;

    // Coercion is exerted if it is MVPMOB.
    if (bool(mode & MobMode::BOSS)
        || (sc_data && sc_data[SC_TRICKDEAD].timer == -1
            && ((option != NULL && !bool(*option & (Option::CLOAK | Option::HIDE2)))
                || race == 4
                || race == 6)))
    {
        if (bl->type == BL_PC)
        {
            nullpo_ret(sd = (struct map_session_data *) bl);
            if (sd->invincible_timer != -1 || pc_isinvisible(sd))
                return 0;
            if (!bool(mode & MobMode::BOSS) && race != 4 && race != 6
                && sd->state.gangsterparadise)
                return 0;
        }

        md->target_id = bl->id; // Since there was no disturbance, it locks on to target.
        if (bl->type == BL_PC || bl->type == BL_MOB)
            md->state.attackable = true;
        else
            md->state.attackable = false;
        md->min_chase = dist + 13;
        if (md->min_chase > 26)
            md->min_chase = 26;
    }
    return 0;
}

/*==========================================
 * The ?? routine of an active monster
 *------------------------------------------
 */
static
void mob_ai_sub_hard_activesearch(struct block_list *bl,
        struct mob_data *smd, int *pcc)
{
    struct map_session_data *tsd = NULL;
    struct mob_data *tmd = NULL;
    MobMode mode;
    int race, dist;

    nullpo_retv(bl);
    nullpo_retv(smd);
    nullpo_retv(pcc);

    if (bl->type == BL_PC)
        tsd = (struct map_session_data *) bl;
    else if (bl->type == BL_MOB)
        tmd = (struct mob_data *) bl;
    else
        return;

    //敵味方判定
    if (battle_check_target(&smd->bl, bl, BCT_ENEMY) == 0)
        return;

    if (smd->mode == MobMode::ZERO)
        mode = mob_db[smd->mob_class].mode;
    else
        mode = smd->mode;

    // アクティブでターゲット射程内にいるなら、ロックする
    if (bool(mode & MobMode::AGGRESSIVE))
    {
        race = mob_db[smd->mob_class].race;
        //対象がPCの場合
        if (tsd &&
            !pc_isdead(tsd) &&
            tsd->bl.m == smd->bl.m &&
            tsd->invincible_timer == -1 &&
            !pc_isinvisible(tsd) &&
            (dist =
             distance(smd->bl.x, smd->bl.y, tsd->bl.x, tsd->bl.y)) < 9)
        {
            if (bool(mode & MobMode::BOSS)
                || (tsd->sc_data[SC_TRICKDEAD].timer == -1
                    && ((!pc_ishiding(tsd)
                            && !tsd->state.gangsterparadise)
                        || race == 4 || race == 6)))
            {                   // 妨害がないか判定
                if (mob_can_reach(smd, bl, 12) &&  // 到達可能性判定
                    MRAND(1000) < 1000 / (++(*pcc)))
                {               // 範囲内PCで等確率にする
                    smd->target_id = tsd->bl.id;
                    smd->state.attackable = true;
                    smd->min_chase = 13;
                }
            }
        }
        //対象がMobの場合
        else if (tmd &&
                 tmd->bl.m == smd->bl.m &&
                 (dist =
                  distance(smd->bl.x, smd->bl.y, tmd->bl.x, tmd->bl.y)) < 9)
        {
            if (mob_can_reach(smd, bl, 12) &&  // 到達可能性判定
                MRAND(1000) < 1000 / (++(*pcc)))
            {                   // 範囲内で等確率にする
                smd->target_id = bl->id;
                smd->state.attackable = true;
                smd->min_chase = 13;
            }
        }
    }
}

/*==========================================
 * loot monster item search
 *------------------------------------------
 */
static
void mob_ai_sub_hard_lootsearch(struct block_list *bl, struct mob_data *md, int *itc)
{
    MobMode mode;
    int dist;

    nullpo_retv(bl);

    if (md->mode == MobMode::ZERO)
    {
        mode = mob_db[md->mob_class].mode;
    }
    else
    {
        mode = md->mode;
    }

    if (!md->target_id && bool(mode & MobMode::LOOTER))
    {
        if (!md->lootitem
            || (battle_config.monster_loot_type == 1
                && md->lootitem_count >= LOOTITEM_SIZE))
            return;
        if (bl->m == md->bl.m
            && (dist = distance(md->bl.x, md->bl.y, bl->x, bl->y)) < 9)
        {
            if (mob_can_reach(md, bl, 12) &&   // Reachability judging
                MRAND(1000) < 1000 / (++(*itc)))
            {                   // It is made a probability, such as within the limits PC.
                md->target_id = bl->id;
                md->state.attackable = false;
                md->min_chase = 13;
            }
        }
    }
}

/*==========================================
 * The ?? routine of a link monster
 *------------------------------------------
 */
static
void mob_ai_sub_hard_linksearch(struct block_list *bl, struct mob_data *md, struct block_list *target)
{
    struct mob_data *tmd;

    nullpo_retv(bl);
    tmd = (struct mob_data *) bl;
    nullpo_retv(md);
    nullpo_retv(target);

    if (md->attacked_id > 0
        && bool(mob_db[md->mob_class].mode & MobMode::ASSIST))
    {
        if (tmd->mob_class == md->mob_class
            && tmd->bl.m == md->bl.m
            && (!tmd->target_id || !md->state.attackable))
        {
            if (mob_can_reach(tmd, target, 12))
            {
                // Reachability judging
                tmd->target_id = md->attacked_id;
                tmd->state.attackable = true;
                tmd->min_chase = 13;
            }
        }
    }
}

/*==========================================
 * Processing of slave monsters
 *------------------------------------------
 */
static
int mob_ai_sub_hard_slavemob(struct mob_data *md, unsigned int tick)
{
    struct mob_data *mmd = NULL;
    struct block_list *bl;
    MobMode mode;
    int race, old_dist;

    nullpo_ret(md);

    if ((bl = map_id2bl(md->master_id)) != NULL)
        mmd = (struct mob_data *) bl;

    mode = mob_db[md->mob_class].mode;

    // It is not main monster/leader.
    if (!mmd || mmd->bl.type != BL_MOB || mmd->bl.id != md->master_id)
        return 0;

    // Since it is in the map on which the master is not, teleport is carried out and it pursues.
    if (mmd->bl.m != md->bl.m)
    {
        mob_warp(md, mmd->bl.m, mmd->bl.x, mmd->bl.y, 3);
        md->state.master_check = 1;
        return 0;
    }

    // Distance with between slave and master is measured.
    old_dist = md->master_dist;
    md->master_dist = distance(md->bl.x, md->bl.y, mmd->bl.x, mmd->bl.y);

    // Since the master was in near immediately before, teleport is carried out and it pursues.
    if (old_dist < 10 && md->master_dist > 18)
    {
        mob_warp(md, -1, mmd->bl.x, mmd->bl.y, 3);
        md->state.master_check = 1;
        return 0;
    }

    // Although there is the master, since it is somewhat far, it approaches.
    if ((!md->target_id || !md->state.attackable)
        && mob_can_move(md)
        && (md->walkpath.path_pos >= md->walkpath.path_len
            || md->walkpath.path_len == 0) && md->master_dist < 15)
    {
        int i = 0, dx, dy, ret;
        if (md->master_dist > 4)
        {
            do
            {
                if (i <= 5)
                {
                    dx = mmd->bl.x - md->bl.x;
                    dy = mmd->bl.y - md->bl.y;
                    if (dx < 0)
                        dx += (MPRAND(1, ((dx < -3) ? 3 : -dx)));
                    else if (dx > 0)
                        dx -= (MPRAND(1, ((dx > 3) ? 3 : dx)));
                    if (dy < 0)
                        dy += (MPRAND(1, ((dy < -3) ? 3 : -dy)));
                    else if (dy > 0)
                        dy -= (MPRAND(1, ((dy > 3) ? 3 : dy)));
                }
                else
                {
                    dx = mmd->bl.x - md->bl.x + MRAND(7) - 3;
                    dy = mmd->bl.y - md->bl.y + MRAND(7) - 3;
                }

                ret = mob_walktoxy(md, md->bl.x + dx, md->bl.y + dy, 0);
                i++;
            }
            while (ret && i < 10);
        }
        else
        {
            do
            {
                dx = MRAND(9) - 5;
                dy = MRAND(9) - 5;
                if (dx == 0 && dy == 0)
                {
                    dx = (MRAND(1)) ? 1 : -1;
                    dy = (MRAND(1)) ? 1 : -1;
                }
                dx += mmd->bl.x;
                dy += mmd->bl.y;

                ret = mob_walktoxy(md, mmd->bl.x + dx, mmd->bl.y + dy, 0);
                i++;
            }
            while (ret && i < 10);
        }

        md->next_walktime = tick + 500;
        md->state.master_check = 1;
    }

    // There is the master, the master locks a target and he does not lock.
    if ((mmd->target_id > 0 && mmd->state.attackable)
        && (!md->target_id || !md->state.attackable))
    {
        struct map_session_data *sd = map_id2sd(mmd->target_id);
        if (sd != NULL && !pc_isdead(sd) && sd->invincible_timer == -1
            && !pc_isinvisible(sd))
        {

            race = mob_db[md->mob_class].race;
            if (bool(mode & MobMode::BOSS) ||
                (sd->sc_data[SC_TRICKDEAD].timer == -1 &&
                 ((!pc_ishiding(sd) && !sd->state.gangsterparadise)
                  || race == 4 || race == 6)))
            {                   // 妨害がないか判定

                md->target_id = sd->bl.id;
                md->state.attackable = true;
                md->min_chase =
                    5 + distance(md->bl.x, md->bl.y, sd->bl.x, sd->bl.y);
                md->state.master_check = 1;
            }
        }
    }

    // There is the master, the master locks a target and he does not lock.
/*      if ((md->target_id>0 && mmd->state.attackable) && (!mmd->target_id || !mmd->state.attackable) ){
                struct map_session_data *sd=map_id2sd(md->target_id);
                if (sd!=NULL && !pc_isdead(sd) && sd->invincible_timer == -1 && !pc_isinvisible(sd)){

                        race=mob_db[mmd->mob_class].race;
                        if (mode&0x20 ||
                                (sd->sc_data[SC_TRICKDEAD].timer == -1 &&
                                (!(sd->status.option&0x06) || race==4 || race==6)
                                ) ){    // It judges whether there is any disturbance.

                                mmd->target_id=sd->bl.id;
                                mmd->state.attackable = true;
                                mmd->min_chase=5+distance(mmd->bl.x,mmd->bl.y,sd->bl.x,sd->bl.y);
                        }
                }
        }*/

    return 0;
}

/*==========================================
 * A lock of target is stopped and mob moves to a standby state.
 *------------------------------------------
 */
static
int mob_unlocktarget(struct mob_data *md, int tick)
{
    nullpo_ret(md);

    md->target_id = 0;
    md->state.attackable = false;
    md->state.skillstate = MSS_IDLE;
    md->next_walktime = tick + MPRAND(3000, 3000);
    return 0;
}

/*==========================================
 * Random walk
 *------------------------------------------
 */
static
int mob_randomwalk(struct mob_data *md, int tick)
{
    const int retrycount = 20;
    int speed;

    nullpo_ret(md);

    speed = battle_get_speed(&md->bl);
    if (DIFF_TICK(md->next_walktime, tick) < 0)
    {
        int i, x, y, c, d = 12 - md->move_fail_count;
        if (d < 5)
            d = 5;
        for (i = 0; i < retrycount; i++)
        {                       // Search of a movable place
            int r = mt_random();
            x = md->bl.x + r % (d * 2 + 1) - d;
            y = md->bl.y + r / (d * 2 + 1) % (d * 2 + 1) - d;
            if ((c = map_getcell(md->bl.m, x, y)) != 1 && c != 5
                && mob_walktoxy(md, x, y, 1) == 0)
            {
                md->move_fail_count = 0;
                break;
            }
            if (i + 1 >= retrycount)
            {
                md->move_fail_count++;
                if (md->move_fail_count > 1000)
                {
                    if (battle_config.error_log == 1)
                        PRINTF("MOB cant move. random spawn %d, mob_class = %d\n",
                             md->bl.id, md->mob_class);
                    md->move_fail_count = 0;
                    mob_spawn(md->bl.id);
                }
            }
        }
        for (i = c = 0; i < md->walkpath.path_len; i++)
        {                       // The next walk start time is calculated.
            if (md->walkpath.path[i] & 1)
                c += speed * 14 / 10;
            else
                c += speed;
        }
        md->next_walktime = tick + MPRAND(3000, 3000) + c;
        md->state.skillstate = MSS_WALK;
        return 1;
    }
    return 0;
}

/*==========================================
 * AI of MOB whose is near a Player
 *------------------------------------------
 */
static
void mob_ai_sub_hard(struct block_list *bl, unsigned int tick)
{
    struct mob_data *md, *tmd = NULL;
    struct map_session_data *tsd = NULL;
    struct block_list *tbl = NULL;
    struct flooritem_data *fitem;
    int i, dx, dy, ret, dist;
    int attack_type = 0;
    MobMode mode;
    int race;

    nullpo_retv(bl);
    md = (struct mob_data *) bl;

    if (DIFF_TICK(tick, md->last_thinktime) < MIN_MOBTHINKTIME)
        return;
    md->last_thinktime = tick;

    if (md->skilltimer != -1 || md->bl.prev == NULL)
    {                           // Under a skill aria and death
        if (DIFF_TICK(tick, md->next_walktime) > MIN_MOBTHINKTIME)
            md->next_walktime = tick;
        return;
    }

    if (md->mode == MobMode::ZERO)
        mode = mob_db[md->mob_class].mode;
    else
        mode = md->mode;

    race = mob_db[md->mob_class].race;

    // Abnormalities
    if ((bool(md->opt1) && md->opt1 != Opt1::_stone6)
        || md->state.state == MS_DELAY)
        return;

    if (!bool(mode & MobMode::CAN_ATTACK) && md->target_id > 0)
        md->target_id = 0;

    if (md->attacked_id > 0 && bool(mode & MobMode::ASSIST))
    {                           // Link monster
        struct map_session_data *asd = map_id2sd(md->attacked_id);
        if (asd)
        {
            if (asd->invincible_timer == -1 && !pc_isinvisible(asd))
            {
                map_foreachinarea(std::bind(mob_ai_sub_hard_linksearch, ph::_1, md, &asd->bl),
                        md->bl.m, md->bl.x - 13, md->bl.y - 13,
                        md->bl.x + 13, md->bl.y + 13, BL_MOB);
            }
        }
    }

    // It checks to see it was attacked first (if active, it is target change at 25% of probability).
    if (mode != MobMode::ZERO && md->attacked_id > 0
        && (!md->target_id || !md->state.attackable
            || (bool(mode & MobMode::AGGRESSIVE) && MRAND(100) < 25)))
    {
        struct block_list *abl = map_id2bl(md->attacked_id);
        struct map_session_data *asd = NULL;
        if (abl)
        {
            if (abl->type == BL_PC)
                asd = (struct map_session_data *) abl;
            if (asd == NULL || md->bl.m != abl->m || abl->prev == NULL
                || asd->invincible_timer != -1 || pc_isinvisible(asd)
                || (dist =
                    distance(md->bl.x, md->bl.y, abl->x, abl->y)) >= 32
                || battle_check_target(bl, abl, BCT_ENEMY) == 0)
                md->attacked_id = 0;
            else
            {
                md->target_id = md->attacked_id;    // set target
                md->state.attackable = true;
                attack_type = 1;
                md->attacked_id = 0;
                md->min_chase = dist + 13;
                if (md->min_chase > 26)
                    md->min_chase = 26;
            }
        }
    }

    md->state.master_check = 0;
    // Processing of slave monster
    if (md->master_id > 0 && md->state.special_mob_ai == 0)
        mob_ai_sub_hard_slavemob(md, tick);

    // アクティヴモンスターの策敵 (?? of a bitter taste TIVU monster)
    if ((!md->target_id || !md->state.attackable)
        && bool(mode & MobMode::AGGRESSIVE) && !md->state.master_check
        && battle_config.monster_active_enable == 1)
    {
        i = 0;
        if (md->state.special_mob_ai)
        {
            map_foreachinarea(std::bind(mob_ai_sub_hard_activesearch, ph::_1, md, &i),
                    md->bl.m, md->bl.x - AREA_SIZE * 2, md->bl.y - AREA_SIZE * 2,
                    md->bl.x + AREA_SIZE * 2, md->bl.y + AREA_SIZE * 2,
                    BL_NUL);
        }
        else
        {
            map_foreachinarea(std::bind(mob_ai_sub_hard_activesearch, ph::_1, md, &i),
                    md->bl.m, md->bl.x - AREA_SIZE * 2, md->bl.y - AREA_SIZE * 2,
                    md->bl.x + AREA_SIZE * 2, md->bl.y + AREA_SIZE * 2, BL_PC);
        }
    }

    // The item search of a route monster
    if (!md->target_id
        && bool(mode & MobMode::LOOTER)
        && !md->state.master_check)
    {
        i = 0;
        map_foreachinarea(std::bind(mob_ai_sub_hard_lootsearch, ph::_1, md, &i),
                md->bl.m, md->bl.x - AREA_SIZE * 2, md->bl.y - AREA_SIZE * 2,
                md->bl.x + AREA_SIZE * 2, md->bl.y + AREA_SIZE * 2, BL_ITEM);
    }

    // It will attack, if the candidate for an attack is.
    if (md->target_id > 0)
    {
        if ((tbl = map_id2bl(md->target_id)))
        {
            if (tbl->type == BL_PC)
                tsd = (struct map_session_data *) tbl;
            else if (tbl->type == BL_MOB)
                tmd = (struct mob_data *) tbl;
            if (tsd || tmd)
            {
                if (tbl->m != md->bl.m || tbl->prev == NULL
                    || (dist =
                        distance(md->bl.x, md->bl.y, tbl->x,
                                  tbl->y)) >= md->min_chase)
                    mob_unlocktarget(md, tick);    // 別マップか、視界外
                else if (tsd && !bool(mode & MobMode::BOSS)
                         && (tsd->sc_data[SC_TRICKDEAD].timer != -1
                             ||
                             ((pc_ishiding(tsd)
                               || tsd->state.gangsterparadise) && race != 4
                              && race != 6)))
                    mob_unlocktarget(md, tick);    // スキルなどによる策敵妨害
                else if (!battle_check_range(&md->bl, tbl, mob_db[md->mob_class].range))
                {
                    // 攻撃範囲外なので移動
                    if (!bool(mode & MobMode::CAN_MOVE))
                    {           // 移動しないモード
                        mob_unlocktarget(md, tick);
                        return;
                    }
                    if (!mob_can_move(md)) // 動けない状態にある
                        return;
                    md->state.skillstate = MSS_CHASE;   // 突撃時スキル
                    mobskill_use(md, tick, MSC::ANY);
//                  if(md->timer != -1 && (DIFF_TICK(md->next_walktime,tick)<0 || distance(md->to_x,md->to_y,tsd->bl.x,tsd->bl.y)<2) )
                    if (md->timer != -1 && md->state.state != MS_ATTACK
                        && (DIFF_TICK(md->next_walktime, tick) < 0
                            || distance(md->to_x, md->to_y, tbl->x,
                                         tbl->y) < 2))
                        return;   // 既に移動中
                    if (!mob_can_reach(md, tbl, (md->min_chase > 13) ? md->min_chase : 13))
                        mob_unlocktarget(md, tick);    // 移動できないのでタゲ解除（IWとか？）
                    else
                    {
                        // 追跡
                        md->next_walktime = tick + 500;
                        i = 0;
                        do
                        {
                            if (i == 0)
                            {   // 最初はAEGISと同じ方法で検索
                                dx = tbl->x - md->bl.x;
                                dy = tbl->y - md->bl.y;
                                if (dx < 0)
                                    dx++;
                                else if (dx > 0)
                                    dx--;
                                if (dy < 0)
                                    dy++;
                                else if (dy > 0)
                                    dy--;
                            }
                            else
                            {   // だめならAthena式(ランダム)
                                dx = tbl->x - md->bl.x + MRAND(3) - 1;
                                dy = tbl->y - md->bl.y + MRAND(3) - 1;
                            }
                            /*                      if (path_search(&md->walkpath,md->bl.m,md->bl.x,md->bl.y,md->bl.x+dx,md->bl.y+dy,0)){
                             * dx=tsd->bl.x - md->bl.x;
                             * dy=tsd->bl.y - md->bl.y;
                             * if (dx<0) dx--;
                             * else if (dx>0) dx++;
                             * if (dy<0) dy--;
                             * else if (dy>0) dy++;
                             * } */
                            ret =
                                mob_walktoxy(md, md->bl.x + dx,
                                              md->bl.y + dy, 0);
                            i++;
                        }
                        while (ret && i < 5);

                        if (ret)
                        {       // 移動不可能な所からの攻撃なら2歩下る
                            if (dx < 0)
                                dx = 2;
                            else if (dx > 0)
                                dx = -2;
                            if (dy < 0)
                                dy = 2;
                            else if (dy > 0)
                                dy = -2;
                            mob_walktoxy(md, md->bl.x + dx, md->bl.y + dy,
                                          0);
                        }
                    }
                }
                else
                {               // 攻撃射程範囲内
                    md->state.skillstate = MSS_ATTACK;
                    if (md->state.state == MS_WALK)
                        mob_stop_walking(md, 1);   // 歩行中なら停止
                    if (md->state.state == MS_ATTACK)
                        return;   // 既に攻撃中
                    mob_changestate(md, MS_ATTACK, attack_type);

/*                                      if (mode&0x08){ // リンクモンスター
                                        map_foreachinarea(mob_ai_sub_hard_linksearch,md->bl.m,
                                                md->bl.x-13,md->bl.y-13,
                                                md->bl.x+13,md->bl.y+13,
                                                        BL_MOB,md,&tsd->bl);
                                }*/
                }
                return;
            }
            else
            {                   // ルートモンスター処理
                if (tbl == NULL || tbl->type != BL_ITEM || tbl->m != md->bl.m
                    || (dist =
                        distance(md->bl.x, md->bl.y, tbl->x,
                                  tbl->y)) >= md->min_chase || !md->lootitem)
                {
                    // 遠すぎるかアイテムがなくなった
                    mob_unlocktarget(md, tick);
                    if (md->state.state == MS_WALK)
                        mob_stop_walking(md, 1);   // 歩行中なら停止
                }
                else if (dist)
                {
                    if (!bool(mode & MobMode::CAN_MOVE))
                    {           // 移動しないモード
                        mob_unlocktarget(md, tick);
                        return;
                    }
                    if (!mob_can_move(md)) // 動けない状態にある
                        return;
                    md->state.skillstate = MSS_LOOT;    // ルート時スキル使用
                    mobskill_use(md, tick, MSC::ANY);
//                  if(md->timer != -1 && (DIFF_TICK(md->next_walktime,tick)<0 || distance(md->to_x,md->to_y,tbl->x,tbl->y)<2) )
                    if (md->timer != -1 && md->state.state != MS_ATTACK
                        && (DIFF_TICK(md->next_walktime, tick) < 0
                            || distance(md->to_x, md->to_y, tbl->x,
                                         tbl->y) <= 0))
                        return;   // 既に移動中
                    md->next_walktime = tick + 500;
                    dx = tbl->x - md->bl.x;
                    dy = tbl->y - md->bl.y;
/*                              if (path_search(&md->walkpath,md->bl.m,md->bl.x,md->bl.y,md->bl.x+dx,md->bl.y+dy,0)){
                                                dx=tbl->x - md->bl.x;
                                                dy=tbl->y - md->bl.y;
                                }*/
                    ret = mob_walktoxy(md, md->bl.x + dx, md->bl.y + dy, 0);
                    if (ret)
                        mob_unlocktarget(md, tick);    // 移動できないのでタゲ解除（IWとか？）
                }
                else
                {               // アイテムまでたどり着いた
                    if (md->state.state == MS_ATTACK)
                        return;   // 攻撃中
                    if (md->state.state == MS_WALK)
                        mob_stop_walking(md, 1);   // 歩行中なら停止
                    fitem = (struct flooritem_data *) tbl;
                    if (md->lootitem_count < LOOTITEM_SIZE)
                        memcpy(&md->lootitem[md->lootitem_count++],
                                &fitem->item_data, sizeof(md->lootitem[0]));
                    else if (battle_config.monster_loot_type == 1
                             && md->lootitem_count >= LOOTITEM_SIZE)
                    {
                        mob_unlocktarget(md, tick);
                        return;
                    }
                    else
                    {
                        for (i = 0; i < LOOTITEM_SIZE - 1; i++)
                            memcpy(&md->lootitem[i], &md->lootitem[i + 1],
                                    sizeof(md->lootitem[0]));
                        memcpy(&md->lootitem[LOOTITEM_SIZE - 1],
                                &fitem->item_data, sizeof(md->lootitem[0]));
                    }
                    map_clearflooritem(tbl->id);
                    mob_unlocktarget(md, tick);
                }
                return;
            }
        }
        else
        {
            mob_unlocktarget(md, tick);
            if (md->state.state == MS_WALK)
                mob_stop_walking(md, 4);   // 歩行中なら停止
            return;
        }
    }

    // It is skill use at the time of /standby at the time of a walk.
    if (mobskill_use(md, tick, MSC::ANY))
        return;

    // 歩行処理
    if (bool(mode & MobMode::CAN_MOVE)
        && mob_can_move(md)
        && (md->master_id == 0 || md->state.special_mob_ai
            || md->master_dist > 10))
    {                           //取り巻きMOBじゃない

        if (DIFF_TICK(md->next_walktime, tick) > +7000 &&
            (md->walkpath.path_len == 0
             || md->walkpath.path_pos >= md->walkpath.path_len))
        {
            md->next_walktime = tick + 3000 * MRAND(2000);
        }

        // Random movement
        if (mob_randomwalk(md, tick))
            return;
    }

    // Since he has finished walking, it stands by.
    if (md->walkpath.path_len == 0
        || md->walkpath.path_pos >= md->walkpath.path_len)
        md->state.skillstate = MSS_IDLE;
}

/*==========================================
 * Serious processing for mob in PC field of view (foreachclient)
 *------------------------------------------
 */
static
void mob_ai_sub_foreachclient(struct map_session_data *sd, unsigned int tick)
{
    nullpo_retv(sd);

    map_foreachinarea(std::bind(mob_ai_sub_hard, ph::_1, tick),
            sd->bl.m, sd->bl.x - AREA_SIZE * 2, sd->bl.y - AREA_SIZE * 2,
            sd->bl.x + AREA_SIZE * 2, sd->bl.y + AREA_SIZE * 2, BL_MOB);
}

/*==========================================
 * Serious processing for mob in PC field of view   (interval timer function)
 *------------------------------------------
 */
static
void mob_ai_hard(timer_id, tick_t tick, custom_id_t, custom_data_t)
{
    clif_foreachclient(std::bind(mob_ai_sub_foreachclient, ph::_1, tick));
}

/*==========================================
 * Negligent mode MOB AI (PC is not in near)
 *------------------------------------------
 */
static
void mob_ai_sub_lazy(db_key_t, db_val_t data, unsigned int tick)
{
    struct mob_data *md = (struct mob_data *)data;

    nullpo_retv(md);

    if (md == NULL)
        return;

    if (md->bl.type == BL_NUL || md->bl.type != BL_MOB)
        return;

    if (DIFF_TICK(tick, md->last_thinktime) < MIN_MOBTHINKTIME * 10)
        return;
    md->last_thinktime = tick;

    if (md->bl.prev == NULL || md->skilltimer != -1)
    {
        if (DIFF_TICK(tick, md->next_walktime) > MIN_MOBTHINKTIME * 10)
            md->next_walktime = tick;
        return;
    }

    if (DIFF_TICK(md->next_walktime, tick) < 0
        && bool(mob_db[md->mob_class].mode & MobMode::CAN_MOVE)
        && mob_can_move(md))
    {

        if (map[md->bl.m].users > 0)
        {
            // Since PC is in the same map, somewhat better negligent processing is carried out.

            // It sometimes moves.
            if (MRAND(1000) < MOB_LAZYMOVEPERC)
                mob_randomwalk(md, tick);

            // MOB which is not not the summons MOB but BOSS, either sometimes reboils.
            else if (MRAND(1000) < MOB_LAZYWARPPERC && md->x0 <= 0
                     && md->master_id != 0 && mob_db[md->mob_class].mexp <= 0
                     && !bool(mob_db[md->mob_class].mode & MobMode::BOSS))
                mob_spawn(md->bl.id);

        }
        else
        {
            // Since PC is not even in the same map, suitable processing is carried out even if it takes.

            // MOB which is not BOSS which is not Summons MOB, either -- a case -- sometimes -- leaping
            if (MRAND(1000) < MOB_LAZYWARPPERC && md->x0 <= 0
                && md->master_id != 0 && mob_db[md->mob_class].mexp <= 0
                && !bool(mob_db[md->mob_class].mode & MobMode::BOSS))
                mob_warp(md, -1, -1, -1, -1);
        }

        md->next_walktime = tick + MPRAND(5000, 10000);
    }
}

/*==========================================
 * Negligent processing for mob outside PC field of view   (interval timer function)
 *------------------------------------------
 */
static
void mob_ai_lazy(timer_id, tick_t tick, custom_id_t, custom_data_t)
{
    map_foreachiddb(std::bind(mob_ai_sub_lazy, ph::_1, ph::_2, tick));
}

/*==========================================
 * The structure object for item drop with delay
 * Since it is only two being able to pass [ int ] a timer function
 * Data is put in and passed to this structure object.
 *------------------------------------------
 */
struct delay_item_drop
{
    int m, x, y;
    int nameid, amount;
    struct map_session_data *first_sd, *second_sd, *third_sd;
};

struct delay_item_drop2
{
    int m, x, y;
    struct item item_data;
    struct map_session_data *first_sd, *second_sd, *third_sd;
};

/*==========================================
 * item drop with delay (timer function)
 *------------------------------------------
 */
static
void mob_delay_item_drop(timer_id, tick_t, custom_id_t id, custom_data_t)
{
    struct delay_item_drop *ditem;
    struct item temp_item;
    PickupFail flag;

    nullpo_retv(ditem = (struct delay_item_drop *) id);

    memset(&temp_item, 0, sizeof(temp_item));
    temp_item.nameid = ditem->nameid;
    temp_item.amount = ditem->amount;
    temp_item.identify = !itemdb_isequip3(temp_item.nameid);

    if (battle_config.item_auto_get == 1)
    {
        if (ditem->first_sd
            && (flag =
                pc_additem(ditem->first_sd, &temp_item, ditem->amount))
            != PickupFail::OKAY)
        {
            clif_additem(ditem->first_sd, 0, 0, flag);
            map_addflooritem(&temp_item, 1, ditem->m, ditem->x, ditem->y,
                              ditem->first_sd, ditem->second_sd,
                              ditem->third_sd, 0);
        }
        free(ditem);
        return;
    }

    map_addflooritem(&temp_item, 1, ditem->m, ditem->x, ditem->y,
                      ditem->first_sd, ditem->second_sd, ditem->third_sd, 0);

    free(ditem);
}

/*==========================================
 * item drop (timer function)-lootitem with delay
 *------------------------------------------
 */
static
void mob_delay_item_drop2(timer_id, tick_t, custom_id_t id, custom_data_t)
{
    struct delay_item_drop2 *ditem;
    PickupFail flag;

    nullpo_retv(ditem = (struct delay_item_drop2 *) id);

    if (battle_config.item_auto_get == 1)
    {
        if (ditem->first_sd
            && (flag =
                pc_additem(ditem->first_sd, &ditem->item_data,
                            ditem->item_data.amount))
            != PickupFail::OKAY)
        {
            clif_additem(ditem->first_sd, 0, 0, flag);
            map_addflooritem(&ditem->item_data, ditem->item_data.amount,
                              ditem->m, ditem->x, ditem->y, ditem->first_sd,
                              ditem->second_sd, ditem->third_sd, 0);
        }
        free(ditem);
        return;
    }

    map_addflooritem(&ditem->item_data, ditem->item_data.amount, ditem->m,
                      ditem->x, ditem->y, ditem->first_sd, ditem->second_sd,
                      ditem->third_sd, 0);

    free(ditem);
}

/*==========================================
 * mob data is erased.
 *------------------------------------------
 */
int mob_delete(struct mob_data *md)
{
    nullpo_retr(1, md);

    if (md->bl.prev == NULL)
        return 1;
    mob_changestate(md, MS_DEAD, 0);
    clif_clearchar_area(&md->bl, 1);
    map_delblock(&md->bl);
    mob_deleteslave(md);
    mob_setdelayspawn(md->bl.id);
    return 0;
}

int mob_catch_delete(struct mob_data *md, int type)
{
    nullpo_retr(1, md);

    if (md->bl.prev == NULL)
        return 1;
    mob_changestate(md, MS_DEAD, 0);
    clif_clearchar_area(&md->bl, type);
    map_delblock(&md->bl);
    mob_setdelayspawn(md->bl.id);
    return 0;
}

void mob_timer_delete(timer_id, tick_t, custom_id_t id, custom_data_t)
{
    struct block_list *bl = map_id2bl(id);
    struct mob_data *md;

    nullpo_retv(bl);

    md = (struct mob_data *) bl;
    mob_catch_delete(md, 3);
}

/*==========================================
 *
 *------------------------------------------
 */
static
void mob_deleteslave_sub(struct block_list *bl, int id)
{
    struct mob_data *md;

    nullpo_retv(bl);
    md = (struct mob_data *) bl;

    if (md->master_id > 0 && md->master_id == id)
        mob_damage(NULL, md, md->hp, 1);
}

/*==========================================
 *
 *------------------------------------------
 */
int mob_deleteslave(struct mob_data *md)
{
    nullpo_ret(md);

    map_foreachinarea(std::bind(mob_deleteslave_sub, ph::_1, md->bl.id),
            md->bl.m, 0, 0,
            map[md->bl.m].xs, map[md->bl.m].ys, BL_MOB);
    return 0;
}

#define DAMAGE_BONUS_COUNT 6    // max. number of players to account for
const static double damage_bonus_factor[DAMAGE_BONUS_COUNT + 1] = {
    1.0, 1.0, 2.0, 2.5, 2.75, 2.9, 3.0
};

/*==========================================
 * It is the damage of sd to damage to md.
 *------------------------------------------
 */
int mob_damage(struct block_list *src, struct mob_data *md, int damage,
                int type)
{
    int count, minpos, mindmg;
    struct map_session_data *sd = NULL, *tmpsd[DAMAGELOG_SIZE];
    struct
    {
        struct party *p;
        int id, base_exp, job_exp;
    } pt[DAMAGELOG_SIZE];
    int pnum = 0;
    int mvp_damage, max_hp;
    unsigned int tick = gettick();
    struct map_session_data *mvp_sd = NULL, *second_sd = NULL, *third_sd =
        NULL;
    double tdmg, temp;
    struct item item;
    PickupFail ret;

    nullpo_ret(md);        //srcはNULLで呼ばれる場合もあるので、他でチェック

    if (src && src->id == md->master_id
        && bool(md->mode & MOB_MODE_TURNS_AGAINST_BAD_MASTER))
    {
        /* If the master hits a monster, have the monster turn against him */
        md->master_id = 0;
        md->mode = MobMode::war;        /* Regular war mode */
        md->target_id = src->id;
        md->attacked_id = src->id;
    }

    max_hp = battle_get_max_hp(&md->bl);

    if (src && src->type == BL_PC)
    {
        sd = (struct map_session_data *) src;
        mvp_sd = sd;
    }

//  if(battle_config.battle_log)
//      PRINTF("mob_damage %d %d %d\n",md->hp,max_hp,damage);
    if (md->bl.prev == NULL)
    {
        if (battle_config.error_log == 1)
            PRINTF("mob_damage : BlockError!!\n");
        return 0;
    }

    if (md->state.state == MS_DEAD || md->hp <= 0)
    {
        if (md->bl.prev != NULL)
        {
            mob_changestate(md, MS_DEAD, 0);
            // It is skill at the time of death.
            mobskill_use(md, tick, MSC::ANY);

            clif_clearchar_area(&md->bl, 1);
            map_delblock(&md->bl);
            mob_setdelayspawn(md->bl.id);
        }
        return 0;
    }

    if (md->sc_data[SC_ENDURE].timer == -1)
        mob_stop_walking(md, 3);
    if (damage > max_hp >> 2)
        skill_stop_dancing(&md->bl, 0);

    if (md->hp > max_hp)
        md->hp = max_hp;

    // The amount of overkill rounds to hp.
    if (damage > md->hp)
        damage = md->hp;

    if (!(type & 2))
    {
        if (sd != NULL)
        {
            int i;
            for (i = 0, minpos = 0, mindmg = 0x7fffffff; i < DAMAGELOG_SIZE;
                 i++)
            {
                if (md->dmglog[i].id == sd->bl.id)
                    break;
                if (md->dmglog[i].id == 0)
                {
                    minpos = i;
                    mindmg = 0;
                }
                else if (md->dmglog[i].dmg < mindmg)
                {
                    minpos = i;
                    mindmg = md->dmglog[i].dmg;
                }
            }
            if (i < DAMAGELOG_SIZE)
                md->dmglog[i].dmg += damage;
            else
            {
                md->dmglog[minpos].id = sd->bl.id;
                md->dmglog[minpos].dmg = damage;
            }

            if (md->attacked_id <= 0 && md->state.special_mob_ai == 0)
                md->attacked_id = sd->bl.id;
        }
        if (src && src->type == BL_MOB
            && ((struct mob_data *) src)->state.special_mob_ai)
        {
            struct mob_data *md2 = (struct mob_data *) src;
            struct block_list *master_bl = map_id2bl(md2->master_id);
            if (master_bl && master_bl->type == BL_PC)
            {
                MAP_LOG_PC(((struct map_session_data *) master_bl),
                            "MOB-TO-MOB-DMG FROM MOB%d %d TO MOB%d %d FOR %d",
                            md2->bl.id, md2->mob_class, md->bl.id, md->mob_class,
                            damage);
            }

            nullpo_ret(md2);
            int i;
            for (i = 0, minpos = 0, mindmg = 0x7fffffff; i < DAMAGELOG_SIZE;
                 i++)
            {
                if (md->dmglog[i].id == md2->master_id)
                    break;
                if (md->dmglog[i].id == 0)
                {
                    minpos = i;
                    mindmg = 0;
                }
                else if (md->dmglog[i].dmg < mindmg)
                {
                    minpos = i;
                    mindmg = md->dmglog[i].dmg;
                }
            }
            if (i < DAMAGELOG_SIZE)
                md->dmglog[i].dmg += damage;
            else
            {
                md->dmglog[minpos].id = md2->master_id;
                md->dmglog[minpos].dmg = damage;

                if (md->attacked_id <= 0 && md->state.special_mob_ai == 0)
                    md->attacked_id = md2->master_id;
            }
        }

    }

    md->hp -= damage;

    if (bool(md->option & Option::HIDE2))
        skill_status_change_end(&md->bl, SC_HIDING, -1);
    if (bool(md->option & Option::CLOAK))
        skill_status_change_end(&md->bl, SC_CLOAKING, -1);

    if (md->state.special_mob_ai == 2)
    {                           //スフィアーマイン
        int skillidx = 0;

        if ((skillidx =
             mob_skillid2skillidx(md->mob_class, NPC_SELFDESTRUCTION2)) >= 0)
        {
            md->mode |= MobMode::CAN_MOVE;
            md->next_walktime = tick;
            mobskill_use_id(md, &md->bl, skillidx);    //自爆詠唱開始
            md->state.special_mob_ai++;
        }
    }

    if (md->hp > 0)
    {
        return 0;
    }

    MAP_LOG("MOB%d DEAD", md->bl.id);

    // ----- ここから死亡処理 -----

    map_freeblock_lock();
    mob_changestate(md, MS_DEAD, 0);
    mobskill_use(md, tick, MSC::ANY);

    memset(tmpsd, 0, sizeof(tmpsd));
    memset(pt, 0, sizeof(pt));

    max_hp = battle_get_max_hp(&md->bl);

    if (src && src->type == BL_MOB)
        mob_unlocktarget((struct mob_data *) src, tick);

    // map外に消えた人は計算から除くので
    // overkill分は無いけどsumはmax_hpとは違う

    tdmg = 0;
    count = 0;
    mvp_damage = 0;
    for (int i = 0; i < DAMAGELOG_SIZE; i++)
    {
        if (md->dmglog[i].id == 0)
            continue;
        tmpsd[i] = map_id2sd(md->dmglog[i].id);
        if (tmpsd[i] == NULL)
            continue;
        count++;
        if (tmpsd[i]->bl.m != md->bl.m || pc_isdead(tmpsd[i]))
            continue;

        tdmg += (double) md->dmglog[i].dmg;
        if (mvp_damage < md->dmglog[i].dmg)
        {
            third_sd = second_sd;
            second_sd = mvp_sd;
            mvp_sd = tmpsd[i];
            mvp_damage = md->dmglog[i].dmg;
        }
    }

    // [MouseJstr]
    if ((map[md->bl.m].flag.pvp == 0) || (battle_config.pvp_exp == 1))
    {
        // 経験値の分配
        for (int i = 0; i < DAMAGELOG_SIZE; i++)
        {

            int pid, base_exp, job_exp, flag = 1;
            double per;
            struct party *p;
            if (tmpsd[i] == NULL || tmpsd[i]->bl.m != md->bl.m)
                continue;
/* jAthena's exp formula
                per = ((double)md->dmglog[i].dmg)* (9.+(double)((count > 6)? 6:count))/10./((double)max_hp) * dmg_rate;
                temp = ((double)mob_db[md->mob_class].base_exp * (double)battle_config.base_exp_rate / 100. * per);
                base_exp = (temp > 2147483647.)? 0x7fffffff:(int)temp;
                if (mob_db[md->mob_class].base_exp > 0 && base_exp < 1) base_exp = 1;
                if (base_exp < 0) base_exp = 0;
                temp = ((double)mob_db[md->mob_class].job_exp * (double)battle_config.job_exp_rate / 100. * per);
                job_exp = (temp > 2147483647.)? 0x7fffffff:(int)temp;
                if (mob_db[md->mob_class].job_exp > 0 && job_exp < 1) job_exp = 1;
                if (job_exp < 0) job_exp = 0;
*/
//eAthena's exp formula rather than jAthena's
//      per=(double)md->dmglog[i].dmg*256*(9+(double)((count > 6)? 6:count))/10/(double)max_hp;
            // [Fate] The above is the old formula.  We do a more involved computation below.
            per = (double) md->dmglog[i].dmg * 256 / (double) max_hp;   // 256 = 100% of the score
            per *= damage_bonus_factor[count > DAMAGE_BONUS_COUNT ? DAMAGE_BONUS_COUNT : count];    // Bonus for party attack
            if (per > 512)
                per = 512;      // [Fate] Retained from before.  The maximum a single individual can get is double the original value.
            if (per < 1)
                per = 1;

            base_exp =
                ((mob_db[md->mob_class].base_exp *
                  md->stats[MOB_XP_BONUS]) >> MOB_XP_BONUS_SHIFT) * per / 256;
            if (base_exp < 1)
                base_exp = 1;
            if (sd && md && battle_config.pk_mode == 1
                && (mob_db[md->mob_class].lv - sd->status.base_level >= 20))
            {
                base_exp *= 1.15;   // pk_mode additional exp if monster >20 levels [Valaris]
            }
            if (md->state.special_mob_ai >= 1
                && battle_config.alchemist_summon_reward != 1)
                base_exp = 0;   // Added [Valaris]
            job_exp = mob_db[md->mob_class].job_exp * per / 256;
            if (job_exp < 1)
                job_exp = 1;
            if (sd && md && battle_config.pk_mode == 1
                && (mob_db[md->mob_class].lv - sd->status.base_level >= 20))
            {
                job_exp *= 1.15;    // pk_mode additional exp if monster >20 levels [Valaris]
            }
            if (md->state.special_mob_ai >= 1
                && battle_config.alchemist_summon_reward != 1)
                job_exp = 0;    // Added [Valaris]

            if ((pid = tmpsd[i]->status.party_id) > 0)
            {                   // パーティに入っている
                int j = 0;
                for (j = 0; j < pnum; j++)  // 公平パーティリストにいるかどうか
                    if (pt[j].id == pid)
                        break;
                if (j == pnum)
                {               // いないときは公平かどうか確認
                    if ((p = party_search(pid)) != NULL && p->exp != 0)
                    {
                        pt[pnum].id = pid;
                        pt[pnum].p = p;
                        pt[pnum].base_exp = base_exp;
                        pt[pnum].job_exp = job_exp;
                        pnum++;
                        flag = 0;
                    }
                }
                else
                {               // いるときは公平
                    pt[j].base_exp += base_exp;
                    pt[j].job_exp += job_exp;
                    flag = 0;
                }
            }
            if (flag)           // 各自所得
                pc_gainexp(tmpsd[i], base_exp, job_exp);
        }
        // 公平分配
        for (int i = 0; i < pnum; i++)
            party_exp_share(pt[i].p, md->bl.m, pt[i].base_exp,
                             pt[i].job_exp);

        // item drop
        if (!(type & 1))
        {
            for (int i = 0; i < 8; i++)
            {
                struct delay_item_drop *ditem;
                int drop_rate;

                if (md->state.special_mob_ai >= 1 && battle_config.alchemist_summon_reward != 1)    // Added [Valaris]
                    break;      // End

                if (mob_db[md->mob_class].dropitem[i].nameid <= 0)
                    continue;
                drop_rate = mob_db[md->mob_class].dropitem[i].p;
                if (drop_rate <= 0 && battle_config.drop_rate0item == 1)
                    drop_rate = 1;
                if (battle_config.drops_by_luk > 0 && sd && md)
                    drop_rate += (sd->status.attrs[ATTR::LUK] * battle_config.drops_by_luk) / 100;   // drops affected by luk [Valaris]
                if (sd && md && battle_config.pk_mode == 1
                    && (mob_db[md->mob_class].lv - sd->status.base_level >= 20))
                    drop_rate *= 1.25;  // pk_mode increase drops if 20 level difference [Valaris]
                if (drop_rate <= MRAND(10000))
                    continue;

                ditem = (struct delay_item_drop *)
                    calloc(1, sizeof(struct delay_item_drop));
                ditem->nameid = mob_db[md->mob_class].dropitem[i].nameid;
                ditem->amount = 1;
                ditem->m = md->bl.m;
                ditem->x = md->bl.x;
                ditem->y = md->bl.y;
                ditem->first_sd = mvp_sd;
                ditem->second_sd = second_sd;
                ditem->third_sd = third_sd;
                add_timer(tick + 500 + i, mob_delay_item_drop, (int) ditem, 0);
            }
            if (sd && sd->state.attack_type == BF_WEAPON)
            {
                for (int i = 0; i < sd->monster_drop_item_count; i++)
                {
                    struct delay_item_drop *ditem;
                    int race = battle_get_race(&md->bl);
                    if (sd->monster_drop_itemid[i] <= 0)
                        continue;
                    if (sd->monster_drop_race[i] & (1 << race) ||
                        (bool(mob_db[md->mob_class].mode & MobMode::BOSS)
                         && sd->monster_drop_race[i] & 1 << 10)
                        || (!bool(mob_db[md->mob_class].mode & MobMode::BOSS)
                            && sd->monster_drop_race[i] & 1 << 11))
                    {
                        if (sd->monster_drop_itemrate[i] <= MRAND(10000))
                            continue;

                        ditem = (struct delay_item_drop *)
                            calloc(1, sizeof(struct delay_item_drop));
                        ditem->nameid = sd->monster_drop_itemid[i];
                        ditem->amount = 1;
                        ditem->m = md->bl.m;
                        ditem->x = md->bl.x;
                        ditem->y = md->bl.y;
                        ditem->first_sd = mvp_sd;
                        ditem->second_sd = second_sd;
                        ditem->third_sd = third_sd;
                        add_timer(tick + 520 + i, mob_delay_item_drop,
                                   (int) ditem, 0);
                    }
                }
                if (sd->get_zeny_num > 0)
                    pc_getzeny(sd,
                                mob_db[md->mob_class].lv * 10 +
                                MRAND((sd->get_zeny_num + 1)));
            }
            if (md->lootitem)
            {
                for (int i = 0; i < md->lootitem_count; i++)
                {
                    struct delay_item_drop2 *ditem;

                    ditem = (struct delay_item_drop2 *)
                        calloc(1, sizeof(struct delay_item_drop2));
                    memcpy(&ditem->item_data, &md->lootitem[i],
                            sizeof(md->lootitem[0]));
                    ditem->m = md->bl.m;
                    ditem->x = md->bl.x;
                    ditem->y = md->bl.y;
                    ditem->first_sd = mvp_sd;
                    ditem->second_sd = second_sd;
                    ditem->third_sd = third_sd;
                    add_timer(tick + 540 + i, mob_delay_item_drop2,
                               (int) ditem, 0);
                }
            }
        }

        // mvp処理
        if (mvp_sd && mob_db[md->mob_class].mexp > 0)
        {
            int j;
            int mexp = battle_get_mexp(&md->bl);
            temp =
                ((double) mexp * (double) battle_config.mvp_exp_rate *
                 (9. + (double) count) / 1000.);
            mexp = (temp > 2147483647.) ? 0x7fffffff : (int) temp;
            if (mexp < 1)
                mexp = 1;
            clif_mvp_effect(mvp_sd);   // エフェクト
            pc_gainexp(mvp_sd, mexp, 0);
            for (j = 0; j < 3; j++)
            {
                int i = MRAND(3);
                if (mob_db[md->mob_class].mvpitem[i].nameid <= 0)
                    continue;
                int drop_rate = mob_db[md->mob_class].mvpitem[i].p;
                if (drop_rate <= 0 && battle_config.drop_rate0item == 1)
                    drop_rate = 1;
                if (drop_rate < battle_config.item_drop_mvp_min)
                    drop_rate = battle_config.item_drop_mvp_min;
                if (drop_rate > battle_config.item_drop_mvp_max)
                    drop_rate = battle_config.item_drop_mvp_max;
                if (drop_rate <= MRAND(10000))
                    continue;
                memset(&item, 0, sizeof(item));
                item.nameid = mob_db[md->mob_class].mvpitem[i].nameid;
                item.identify = !itemdb_isequip3(item.nameid);
                if (mvp_sd->weight * 2 > mvp_sd->max_weight)
                    map_addflooritem(&item, 1, mvp_sd->bl.m, mvp_sd->bl.x,
                                      mvp_sd->bl.y, mvp_sd, second_sd,
                                      third_sd, 1);
                else if ((ret = pc_additem(mvp_sd, &item, 1))
                        != PickupFail::OKAY)
                {
                    clif_additem(sd, 0, 0, ret);
                    map_addflooritem(&item, 1, mvp_sd->bl.m, mvp_sd->bl.x,
                                      mvp_sd->bl.y, mvp_sd, second_sd,
                                      third_sd, 1);
                }
                break;
            }
        }

    }                           // [MouseJstr]

    // SCRIPT実行
    if (md->npc_event[0])
    {
        if (sd == NULL)
        {
            if (mvp_sd != NULL)
                sd = mvp_sd;
            else
            {
                struct map_session_data *tmp_sd;
                int i;
                for (i = 0; i < fd_max; i++)
                {
                    if (session[i] && (tmp_sd = (struct map_session_data *)session[i]->session_data)
                        && tmp_sd->state.auth)
                    {
                        if (md->bl.m == tmp_sd->bl.m)
                        {
                            sd = tmp_sd;
                            break;
                        }
                    }
                }
            }
        }
        if (sd)
            npc_event(sd, md->npc_event, 0);
    }

    clif_clearchar_area(&md->bl, 1);
    map_delblock(&md->bl);
    mob_deleteslave(md);
    mob_setdelayspawn(md->bl.id);
    map_freeblock_unlock();

    return 0;
}

/*==========================================
 *
 *------------------------------------------
 */
int mob_class_change(struct mob_data *md, int *value)
{
    unsigned int tick = gettick();
    int i, c, hp_rate, max_hp, mob_class, count = 0;

    nullpo_ret(md);
    nullpo_ret(value);

    if (value[0] <= 1000 || value[0] > 2000)
        return 0;
    if (md->bl.prev == NULL)
        return 0;

    while (count < 5 && value[count] > 1000 && value[count] <= 2000)
        count++;
    if (count < 1)
        return 0;

    mob_class = value[MRAND(count)];
    if (mob_class <= 1000 || mob_class > 2000)
        return 0;

    max_hp = battle_get_max_hp(&md->bl);
    hp_rate = md->hp * 100 / max_hp;
    md->mob_class = mob_class;
    max_hp = battle_get_max_hp(&md->bl);
    if (battle_config.monster_class_change_full_recover == 1)
    {
        md->hp = max_hp;
        memset(md->dmglog, 0, sizeof(md->dmglog));
    }
    else
        md->hp = max_hp * hp_rate / 100;
    if (md->hp > max_hp)
        md->hp = max_hp;
    else if (md->hp < 1)
        md->hp = 1;

    memcpy(md->name, mob_db[mob_class].jname, 24);
    memset(&md->state, 0, sizeof(md->state));
    md->attacked_id = 0;
    md->target_id = 0;
    md->move_fail_count = 0;

    md->stats[MOB_SPEED] = mob_db[md->mob_class].speed;
    md->def_ele = mob_db[md->mob_class].element;

    mob_changestate(md, MS_IDLE, 0);
    skill_castcancel(&md->bl, 0);
    md->state.skillstate = MSS_IDLE;
    md->last_thinktime = tick;
    md->next_walktime = tick + MPRAND(5000, 50);
    md->attackabletime = tick;
    md->canmove_tick = tick;
    md->sg_count = 0;

    for (i = 0, c = tick - 1000 * 3600 * 10; i < MAX_MOBSKILL; i++)
        md->skilldelay[i] = c;
    md->skillid = SkillID();
    md->skilllv = 0;

    if (md->lootitem == NULL
        && bool(mob_db[mob_class].mode & MobMode::LOOTER))
        md->lootitem = (struct item *)
            calloc(LOOTITEM_SIZE, sizeof(struct item));

    skill_clear_unitgroup(&md->bl);
    skill_cleartimerskill(&md->bl);

    clif_clearchar_area(&md->bl, 0);
    clif_spawnmob(md);

    return 0;
}

/*==========================================
 * mob回復
 *------------------------------------------
 */
int mob_heal(struct mob_data *md, int heal)
{
    int max_hp = battle_get_max_hp(&md->bl);

    nullpo_ret(md);

    md->hp += heal;
    if (max_hp < md->hp)
        md->hp = max_hp;

    return 0;
}

/*==========================================
 * Added by RoVeRT
 *------------------------------------------
 */
static
void mob_warpslave_sub(struct block_list *bl, int id, int x, int y)
{
    struct mob_data *md = (struct mob_data *) bl;

    if (md->master_id == id)
    {
        mob_warp(md, -1, x, y, 2);
    }
}

/*==========================================
 * Added by RoVeRT
 *------------------------------------------
 */
static
int mob_warpslave(struct mob_data *md, int x, int y)
{
//PRINTF("warp slave\n");
    map_foreachinarea(std::bind(mob_warpslave_sub, ph::_1, md->bl.id, md->bl.x, md->bl.y),
            md->bl.m, x - AREA_SIZE, y - AREA_SIZE,
            x + AREA_SIZE, y + AREA_SIZE, BL_MOB);
    return 0;
}

/*==========================================
 * mobワープ
 *------------------------------------------
 */
int mob_warp(struct mob_data *md, int m, int x, int y, int type)
{
    int i = 0, c, xs = 0, ys = 0, bx = x, by = y;

    nullpo_ret(md);

    if (md->bl.prev == NULL)
        return 0;

    if (m < 0)
        m = md->bl.m;

    if (type >= 0)
    {
        if (map[md->bl.m].flag.monster_noteleport)
            return 0;
        clif_clearchar_area(&md->bl, type);
    }
    skill_unit_out_all(&md->bl, gettick(), 1);
    map_delblock(&md->bl);

    if (bx > 0 && by > 0)
    {                           // 位置指定の場合周囲９セルを探索
        xs = ys = 9;
    }

    while ((x < 0 || y < 0 || ((c = read_gat(m, x, y)) == 1 || c == 5))
           && (i++) < 1000)
    {
        if (xs > 0 && ys > 0 && i < 250)
        {                       // 指定位置付近の探索
            x = MPRAND(bx, xs) - xs / 2;
            y = MPRAND(by, ys) - ys / 2;
        }
        else
        {                       // 完全ランダム探索
            x = MPRAND(1, (map[m].xs - 2));
            y = MPRAND(1, (map[m].ys - 2));
        }
    }
    md->dir = 0;
    if (i < 1000)
    {
        md->bl.x = md->to_x = x;
        md->bl.y = md->to_y = y;
        md->bl.m = m;
    }
    else
    {
        if (battle_config.error_log == 1)
            PRINTF("MOB %d warp failed, mob_class = %d\n", md->bl.id, md->mob_class);
    }

    md->target_id = 0;          // タゲを解除する
    md->state.attackable = false;
    md->attacked_id = 0;
    md->state.skillstate = MSS_IDLE;
    mob_changestate(md, MS_IDLE, 0);

    if (type > 0 && i == 1000)
    {
        if (battle_config.battle_log == 1)
            PRINTF("MOB %d warp to (%d,%d), mob_class = %d\n", md->bl.id, x, y,
                    md->mob_class);
    }

    map_addblock(&md->bl);
    if (type > 0)
    {
        clif_spawnmob(md);
        mob_warpslave(md, md->bl.x, md->bl.y);
    }

    return 0;
}

/*==========================================
 * 画面内の取り巻きの数計算用(foreachinarea)
 *------------------------------------------
 */
static
void mob_countslave_sub(struct block_list *bl, int id, int *c)
{
    struct mob_data *md;

    nullpo_retv(bl);
    md = (struct mob_data *) bl;

    if (md->master_id == id)
        (*c)++;
}

/*==========================================
 * 画面内の取り巻きの数計算
 *------------------------------------------
 */
static
int mob_countslave(struct mob_data *md)
{
    int c = 0;

    nullpo_ret(md);

    map_foreachinarea(std::bind(mob_countslave_sub, ph::_1, md->bl.id, &c),
            md->bl.m, 0, 0,
            map[md->bl.m].xs - 1, map[md->bl.m].ys - 1, BL_MOB);
    return c;
}

/*==========================================
 * 手下MOB召喚
 *------------------------------------------
 */
int mob_summonslave(struct mob_data *md2, int *value, int amount, int flag)
{
    struct mob_data *md;
    int bx, by, m, count = 0, mob_class, k, a = amount;

    nullpo_ret(md2);
    nullpo_ret(value);

    bx = md2->bl.x;
    by = md2->bl.y;
    m = md2->bl.m;

    if (value[0] <= 1000 || value[0] > 2000)    // 値が異常なら召喚を止める
        return 0;
    while (count < 5 && value[count] > 1000 && value[count] <= 2000)
        count++;
    if (count < 1)
        return 0;

    for (k = 0; k < count; k++)
    {
        amount = a;
        mob_class = value[k];
        if (mob_class <= 1000 || mob_class > 2000)
            continue;
        for (; amount > 0; amount--)
        {
            int x = 0, y = 0, c = 0, i = 0;
            md = (struct mob_data *) calloc(1, sizeof(struct mob_data));
            if (bool(mob_db[mob_class].mode & MobMode::LOOTER))
                md->lootitem = (struct item *)
                    calloc(LOOTITEM_SIZE, sizeof(struct item));
            else
                md->lootitem = NULL;

            while ((x <= 0 || y <= 0 || (c = map_getcell(m, x, y)) == 1
                    || c == 5) && (i++) < 100)
            {
                x = MPRAND(bx, 9) - 4;
                y = MPRAND(by, 9) - 4;
            }
            if (i >= 100)
            {
                x = bx;
                y = by;
            }

            mob_spawn_dataset(md, "--ja--", mob_class);
            md->bl.prev = NULL;
            md->bl.next = NULL;
            md->bl.m = m;
            md->bl.x = x;
            md->bl.y = y;

            md->m = m;
            md->x0 = x;
            md->y0 = y;
            md->xs = 0;
            md->ys = 0;
            md->stats[MOB_SPEED] = md2->stats[MOB_SPEED];
            md->spawndelay1 = -1;   // 一度のみフラグ
            md->spawndelay2 = -1;   // 一度のみフラグ

            memset(md->npc_event, 0, sizeof(md->npc_event));
            md->bl.type = BL_MOB;
            map_addiddb(&md->bl);
            mob_spawn(md->bl.id);

            if (flag)
                md->master_id = md2->bl.id;
        }
    }
    return 0;
}

/*==========================================
 * 自分をロックしているPCの数を数える(foreachclient)
 *------------------------------------------
 */
static
void mob_counttargeted_sub(struct block_list *bl,
        int id, int *c, struct block_list *src, ATK target_lv)
{
    nullpo_retv(bl);
    nullpo_retv(c);

    if (id == bl->id || (src && id == src->id))
        return;
    if (bl->type == BL_PC)
    {
        struct map_session_data *sd = (struct map_session_data *) bl;
        if (sd && sd->attacktarget == id && sd->attacktimer != -1
            && sd->attacktarget_lv >= target_lv)
            (*c)++;
    }
    else if (bl->type == BL_MOB)
    {
        struct mob_data *md = (struct mob_data *) bl;
        if (md && md->target_id == id && md->timer != -1
            && md->state.state == MS_ATTACK && md->target_lv >= target_lv)
            (*c)++;
    }
}

/*==========================================
 * 自分をロックしているPCの数を数える
 *------------------------------------------
 */
int mob_counttargeted(struct mob_data *md, struct block_list *src,
        ATK target_lv)
{
    int c = 0;

    nullpo_ret(md);

    map_foreachinarea(std::bind(mob_counttargeted_sub, ph::_1, md->bl.id, &c, src, target_lv),
            md->bl.m, md->bl.x - AREA_SIZE, md->bl.y - AREA_SIZE,
            md->bl.x + AREA_SIZE, md->bl.y + AREA_SIZE,
            BL_NUL);
    return c;
}

/*==========================================
 *MOBskillから該当skillidのskillidxを返す
 *------------------------------------------
 */
int mob_skillid2skillidx(int mob_class, SkillID skillid)
{
    int i;
    struct mob_skill *ms = mob_db[mob_class].skill;

    if (ms == NULL)
        return -1;

    for (i = 0; i < mob_db[mob_class].maxskill; i++)
    {
        if (ms[i].skill_id == skillid)
            return i;
    }
    return -1;

}

//
// MOBスキル
//

/*==========================================
 * スキル使用（詠唱完了、ID指定）
 *------------------------------------------
 */
void mobskill_castend_id(timer_id tid, tick_t tick, custom_id_t id, custom_data_t)
{
    struct mob_data *md = NULL;
    struct block_list *bl;
    struct block_list *mbl;
    int range;

    if ((mbl = map_id2bl(id)) == NULL) //詠唱したMobがもういないというのは良くある正常処理
        return;
    if ((md = (struct mob_data *) mbl) == NULL)
    {
        PRINTF("mobskill_castend_id nullpo mbl->id:%d\n", mbl->id);
        return;
    }
    if (md->bl.type != BL_MOB || md->bl.prev == NULL)
        return;
    if (md->skilltimer != tid)  // タイマIDの確認
        return;

    md->skilltimer = -1;

    if (bool(md->opt1)
        || md->sc_data[SC_DIVINA].timer != -1
        || md->sc_data[SC_ROKISWEIL].timer != -1
        || md->sc_data[SC_STEELBODY].timer != -1)
        return;
    if (md->sc_data[SC_BERSERK].timer != -1)    //バーサーク
        return;

    if (md->skillid != NPC_EMOTION)
        md->last_thinktime = tick + battle_get_adelay(&md->bl);

    if ((bl = map_id2bl(md->skilltarget)) == NULL || bl->prev == NULL)
    {                           //スキルターゲットが存在しない
        //PRINTF("mobskill_castend_id nullpo\n");//ターゲットがいないときはnullpoじゃなくて普通に終了
        return;
    }
    if (md->bl.m != bl->m)
        return;

    if (((skill_get_inf(md->skillid) & 1) || (skill_get_inf2(md->skillid) & 4)) &&    // 彼我敵対関係チェック
        battle_check_target(&md->bl, bl, BCT_ENEMY) <= 0)
        return;
    range = skill_get_range(md->skillid, md->skilllv);
    if (range < 0)
        range = battle_get_range(&md->bl) - (range + 1);
    if (range + battle_config.mob_skill_add_range <
        distance(md->bl.x, md->bl.y, bl->x, bl->y))
        return;

    md->skilldelay[md->skillidx] = tick;

    if (battle_config.mob_skill_log == 1)
        PRINTF("MOB skill castend skill=%d, mob_class = %d\n",
                md->skillid, md->mob_class);
    mob_stop_walking(md, 0);

    switch (skill_get_nk(md->skillid))
    {
            // 攻撃系/吹き飛ばし系
        case 0:
        case 2:
            skill_castend_damage_id(&md->bl, bl, md->skillid, md->skilllv,
                                     tick, BCT_ZERO);
            break;
        case 1:                // 支援系
            if (!mob_db[md->mob_class].skill[md->skillidx].val[0] &&
                (md->skillid == AL_HEAL)
                && battle_check_undead(battle_get_race(bl),
                                        battle_get_elem_type(bl)))
                skill_castend_damage_id(&md->bl, bl, md->skillid,
                                         md->skilllv, tick, BCT_ZERO);
            else
                skill_castend_nodamage_id(&md->bl, bl, md->skillid,
                                           md->skilllv, tick, BCT_ZERO);
            break;
    }
}

/*==========================================
 * スキル使用（詠唱完了、場所指定）
 *------------------------------------------
 */
void mobskill_castend_pos(timer_id tid, tick_t tick, custom_id_t id, custom_data_t)
{
    struct mob_data *md = NULL;
    struct block_list *bl;
    int range, maxcount;

    //mobskill_castend_id同様詠唱したMobが詠唱完了時にもういないというのはありそうなのでnullpoから除外
    if ((bl = map_id2bl(id)) == NULL)
        return;

    nullpo_retv(md = (struct mob_data *) bl);

    if (md->bl.type != BL_MOB || md->bl.prev == NULL)
        return;

    if (md->skilltimer != tid)  // タイマIDの確認
        return;

    md->skilltimer = -1;

    if (bool(md->opt1)
        || md->sc_data[SC_DIVINA].timer != -1
        || md->sc_data[SC_ROKISWEIL].timer != -1
        || md->sc_data[SC_STEELBODY].timer != -1)
        return;
    if (md->sc_data[SC_BERSERK].timer != -1)    //バーサーク
        return;

    if (battle_config.monster_land_skill_limit == 1)
    {
        maxcount = skill_get_maxcount(md->skillid);
        if (maxcount > 0)
        {
            int i, c;
            for (i = c = 0; i < MAX_MOBSKILLUNITGROUP; i++)
            {
                if (md->skillunit[i].alive_count > 0
                    && md->skillunit[i].skill_id == md->skillid)
                    c++;
            }
            if (c >= maxcount)
                return;
        }
    }

    range = skill_get_range(md->skillid, md->skilllv);
    if (range < 0)
        range = battle_get_range(&md->bl) - (range + 1);
    if (range + battle_config.mob_skill_add_range <
        distance(md->bl.x, md->bl.y, md->skillx, md->skilly))
        return;
    md->skilldelay[md->skillidx] = tick;

    if (battle_config.mob_skill_log == 1)
        PRINTF("MOB skill castend skill=%d, mob_class = %d\n",
                md->skillid, md->mob_class);
    mob_stop_walking(md, 0);
}

/*==========================================
 * Skill use (an aria start, ID specification)
 *------------------------------------------
 */
int mobskill_use_id(struct mob_data *md, struct block_list *target,
                     int skill_idx)
{
    int casttime, range;
    struct mob_skill *ms;
    SkillID skill_id;
    int skill_lv;

    nullpo_ret(md);
    nullpo_ret(ms = &mob_db[md->mob_class].skill[skill_idx]);

    if (target == NULL && (target = map_id2bl(md->target_id)) == NULL)
        return 0;

    if (target->prev == NULL || md->bl.prev == NULL)
        return 0;

    skill_id = ms->skill_id;
    skill_lv = ms->skill_lv;

    if (bool(md->opt1)
        || md->sc_data[SC_DIVINA].timer != -1
        || md->sc_data[SC_ROKISWEIL].timer != -1
        || md->sc_data[SC_STEELBODY].timer != -1)
        return 0;
    if (md->sc_data[SC_BERSERK].timer != -1)    //バーサーク
        return 0;

    if (bool(md->option & Option::HIDE2))
        return 0;

    if (skill_get_inf2(skill_id) & 0x200 && md->bl.id == target->id)
        return 0;

    // 射程と障害物チェック
    range = skill_get_range(skill_id, skill_lv);
    if (range < 0)
        range = battle_get_range(&md->bl) - (range + 1);

    if (!battle_check_range(&md->bl, target, range))
        return 0;

//  delay=skill_delayfix(&md->bl, skill_get_delay( skill_id,skill_lv) );

    casttime = skill_castfix(&md->bl, ms->casttime);
    md->state.skillcastcancel = ms->cancel;
    md->skilldelay[skill_idx] = gettick();

    if (battle_config.mob_skill_log == 1)
        PRINTF("MOB skill use target_id=%d skill=%d lv=%d cast=%d, mob_class = %d\n",
             target->id, skill_id, skill_lv,
             casttime, md->mob_class);

    if (casttime <= 0)          // 詠唱の無いものはキャンセルされない
        md->state.skillcastcancel = 0;

    md->skilltarget = target->id;
    md->skillx = 0;
    md->skilly = 0;
    md->skillid = skill_id;
    md->skilllv = skill_lv;
    md->skillidx = skill_idx;

    if (!(battle_config.monster_cloak_check_type & 2)
        && md->sc_data[SC_CLOAKING].timer != -1)
        skill_status_change_end(&md->bl, SC_CLOAKING, -1);

    if (casttime > 0)
    {
        md->skilltimer =
            add_timer(gettick() + casttime, mobskill_castend_id, md->bl.id,
                       0);
    }
    else
    {
        md->skilltimer = -1;
        mobskill_castend_id(md->skilltimer, gettick(), md->bl.id, 0);
    }

    return 1;
}

/*==========================================
 * スキル使用（場所指定）
 *------------------------------------------
 */
static
int mobskill_use_pos(struct mob_data *md,
                      int skill_x, int skill_y, int skill_idx)
{
    int casttime = 0, range;
    struct mob_skill *ms;
    struct block_list bl;
    int skill_lv;

    nullpo_ret(md);
    nullpo_ret(ms = &mob_db[md->mob_class].skill[skill_idx]);

    if (md->bl.prev == NULL)
        return 0;

    SkillID skill_id = ms->skill_id;
    skill_lv = ms->skill_lv;

    if (bool(md->opt1)
        || md->sc_data[SC_DIVINA].timer != -1
        || md->sc_data[SC_ROKISWEIL].timer != -1
        || md->sc_data[SC_STEELBODY].timer != -1)
        return 0;
    if (md->sc_data[SC_BERSERK].timer != -1)    //バーサーク
        return 0;

    if (bool(md->option & Option::HIDE2))
        return 0;

    // 射程と障害物チェック
    bl.type = BL_NUL;
    bl.m = md->bl.m;
    bl.x = skill_x;
    bl.y = skill_y;
    range = skill_get_range(skill_id, skill_lv);
    if (range < 0)
        range = battle_get_range(&md->bl) - (range + 1);
    if (!battle_check_range(&md->bl, &bl, range))
        return 0;

//  delay=skill_delayfix(&sd->bl, skill_get_delay( skill_id,skill_lv) );
    casttime = skill_castfix(&md->bl, ms->casttime);
    md->skilldelay[skill_idx] = gettick();
    md->state.skillcastcancel = ms->cancel;

    if (battle_config.mob_skill_log == 1)
        PRINTF("MOB skill use target_pos= (%d,%d) skill=%d lv=%d cast=%d, mob_class = %d\n",
             skill_x, skill_y, skill_id, skill_lv,
             casttime, md->mob_class);

    if (casttime <= 0)          // A skill without a cast time wont be cancelled.
        md->state.skillcastcancel = 0;

    md->skillx = skill_x;
    md->skilly = skill_y;
    md->skilltarget = 0;
    md->skillid = skill_id;
    md->skilllv = skill_lv;
    md->skillidx = skill_idx;
    if (!(battle_config.monster_cloak_check_type & 2)
        && md->sc_data[SC_CLOAKING].timer != -1)
        skill_status_change_end(&md->bl, SC_CLOAKING, -1);
    if (casttime > 0)
    {
        md->skilltimer =
            add_timer(gettick() + casttime, mobskill_castend_pos, md->bl.id,
                       0);
    }
    else
    {
        md->skilltimer = -1;
        mobskill_castend_pos(md->skilltimer, gettick(), md->bl.id, 0);
    }

    return 1;
}

/*==========================================
 * Friendly Mob whose HP is decreasing by a nearby MOB is looked for.
 *------------------------------------------
 */
static
void mob_getfriendhpltmaxrate_sub(struct block_list *bl, struct mob_data *mmd, int rate, struct mob_data **fr)
{
    struct mob_data *md;

    nullpo_retv(bl);
    nullpo_retv(mmd);

    md = (struct mob_data *) bl;

    if (mmd->bl.id == bl->id)
        return;

    if (md->hp < mob_db[md->mob_class].max_hp * rate / 100)
        (*fr) = md;
}

static
struct mob_data *mob_getfriendhpltmaxrate(struct mob_data *md, int rate)
{
    struct mob_data *fr = NULL;
    const int r = 8;

    nullpo_retr(NULL, md);

    map_foreachinarea(std::bind(mob_getfriendhpltmaxrate_sub, ph::_1, md, rate, &fr),
            md->bl.m, md->bl.x - r, md->bl.y - r,
            md->bl.x + r, md->bl.y + r, BL_MOB);
    return fr;
}

/*==========================================
 * What a status state suits by nearby MOB is looked for.
 *------------------------------------------
 */
static
void mob_getfriendstatus_sub(struct block_list *bl, struct mob_data *mmd, MSC cond1, StatusChange cond2, struct mob_data **fr)
{
    struct mob_data *md;
    int flag = 0;

    nullpo_retv(bl);
    md = (struct mob_data *) bl;

    if (mmd->bl.id == bl->id)
        return;

    if (cond2 == StatusChange::ANY_BAD)
    {
        for (StatusChange j : MAJOR_STATUS_EFFECTS)
        {
            flag = (md->sc_data[j].timer != -1);
            if (flag)
                break;
        }
    }
    else
        flag = (md->sc_data[cond2].timer != -1);
    if (flag ^ (cond1 == MSC_FRIENDSTATUSOFF))
        (*fr) = md;
}

static
struct mob_data *mob_getfriendstatus(struct mob_data *md,
        MSC cond1, StatusChange cond2)
{
    struct mob_data *fr = NULL;
    const int r = 8;

    nullpo_ret(md);

    map_foreachinarea(std::bind(mob_getfriendstatus_sub, ph::_1, md, cond1, cond2, &fr),
            md->bl.m, md->bl.x - r, md->bl.y - r,
            md->bl.x + r, md->bl.y + r, BL_MOB);
    return fr;
}

/*==========================================
 * Skill use judging
 *------------------------------------------
 */
int mobskill_use(struct mob_data *md, unsigned int tick,
        MSC event, SkillID skill)
{
    struct mob_skill *ms;
//  struct block_list *target=NULL;
    int max_hp;

    nullpo_ret(md);
    nullpo_ret(ms = mob_db[md->mob_class].skill);

    max_hp = battle_get_max_hp(&md->bl);

    if (battle_config.mob_skill_use == 0 || md->skilltimer != -1)
        return 0;

    if (md->state.special_mob_ai)
        return 0;

    if (md->sc_data[SC_SELFDESTRUCTION].timer != -1)    //自爆中はスキルを使わない
        return 0;

    for (int ii = 0; ii < mob_db[md->mob_class].maxskill; ii++)
    {
        int flag = 0;
        struct mob_data *fmd = NULL;

        // ディレイ中
        if (DIFF_TICK(tick, md->skilldelay[ii]) < ms[ii].delay)
            continue;

        // 状態判定
        if (ms[ii].state != MSS::ANY && ms[ii].state != md->state.skillstate)
            continue;

        // Note: these *may* both be MSC::ANY
        flag = (event == ms[ii].cond1);
        if (!flag)
        {
            switch (ms[ii].cond1)
            {
                case MSC_ALWAYS:
                    flag = 1;
                    break;
                case MSC_MYHPLTMAXRATE:    // HP< maxhp%
                    flag = (md->hp < max_hp * ms[ii].cond2i / 100);
                    break;
                case MSC_MYSTATUSON:   // status[num] on
                case MSC_MYSTATUSOFF:  // status[num] off
                    if (ms[ii].cond2sc() == StatusChange::ANY_BAD)
                    {
                        for (StatusChange j : MAJOR_STATUS_EFFECTS)
                        {
                            flag = (md->sc_data[j].timer != -1);
                            if (flag)
                                break;
                        }
                    }
                    else
                        flag = (md->sc_data[ms[ii].cond2sc()].timer != -1);
                    flag ^= (ms[ii].cond1 == MSC_MYSTATUSOFF);
                    break;
                case MSC_FRIENDHPLTMAXRATE:    // friend HP < maxhp%
                    flag =
                        ((fmd =
                          mob_getfriendhpltmaxrate(md,
                                                    ms[ii].cond2i)) != NULL);
                    break;
                case MSC_FRIENDSTATUSON:   // friend status[num] on
                case MSC_FRIENDSTATUSOFF:  // friend status[num] off
                    flag =
                        ((fmd =
                          mob_getfriendstatus(md, ms[ii].cond1,
                                               ms[ii].cond2sc())) != NULL);
                    break;
                case MSC_NOTINTOWN:     // Only outside of towns.
                    flag = !map[md->bl.m].flag.town;
                    break;
                case MSC_SLAVELT:  // slave < num
                    flag = (mob_countslave(md) < ms[ii].cond2i);
                    break;
                case MSC_ATTACKPCGT:   // attack pc > num
                    flag = (mob_counttargeted(md, NULL, ATK::ZERO) > ms[ii].cond2i);
                    break;
                case MSC_SLAVELE:  // slave <= num
                    flag = (mob_countslave(md) <= ms[ii].cond2i);
                    break;
                case MSC_ATTACKPCGE:   // attack pc >= num
                    flag = (mob_counttargeted(md, NULL, ATK::ZERO) >= ms[ii].cond2i);
                    break;
                case MSC_SKILLUSED:    // specificated skill used
                    flag = (event == MSC_SKILLUSED
                            && (skill == ms[ii].cond2sk()
                                || ms[ii].cond2sk() == SkillID::ZERO));
                    break;
            }
        }

        // 確率判定
        if (flag && MRAND(10000) < ms[ii].permillage)
        {

            if (skill_get_inf(ms[ii].skill_id) & 2)
            {
                // 場所指定
                struct block_list *bl = NULL;
                int x = 0, y = 0;
                if (ms[ii].target <= MST_AROUND)
                {
                    if (ms[ii].target == MST_MASTER)
                    {
                        bl = &md->bl;
                        if (md->master_id)
                            bl = map_id2bl(md->master_id);
                    }
                    else
                    {
                        bl = ((ms[ii].target == MST_TARGET
                               || ms[ii].target ==
                               MST_AROUND5) ? map_id2bl(md->
                                                         target_id)
                              : (ms[ii].target ==
                                 MST_FRIEND) ? &fmd->bl : &md->bl);
                    }

                    if (bl)
                    {
                        x = bl->x;
                        y = bl->y;
                    }
                }
                if (x <= 0 || y <= 0)
                    continue;
                // 自分の周囲
                if (ms[ii].target >= MST_AROUND1)
                {
                    int bx, by, i = 0, c, m = bl->m;
                    // the enum values for radii are adjacent
                    int r = int(ms[i].target) - int(MST_AROUND1);
                    do
                    {
                        bx = x + MRAND((r * 2 + 3)) - r;
                        by = y + MRAND((r * 2 + 3)) - r;
                    }
                    while ((bx <= 0 || by <= 0 || bx >= map[m].xs
                            || by >= map[m].ys
                            || ((c = read_gat(m, bx, by)) == 1 || c == 5))
                           && (i++) < 1000);
                    if (i < 1000)
                    {
                        x = bx;
                        y = by;
                    }
                }
                // 相手の周囲
                if (ms[ii].target >= MST_AROUND5)
                {
                    int bx, by, i = 0, c, m = bl->m;
                    int r = int(ms[i].target) - int(MST_AROUND5) + 1;
                    do
                    {
                        bx = x + MRAND((r * 2 + 1)) - r;
                        by = y + MRAND((r * 2 + 1)) - r;
                    }
                    while ((bx <= 0 || by <= 0 || bx >= map[m].xs
                            || by >= map[m].ys
                            || ((c = read_gat(m, bx, by)) == 1 || c == 5))
                           && (i++) < 1000);
                    if (i < 1000)
                    {
                        x = bx;
                        y = by;
                    }
                }
                if (!mobskill_use_pos(md, x, y, ii))
                    return 0;

            }
            else
            {
                if (ms[ii].target == MST_MASTER)
                {
                    struct block_list *bl = &md->bl;
                    if (md->master_id)
                        bl = map_id2bl(md->master_id);

                    if (bl && !mobskill_use_id(md, bl, ii))
                        return 0;
                }
                // ID指定
                if (ms[ii].target <= MST_FRIEND)
                {
                    struct block_list *bl = NULL;
                    bl = ((ms[ii].target ==
                           MST_TARGET) ? map_id2bl(md->
                                                    target_id) : (ms[ii].target
                                                                  ==
                                                                  MST_FRIEND)
                          ? &fmd->bl : &md->bl);
                    if (bl && !mobskill_use_id(md, bl, ii))
                        return 0;
                }
            }
            if (ms[ii].emotion >= 0)
                clif_emotion(&md->bl, ms[ii].emotion);
            return 1;
        }
    }

    return 0;
}

/*==========================================
 * Skill use event processing
 *------------------------------------------
 */
int mobskill_event(struct mob_data *md, BF flag)
{
    nullpo_ret(md);

    if (flag == BF::NEGATIVE_1 && mobskill_use(md, gettick(), MSC_CASTTARGETED))
        return 1;
    if (bool(flag & BF_SHORT)
        && mobskill_use(md, gettick(), MSC_CLOSEDATTACKED))
        return 1;
    if (bool(flag & BF_LONG)
        && mobskill_use(md, gettick(), MSC_LONGRANGEATTACKED))
        return 1;
    return 0;
}

//
// 初期化
//
/*==========================================
 * Since un-setting [ mob ] up was used, it is an initial provisional value setup.
 *------------------------------------------
 */
static
int mob_makedummymobdb(int mob_class)
{
    int i;

    sprintf(mob_db[mob_class].name, "mob%d", mob_class);
    sprintf(mob_db[mob_class].jname, "mob%d", mob_class);
    mob_db[mob_class].lv = 1;
    mob_db[mob_class].max_hp = 1000;
    mob_db[mob_class].max_sp = 1;
    mob_db[mob_class].base_exp = 2;
    mob_db[mob_class].job_exp = 1;
    mob_db[mob_class].range = 1;
    mob_db[mob_class].atk1 = 7;
    mob_db[mob_class].atk2 = 10;
    mob_db[mob_class].def = 0;
    mob_db[mob_class].mdef = 0;
    mob_db[mob_class].attrs[ATTR::STR] = 1;
    mob_db[mob_class].attrs[ATTR::AGI] = 1;
    mob_db[mob_class].attrs[ATTR::VIT] = 1;
    mob_db[mob_class].attrs[ATTR::INT] = 1;
    mob_db[mob_class].attrs[ATTR::DEX] = 6;
    mob_db[mob_class].attrs[ATTR::LUK] = 2;
    mob_db[mob_class].range2 = 10;
    mob_db[mob_class].range3 = 10;
    mob_db[mob_class].size = 0;
    mob_db[mob_class].race = 0;
    mob_db[mob_class].element = 0;
    mob_db[mob_class].mode = MobMode::ZERO;
    mob_db[mob_class].speed = 300;
    mob_db[mob_class].adelay = 1000;
    mob_db[mob_class].amotion = 500;
    mob_db[mob_class].dmotion = 500;
    mob_db[mob_class].dropitem[0].nameid = 909; // Jellopy
    mob_db[mob_class].dropitem[0].p = 1000;
    for (i = 1; i < 8; i++)
    {
        mob_db[mob_class].dropitem[i].nameid = 0;
        mob_db[mob_class].dropitem[i].p = 0;
    }
    // Item1,Item2
    mob_db[mob_class].mexp = 0;
    mob_db[mob_class].mexpper = 0;
    for (i = 0; i < 3; i++)
    {
        mob_db[mob_class].mvpitem[i].nameid = 0;
        mob_db[mob_class].mvpitem[i].p = 0;
    }
    for (i = 0; i < MAX_RANDOMMONSTER; i++)
        mob_db[mob_class].summonper[i] = 0;
    return 0;
}

/*==========================================
 * db/mob_db.txt reading
 *------------------------------------------
 */
static
int mob_readdb(void)
{
    FILE *fp;
    char line[1024];
    const char *filename[] = { "db/mob_db.txt", "db/mob_db2.txt" };

    memset(mob_db, 0, sizeof(mob_db));

    for (int j = 0; j < 2; j++)
    {

        fp = fopen_(filename[j], "r");
        if (fp == NULL)
        {
            if (j > 0)
                continue;
            return -1;
        }
        while (fgets(line, 1020, fp))
        {
            int mob_class;
            char *str[57], *p, *np;

            if (line[0] == '/' && line[1] == '/')
                continue;

            p = line;
            for (int i = 0; i < 57; i++)
            {
                while (*p == '\t' || *p == ' ')
                    p++;
                if ((np = strchr(p, ',')) != NULL)
                {
                    str[i] = p;
                    *np = 0;
                    p = np + 1;
                }
                else
                    str[i] = p;
            }

            mob_class = atoi(str[0]);
            if (mob_class <= 1000 || mob_class > 2000)
                continue;

            memcpy(mob_db[mob_class].name, str[1], 24);
            memcpy(mob_db[mob_class].jname, str[2], 24);
            mob_db[mob_class].lv = atoi(str[3]);
            mob_db[mob_class].max_hp = atoi(str[4]);
            mob_db[mob_class].max_sp = atoi(str[5]);

            mob_db[mob_class].base_exp = atoi(str[6]);
            if (mob_db[mob_class].base_exp < 0)
                mob_db[mob_class].base_exp = 0;
            else if (mob_db[mob_class].base_exp > 0
                     && (mob_db[mob_class].base_exp *
                         battle_config.base_exp_rate / 100 > 1000000000
                         || mob_db[mob_class].base_exp *
                         battle_config.base_exp_rate / 100 < 0))
                mob_db[mob_class].base_exp = 1000000000;
            else
                mob_db[mob_class].base_exp *= battle_config.base_exp_rate / 100;

            mob_db[mob_class].job_exp = atoi(str[7]);
            if (mob_db[mob_class].job_exp < 0)
                mob_db[mob_class].job_exp = 0;
            else if (mob_db[mob_class].job_exp > 0
                     && (mob_db[mob_class].job_exp * battle_config.job_exp_rate /
                         100 > 1000000000
                         || mob_db[mob_class].job_exp *
                         battle_config.job_exp_rate / 100 < 0))
                mob_db[mob_class].job_exp = 1000000000;
            else
                mob_db[mob_class].job_exp *= battle_config.job_exp_rate / 100;

            mob_db[mob_class].range = atoi(str[8]);
            mob_db[mob_class].atk1 = atoi(str[9]);
            mob_db[mob_class].atk2 = atoi(str[10]);
            mob_db[mob_class].def = atoi(str[11]);
            mob_db[mob_class].mdef = atoi(str[12]);
            mob_db[mob_class].attrs[ATTR::STR] = atoi(str[13]);
            mob_db[mob_class].attrs[ATTR::AGI] = atoi(str[14]);
            mob_db[mob_class].attrs[ATTR::VIT] = atoi(str[15]);
            mob_db[mob_class].attrs[ATTR::INT] = atoi(str[16]);
            mob_db[mob_class].attrs[ATTR::DEX] = atoi(str[17]);
            mob_db[mob_class].attrs[ATTR::LUK] = atoi(str[18]);
            mob_db[mob_class].range2 = atoi(str[19]);
            mob_db[mob_class].range3 = atoi(str[20]);
            mob_db[mob_class].size = atoi(str[21]);
            mob_db[mob_class].race = atoi(str[22]);
            mob_db[mob_class].element = atoi(str[23]);
            mob_db[mob_class].mode = static_cast<MobMode>(atoi(str[24]));
            mob_db[mob_class].speed = atoi(str[25]);
            mob_db[mob_class].adelay = atoi(str[26]);
            mob_db[mob_class].amotion = atoi(str[27]);
            mob_db[mob_class].dmotion = atoi(str[28]);

            for (int i = 0; i < 8; i++)
            {
                int rate = 0, ratemin, ratemax;
                mob_db[mob_class].dropitem[i].nameid = atoi(str[29 + i * 2]);
                ItemType type = itemdb_type(mob_db[mob_class].dropitem[i].nameid);
                if (type == ItemType::USE)
                {               // Added [Valaris]
                    rate = battle_config.item_rate_heal;
                    ratemin = battle_config.item_drop_heal_min;
                    ratemax = battle_config.item_drop_heal_max;
                }
                else if (type == ItemType::_2)
                {
                    rate = battle_config.item_rate_use;
                    ratemin = battle_config.item_drop_use_min;
                    ratemax = battle_config.item_drop_use_max;  // End
                }
                else if (type == ItemType::WEAPON
                    || type == ItemType::ARMOR
                    || type == ItemType::_8)
                {
                    rate = battle_config.item_rate_equip;
                    ratemin = battle_config.item_drop_equip_min;
                    ratemax = battle_config.item_drop_equip_max;
                }
                else if (type == ItemType::_6)
                {
                    rate = battle_config.item_rate_card;
                    ratemin = battle_config.item_drop_card_min;
                    ratemax = battle_config.item_drop_card_max;
                }
                else
                {
                    rate = battle_config.item_rate_common;
                    ratemin = battle_config.item_drop_common_min;
                    ratemax = battle_config.item_drop_common_max;
                }
                rate = (rate / 100) * atoi(str[30 + i * 2]);
                rate =
                    (rate < ratemin) ? ratemin : (rate >
                                                  ratemax) ? ratemax : rate;
                mob_db[mob_class].dropitem[i].p = rate;
            }
            // Item1,Item2
            mob_db[mob_class].mexp =
                atoi(str[45]) * battle_config.mvp_exp_rate / 100;
            mob_db[mob_class].mexpper = atoi(str[46]);
            for (int i = 0; i < 3; i++)
            {
                mob_db[mob_class].mvpitem[i].nameid = atoi(str[47 + i * 2]);
                mob_db[mob_class].mvpitem[i].p =
                    atoi(str[48 + i * 2]) * battle_config.mvp_item_rate /
                    100;
            }
            mob_db[mob_class].mutations_nr = atoi(str[55]);
            mob_db[mob_class].mutation_power = atoi(str[56]);

            for (int i = 0; i < MAX_RANDOMMONSTER; i++)
                mob_db[mob_class].summonper[i] = 0;
            mob_db[mob_class].maxskill = 0;

            mob_db[mob_class].sex = 0;
            mob_db[mob_class].hair = 0;
            mob_db[mob_class].hair_color = 0;
            mob_db[mob_class].weapon = 0;
            mob_db[mob_class].shield = 0;
            mob_db[mob_class].head_top = 0;
            mob_db[mob_class].head_mid = 0;
            mob_db[mob_class].head_buttom = 0;
            mob_db[mob_class].clothes_color = 0;    //Add for player monster dye - Valaris

            if (mob_db[mob_class].base_exp == 0)
                mob_db[mob_class].base_exp = mob_gen_exp(&mob_db[mob_class]);
        }
        fclose_(fp);
        PRINTF("read %s done\n", filename[j]);
    }
    return 0;
}

/*==========================================
 * Reading of random monster data
 *------------------------------------------
 */
static
int mob_read_randommonster(void)
{
    FILE *fp;
    char line[1024];
    char *str[10], *p;
    int i, j;

    const char *mobfile[] = {
        "db/mob_branch.txt",
        "db/mob_poring.txt",
        "db/mob_boss.txt"
    };

    for (i = 0; i < MAX_RANDOMMONSTER; i++)
    {
        mob_db[0].summonper[i] = 1002;  // 設定し忘れた場合はポリンが出るようにしておく
        fp = fopen_(mobfile[i], "r");
        if (fp == NULL)
        {
            PRINTF("can't read %s\n", mobfile[i]);
            return -1;
        }
        while (fgets(line, 1020, fp))
        {
            int mob_class, per;
            if (line[0] == '/' && line[1] == '/')
                continue;
            memset(str, 0, sizeof(str));
            for (j = 0, p = line; j < 3 && p; j++)
            {
                str[j] = p;
                p = strchr(p, ',');
                if (p)
                    *p++ = 0;
            }

            if (str[0] == NULL || str[2] == NULL)
                continue;

            mob_class = atoi(str[0]);
            per = atoi(str[2]);
            if ((mob_class > 1000 && mob_class <= 2000) || mob_class == 0)
                mob_db[mob_class].summonper[i] = per;
        }
        fclose_(fp);
        PRINTF("read %s done\n", mobfile[i]);
    }
    return 0;
}

/*==========================================
 * db/mob_skill_db.txt reading
 *------------------------------------------
 */
static
int mob_readskilldb(void)
{
    FILE *fp;
    char line[1024];
    int i;

    const struct
    {
        char str[32];
        MSC id;
    } cond1[] =
    {
        {"always", MSC_ALWAYS},
        {"myhpltmaxrate", MSC_MYHPLTMAXRATE},
        {"friendhpltmaxrate", MSC_FRIENDHPLTMAXRATE},
        {"mystatuson", MSC_MYSTATUSON},
        {"mystatusoff", MSC_MYSTATUSOFF},
        {"friendstatuson", MSC_FRIENDSTATUSON},
        {"friendstatusoff", MSC_FRIENDSTATUSOFF},
        {"notintown", MSC_NOTINTOWN},
        {"attackpcgt", MSC_ATTACKPCGT},
        {"attackpcge", MSC_ATTACKPCGE},
        {"slavelt", MSC_SLAVELT},
        {"slavele", MSC_SLAVELE},
        {"closedattacked", MSC_CLOSEDATTACKED},
        {"longrangeattacked", MSC_LONGRANGEATTACKED},
        {"skillused", MSC_SKILLUSED},
        {"casttargeted", MSC_CASTTARGETED},
    };
    const struct
    {
        char str[32];
        StatusChange id;
    } cond2[] =
    {
        {"anybad", StatusChange::ANY_BAD},
        {"stone", SC_STONE},
        {"freeze", SC_FREEZE},
        {"stan", SC_STAN},
        {"sleep", SC_SLEEP},
        {"poison", SC_POISON},
        {"curse", SC_CURSE},
        {"silence", SC_SILENCE},
        {"confusion", SC_CONFUSION},
        {"blind", SC_BLIND},
        {"hiding", SC_HIDING},
        {"sight", SC_SIGHT},
    };
    const struct
    {
        char str[32];
        MSS id;
    } state[] =
    {
        {"any", MSS::ANY},
        {"idle", MSS_IDLE},
        {"walk", MSS_WALK},
        {"attack", MSS_ATTACK},
        {"dead", MSS_DEAD},
        {"loot", MSS_LOOT},
        {"chase", MSS_CHASE},
    };
    const struct
    {
        char str[32];
        MST id;
    } target[] =
    {
        {"target", MST_TARGET},
        {"self", MST_SELF},
        {"friend", MST_FRIEND},
        {"master", MST_MASTER},
        {"around5", MST_AROUND5},
        {"around6", MST_AROUND6},
        {"around7", MST_AROUND7},
        {"around8", MST_AROUND8},
        {"around1", MST_AROUND1},
        {"around2", MST_AROUND2},
        {"around3", MST_AROUND3},
        {"around4", MST_AROUND4},
        {"around", MST_AROUND},
    };

    int x;
    const char *filename[] = { "db/mob_skill_db.txt", "db/mob_skill_db2.txt" };

    for (x = 0; x < 2; x++)
    {

        fp = fopen_(filename[x], "r");
        if (fp == NULL)
        {
            if (x == 0)
                PRINTF("can't read %s\n", filename[x]);
            continue;
        }
        while (fgets(line, 1020, fp))
        {
            char *sp[20], *p;
            int mob_id;
            // always initialized, but clang is not smart enough yet
            struct mob_skill *ms = nullptr;
            int j = 0;

            if (line[0] == '/' && line[1] == '/')
                continue;

            memset(sp, 0, sizeof(sp));
            for (i = 0, p = line; i < 18 && p; i++)
            {
                sp[i] = p;
                if ((p = strchr(p, ',')) != NULL)
                    *p++ = 0;
            }
            if ((mob_id = atoi(sp[0])) <= 0)
                continue;

            if (strcmp(sp[1], "clear") == 0)
            {
                memset(mob_db[mob_id].skill, 0,
                        sizeof(mob_db[mob_id].skill));
                mob_db[mob_id].maxskill = 0;
                continue;
            }

            for (i = 0; i < MAX_MOBSKILL; i++)
                if ((ms = &mob_db[mob_id].skill[i])->skill_id == SkillID::ZERO)
                    break;
            if (i == MAX_MOBSKILL)
            {
                PRINTF("mob_skill: readdb: too many skill ! [%s] in %d[%s]\n",
                     sp[1], mob_id, mob_db[mob_id].jname);
                continue;
            }

            ms->state = MSS(atoi(sp[2]));
            for (j = 0; j < sizeof(state) / sizeof(state[0]); j++)
            {
                if (strcmp(sp[2], state[j].str) == 0)
                    ms->state = state[j].id;
            }
            ms->skill_id = SkillID(atoi(sp[3]));
            ms->skill_lv = atoi(sp[4]);

            ms->permillage = atoi(sp[5]);
            ms->casttime = atoi(sp[6]);
            ms->delay = atoi(sp[7]);
            ms->cancel = atoi(sp[8]);
            if (strcmp(sp[8], "yes") == 0)
                ms->cancel = 1;
            ms->target = MST(atoi(sp[9]));
            for (j = 0; j < sizeof(target) / sizeof(target[0]); j++)
            {
                if (strcmp(sp[9], target[j].str) == 0)
                    ms->target = target[j].id;
            }
            ms->cond1 = MSC::ANY;
            for (j = 0; j < sizeof(cond1) / sizeof(cond1[0]); j++)
            {
                if (strcmp(sp[10], cond1[j].str) == 0)
                    ms->cond1 = cond1[j].id;
            }
            // sometimes legitimately an integer
            // in fact, with current data it always is. Yay!
            ms->cond2i = atoi(sp[11]);
            for (j = 0; j < sizeof(cond2) / sizeof(cond2[0]); j++)
            {
                if (strcmp(sp[11], cond2[j].str) == 0)
                    ms->cond2i = int(cond2[j].id);
            }
            ms->val[0] = atoi(sp[12]);
            ms->val[1] = atoi(sp[13]);
            ms->val[2] = atoi(sp[14]);
            ms->val[3] = atoi(sp[15]);
            ms->val[4] = atoi(sp[16]);
            if (sp[17] != NULL && strlen(sp[17]) > 2)
                ms->emotion = atoi(sp[17]);
            else
                ms->emotion = -1;
            mob_db[mob_id].maxskill = i + 1;
        }
        fclose_(fp);
        PRINTF("read %s done\n", filename[x]);
    }
    return 0;
}

void mob_reload(void)
{
    /*
     *
     * <empty monster database>
     * mob_read();
     *
     */

    do_init_mob();
}

/*==========================================
 * Circumference initialization of mob
 *------------------------------------------
 */
int do_init_mob(void)
{
    mob_readdb();

    mob_read_randommonster();
    mob_readskilldb();

    add_timer_interval(gettick() + MIN_MOBTHINKTIME, mob_ai_hard, 0, 0,
                        MIN_MOBTHINKTIME);
    add_timer_interval(gettick() + MIN_MOBTHINKTIME * 10, mob_ai_lazy, 0, 0,
                        MIN_MOBTHINKTIME * 10);

    return 0;
}
