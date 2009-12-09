/*
 *  File:       beam.cc
 *  Summary:    Functions related to ranged attacks.
 *  Written by: Linley Henzell
 */

#include "AppHdr.h"

#include "beam.h"

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <iostream>
#include <set>
#include <algorithm>
#include <cmath>

#include "externs.h"
#include "options.h"

#include "areas.h"
#include "attitude-change.h"
#include "cio.h"
#include "cloud.h"
#include "colour.h"
#include "coord.h"
#include "coordit.h"
#include "delay.h"
#include "dgnevent.h"
#include "effects.h"
#include "env.h"
#include "enum.h"
#include "map_knowledge.h"
#include "fprop.h"
#include "fight.h"
#include "item_use.h"
#include "it_use2.h"
#include "items.h"
#include "itemname.h"
#include "itemprop.h"
#include "los.h"
#include "message.h"
#include "misc.h"
#include "mon-behv.h"
#include "mon-iter.h"
#include "mon-place.h"
#include "mgen_data.h"
#include "mon-stuff.h"
#include "mon-util.h"
#include "mutation.h"
#include "ouch.h"
#include "player.h"
#include "religion.h"
#include "skills.h"
#include "spells1.h"
#include "spells3.h"
#include "spells4.h"
#include "state.h"
#include "stuff.h"
#include "teleport.h"
#include "terrain.h"
#include "transfor.h"
#include "traps.h"
#include "view.h"
#include "shout.h"
#include "viewchar.h"
#include "viewgeom.h"
#include "xom.h"

#include "tiles.h"

#define BEAM_STOP       1000        // all beams stopped by subtracting this
                                    // from remaining range

// Helper functions (some of these should probably be public).
static void _ench_animation(int flavour, const monsters *mon = NULL,
                            bool force = false);
static void _zappy(zap_type z_type, int power, bolt &pbolt);
static beam_type _chaos_beam_flavour();

tracer_info::tracer_info()
{
    reset();
}

void tracer_info::reset()
{
    count = power = hurt = helped = 0;
    dont_stop = false;
}

const tracer_info& tracer_info::operator+=(const tracer_info &other)
{
    count  += other.count;
    power  += other.power;
    hurt   += other.hurt;
    helped += other.helped;

    dont_stop = dont_stop || other.dont_stop;

    return (*this);
}

bool bolt::is_blockable() const
{
    // BEAM_ELECTRICITY is added here because chain lightning is not
    // a true beam (stops at the first target it gets to and redirects
    // from there)... but we don't want it shield blockable.
    return (!is_beam && !is_explosion && flavour != BEAM_ELECTRICITY);
}

void bolt::emit_message(msg_channel_type chan, const char* m)
{
    const std::string message = m;
    if (message_cache.find(message) == message_cache.end())
        mpr(m, chan);

    message_cache.insert(message);
}

kill_category bolt::whose_kill() const
{
    if (YOU_KILL(thrower))
        return (KC_YOU);
    else if (MON_KILL(thrower))
    {
        if (beam_source == ANON_FRIENDLY_MONSTER)
            return (KC_FRIENDLY);
        if (!invalid_monster_index(beam_source))
        {
            const monsters *mon = &menv[beam_source];
            if (mon->friendly())
                return (KC_FRIENDLY);
        }
    }
    return (KC_OTHER);
}

// A simple animated flash from Rupert Smith (expanded to be more
// generic).
static void _zap_animation(int colour, const monsters *mon = NULL,
                           bool force = false)
{
    coord_def p = you.pos();

    if (mon)
    {
        if (!force && !mon->visible_to(&you))
            return;

        p = mon->pos();
    }

    if (!you.see_cell(p))
        return;

    const coord_def drawp = grid2view(p);

    if (in_los_bounds(drawp))
    {
        // Default to whatever colour magic is today.
        if (colour == -1)
            colour = ETC_MAGIC;

#ifdef USE_TILE
        tiles.add_overlay(p, tileidx_zap(colour));
#else
        view_update();
        cgotoxy(drawp.x, drawp.y, GOTO_DNGN);
        put_colour_ch(colour, dchar_glyph(DCHAR_FIRED_ZAP));
#endif

        update_screen();

        int zap_delay = 50;
        // Scale delay to match change in arena_delay.
        if (crawl_state.arena)
        {
            zap_delay *= Options.arena_delay;
            zap_delay /= 600;
        }

        delay(zap_delay);
    }
}

// Special front function for zap_animation to interpret enchantment flavours.
static void _ench_animation(int flavour, const monsters *mon, bool force)
{
    element_type elem;
    switch (flavour)
    {
    case BEAM_HEALING:
        elem = ETC_HEAL;
        break;
    case BEAM_PAIN:
        elem = ETC_UNHOLY;
        break;
    case BEAM_DISPEL_UNDEAD:
        elem = ETC_HOLY;
        break;
    case BEAM_POLYMORPH:
        elem = ETC_MUTAGENIC;
        break;
    case BEAM_CHAOS:
        elem = ETC_RANDOM;
        break;
    case BEAM_TELEPORT:
    case BEAM_BANISH:
    case BEAM_BLINK:
    case BEAM_BLINK_CLOSE:
        elem = ETC_WARP;
        break;
    default:
        elem = ETC_ENCHANT;
        break;
    }

    _zap_animation(element_colour(elem), mon, force);
}

// If needs_tracer is true, we need to check the beam path for friendly
// monsters.
bool zapping(zap_type ztype, int power, bolt &pbolt,
             bool needs_tracer, const char* msg)
{
#if DEBUG_DIAGNOSTICS
    mprf(MSGCH_DIAGNOSTICS, "zapping: power=%d", power);
#endif

    pbolt.thrower = KILL_YOU_MISSILE;

    // Check whether tracer goes through friendlies.
    // NOTE: Whenever zapping() is called with a randomised value for power
    // (or effect), player_tracer should be called directly with the highest
    // power possible respecting current skill, experience level, etc.
    if (needs_tracer && !player_tracer(ztype, power, pbolt))
        return (false);

    // Fill in the bolt structure.
    _zappy(ztype, power, pbolt);

    if (msg)
        mpr(msg);

    if (ztype == ZAP_LIGHTNING)
    {
        noisy(25, you.pos(), "You hear a mighty clap of thunder!");
        pbolt.heard = true;
    }

    if (ztype == ZAP_DIGGING)
        pbolt.aimed_at_spot = false;

    pbolt.fire();

    return (true);
}

// Returns true if the path is considered "safe", and false if there are
// monsters in the way the player doesn't want to hit.
// NOTE: Doesn't check for the player being hit by a rebounding lightning bolt.
bool player_tracer( zap_type ztype, int power, bolt &pbolt, int range)
{
    // Non-controlleable during confusion.
    // (We'll shoot in a different direction anyway.)
    if (you.confused())
        return (true);

    _zappy(ztype, power, pbolt);
    pbolt.name = "unimportant";

    pbolt.is_tracer      = true;
    pbolt.source         = you.pos();
    pbolt.can_see_invis  = you.can_see_invisible();
    pbolt.smart_monster  = true;
    pbolt.attitude       = ATT_FRIENDLY;
    pbolt.thrower        = KILL_YOU_MISSILE;

    // Init tracer variables.
    pbolt.friend_info.reset();
    pbolt.foe_info.reset();

    pbolt.foe_ratio        = 100;
    pbolt.beam_cancelled   = false;
    pbolt.dont_stop_player = false;

    // Clear misc
    pbolt.seen          = false;
    pbolt.heard         = false;
    pbolt.reflections   = 0;
    pbolt.bounces       = 0;

    // Save range before overriding it
    const int old_range = pbolt.range;
    if (range)
        pbolt.range = range;

    pbolt.fire();

    if (range)
        pbolt.range = old_range;

    // Should only happen if the player answered 'n' to one of those
    // "Fire through friendly?" prompts.
    if (pbolt.beam_cancelled)
    {
#if DEBUG_DIAGNOSTICS
        mprf(MSGCH_DIAGNOSTICS, "%s", "Beam cancelled.");
#endif
        canned_msg(MSG_OK);
        you.turn_is_over = false;
        return (false);
    }

    // Set to non-tracing for actual firing.
    pbolt.is_tracer = false;
    return (true);
}

template<typename T>
struct power_deducer
{
    virtual T operator()(int pow) const = 0;
    virtual ~power_deducer() {}
};

typedef power_deducer<int> tohit_deducer;

template<int adder, int mult_num = 0, int mult_denom = 1>
struct tohit_calculator : public tohit_deducer
{
    int operator()(int pow) const
    {
        return adder + (pow * mult_num) / mult_denom;
    }
};

typedef power_deducer<dice_def> dam_deducer;

template<int numdice, int adder, int mult_num, int mult_denom>
struct dicedef_calculator : public dam_deducer
{
    dice_def operator()(int pow) const
    {
        return dice_def(numdice, adder + (pow * mult_num) / mult_denom);
    }
};

template<int numdice, int adder, int mult_num, int mult_denom>
struct calcdice_calculator : public dam_deducer
{
    dice_def operator()(int pow) const
    {
        return calc_dice(numdice, adder + (pow * mult_num) / mult_denom);
    }
};

struct zap_info
{
    zap_type ztype;
    const char* name;           // NULL means handled specially
    int power_cap;
    dam_deducer* damage;
    tohit_deducer* tohit;       // Enchantments have power modifier here
    int colour;
    bool is_enchantment;
    beam_type flavour;
    dungeon_char_type glyph;
    bool always_obvious;
    bool can_beam;
    bool is_explosion;
    int hit_loudness;
};

const zap_info zap_data[] = {

    {
        ZAP_FLAME,
        "puff of flame",
        50,
        new dicedef_calculator<2, 4, 1, 10>,
        new tohit_calculator<8, 1, 10>,
        RED,
        false,
        BEAM_FIRE,
        DCHAR_FIRED_ZAP,
        true,
        false,
        false,
        2
    },

    {
        ZAP_FROST,
        "puff of frost",
        50,
        new dicedef_calculator<2, 4, 1, 10>,
        new tohit_calculator<8, 1, 10>,
        WHITE,
        false,
        BEAM_COLD,
        DCHAR_FIRED_ZAP,
        true,
        false,
        false,
        2
    },

    {
        ZAP_SLOWING,
        "0",
        100,
        NULL,
        NULL,
        BLACK,
        true,
        BEAM_SLOW,
        DCHAR_SPACE,
        false,
        false,
        false,
        0
    },

    {
        ZAP_HASTING,
        "0",
        100,
        NULL,
        NULL,
        BLACK,
        true,
        BEAM_HASTE,
        DCHAR_SPACE,
        false,
        false,
        false,
        0
    },

    {
        ZAP_MAGIC_DARTS,
        "magic dart",
        25,
        new dicedef_calculator<1, 3, 1, 5>,
        new tohit_calculator<AUTOMATIC_HIT>,
        LIGHTMAGENTA,
        false,
        BEAM_MMISSILE,
        DCHAR_FIRED_ZAP,
        true,
        false,
        false,
        1
    },

    {
        ZAP_HEALING,
        "0",
        100,
        new dicedef_calculator<1, 7, 1, 3>,
        NULL,
        BLACK,
        true,
        BEAM_HEALING,
        DCHAR_SPACE,
        false,
        false,
        false,
        0
    },

    {
        ZAP_PARALYSIS,
        "0",
        100,
        NULL,
        NULL,
        BLACK,
        true,
        BEAM_PARALYSIS,
        DCHAR_SPACE,
        false,
        false,
        false,
        0
    },

    {
        ZAP_FIRE,
        "bolt of fire",
        200,
        new calcdice_calculator<6, 18, 2, 3>,
        new tohit_calculator<10, 1, 25>,
        RED,
        false,
        BEAM_FIRE,
        DCHAR_FIRED_ZAP,
        true,
        true,
        false,
        6
    },

    {
        ZAP_COLD,
        "bolt of cold",
        200,
        new calcdice_calculator<6, 18, 2, 3>,
        new tohit_calculator<10, 1, 25>,
        WHITE,
        false,
        BEAM_COLD,
        DCHAR_FIRED_ZAP,
        true,
        true,
        false,
        6
    },

    {
        ZAP_CONFUSION,
        "0",
        100,
        NULL,
        NULL,
        BLACK,
        true,
        BEAM_CONFUSION,
        DCHAR_SPACE,
        false,
        false,
        false,
        0
    },

    {
        ZAP_INVISIBILITY,
        "0",
        100,
        NULL,
        NULL,
        BLACK,
        true,
        BEAM_INVISIBILITY,
        DCHAR_SPACE,
        false,
        false,
        false,
        0
    },

    {
        ZAP_DIGGING,
        "0",
        100,
        NULL,
        NULL,
        BLACK,
        true,
        BEAM_DIGGING,
        DCHAR_SPACE,
        false,
        true,
        false,
        0
    },

    {
        ZAP_FIREBALL,
        "fireball",
        200,
        new calcdice_calculator<3, 10, 1, 2>,
        new tohit_calculator<40>,
        RED,
        false,
        BEAM_FIRE,
        DCHAR_FIRED_ZAP,
        false,
        false,
        true,
        0 // Noise comes from explosion
    },

    {
        ZAP_TELEPORTATION,
        "0",
        100,
        NULL,
        NULL,
        BLACK,
        true,
        BEAM_TELEPORT,
        DCHAR_SPACE,
        false,
        false,
        false,
        0
    },

    {
        ZAP_LIGHTNING,
        "bolt of lightning",
        200,
        new calcdice_calculator<1, 10, 3, 5>,
        new tohit_calculator<7, 1, 40>,
        LIGHTCYAN,
        false,
        BEAM_ELECTRICITY,
        DCHAR_FIRED_ZAP,
        true,
        true,
        false,
        5 // XXX: Maybe louder?
    },

    {
        ZAP_POLYMORPH_OTHER,
        "0",
        100,
        NULL,
        NULL,
        BLACK,
        true,
        BEAM_POLYMORPH,
        DCHAR_SPACE,
        false,
        false,
        false,
        0
    },

    {
        ZAP_VENOM_BOLT,
        "bolt of poison",
        200,
        new calcdice_calculator<4, 15, 1, 2>,
        new tohit_calculator<8, 1, 20>,
        LIGHTGREEN,
        false,
        BEAM_POISON,
        DCHAR_FIRED_ZAP,
        true,
        true,
        false,
        5 // XXX: Quieter because it's poison?
    },

    {
        ZAP_NEGATIVE_ENERGY,
        "bolt of negative energy",
        200,
        new calcdice_calculator<4, 15, 3, 5>,
        new tohit_calculator<8, 1, 20>,
        DARKGREY,
        false,
        BEAM_NEG,
        DCHAR_FIRED_ZAP,
        true,
        true,
        false,
        0 // Draining is soundless
    },

    {
        ZAP_CRYSTAL_SPEAR,
        "crystal spear",
        200,
        new calcdice_calculator<10, 23, 1, 1>,
        new tohit_calculator<10, 1, 15>,
        WHITE,
        false,
        BEAM_MMISSILE,
        DCHAR_FIRED_MISSILE,
        true,
        false,
        false,
        8
    },

    {
        ZAP_BEAM_OF_ENERGY,
        "narrow beam of energy",
        1000,
        new calcdice_calculator<12, 40, 3, 2>,
        new tohit_calculator<1>,
        YELLOW,
        false,
        BEAM_ENERGY,
        DCHAR_FIRED_ZAP,
        true,
        true,
        false,
        3
    },

    {
        ZAP_MYSTIC_BLAST,
        "orb of energy",
        100,
        new calcdice_calculator<2, 15, 2, 5>,
        new tohit_calculator<10, 1, 7>,
        LIGHTMAGENTA,
        false,
        BEAM_MMISSILE,
        DCHAR_FIRED_ZAP,
        true,
        false,
        false,
        4
    },

    {
        ZAP_ENSLAVEMENT,
        "0",
        100,
        NULL,
        NULL,
        BLACK,
        true,
        BEAM_CHARM,
        DCHAR_SPACE,
        false,
        false,
        false,
        0
    },

    {
        ZAP_PAIN,
        "0",
        100,
        new dicedef_calculator<1, 4, 1,5>,
        new tohit_calculator<0, 7, 2>,
        BLACK,
        true,
        BEAM_PAIN,
        DCHAR_SPACE,
        false,
        false,
        false,
        1 // XXX: Should this be soundless?
    },

    {
        ZAP_STICKY_FLAME,
        "sticky flame",
        100,
        new dicedef_calculator<2, 3, 1, 12>,
        new tohit_calculator<11, 1, 10>,
        RED,
        false,
        BEAM_FIRE,
        DCHAR_FIRED_ZAP,
        true,
        false,
        false,
        4 // XXX: Would sticky flame really be this noisy?
    },

    {
        ZAP_DISPEL_UNDEAD,
        "0",
        100,
        new calcdice_calculator<3, 20, 3, 4>,
        new tohit_calculator<0, 3, 2>,
        BLACK,
        true,
        BEAM_DISPEL_UNDEAD,
        DCHAR_SPACE,
        false,
        false,
        false,
        0
    },

    {
        ZAP_BONE_SHARDS,
        "spray of bone shards",
        // Incoming power is highly dependent on mass (see spells3.cc).
        // Basic function is power * 15 + mass...  with the largest
        // available mass (3000) we get a power of 4500 at a power
        // level of 100 (for 3d20).
        10000,
        new dicedef_calculator<3, 2, 1, 250>,
        new tohit_calculator<8, 1, 100>,
        LIGHTGREY,
        false,
        BEAM_MAGIC,
        DCHAR_FIRED_ZAP,
        true,
        true,
        false,
        3
    },

    {
        ZAP_BANISHMENT,
        "0",
        100,
        NULL,
        NULL,
        BLACK,
        true,
        BEAM_BANISH,
        DCHAR_SPACE,
        false,
        false,
        false,
        0
    },

    {
        ZAP_DEGENERATION,
        "0",
        100,
        NULL,
        NULL,
        BLACK,
        true,
        BEAM_DEGENERATE,
        DCHAR_SPACE,
        false,
        false,
        false,
        0 // XXX: How loud should this be?
    },

    {
        ZAP_STING,
        "sting",
        25,
        new dicedef_calculator<1, 3, 1, 5>,
        new tohit_calculator<8, 1, 5>,
        GREEN,
        false,
        BEAM_POISON,
        DCHAR_FIRED_ZAP,
        true,
        false,
        false,
        1 // XXX: Maybe silent because it's poison?
    },

    {
        ZAP_HELLFIRE,
        "hellfire",
        200,
        new calcdice_calculator<3, 10, 3, 4>,
        new tohit_calculator<20, 1, 10>,
        RED,
        false,
        BEAM_HELLFIRE,
        DCHAR_FIRED_ZAP,
        true,
        false,
        true,
        9 // XXX: Even louder because it's hellish?
    },

    {
        ZAP_IRON_SHOT,
        "iron shot",
        200,
        new calcdice_calculator<9, 15, 3, 4>,
        new tohit_calculator<7, 1, 15>,
        LIGHTCYAN,
        false,
        BEAM_MMISSILE,
        DCHAR_FIRED_MISSILE,
        true,
        false,
        false,
        6
    },

    {
        ZAP_STRIKING,
        "force bolt",
        25,
        new dicedef_calculator<1, 5, 0, 1>,
        new tohit_calculator<8, 1, 10>,
        BLACK,
        false,
        BEAM_MMISSILE,
        DCHAR_SPACE,
        true,
        false,
        false,
        4 // XXX: this is just a guess.
    },

    {
        ZAP_STONE_ARROW,
        "stone arrow",
        50,
        new dicedef_calculator<2, 5, 1, 7>,
        new tohit_calculator<8, 1, 10>,
        LIGHTGREY,
        false,
        BEAM_MMISSILE,
        DCHAR_FIRED_MISSILE,
        true,
        false,
        false,
        3
    },

    {
        ZAP_ELECTRICITY,
        "zap",
        25,
        new dicedef_calculator<1, 3, 1, 4>,
        new tohit_calculator<8, 1, 7>,
        LIGHTCYAN,
        false,
        BEAM_ELECTRICITY,             // beams & reflects
        DCHAR_FIRED_ZAP,
        true,
        true,
        false,
        1 // XXX: maybe electricity should be louder?
    },

    {
        ZAP_ORB_OF_ELECTRICITY,
        "orb of electricity",
        200,
        new calcdice_calculator<0, 15, 4, 5>,
        new tohit_calculator<40>,
        LIGHTBLUE,
        false,
        BEAM_ELECTRICITY,
        DCHAR_FIRED_ZAP,
        true,
        false,
        true,
        6 // XXX: maybe electricity should be louder?
    },

    {
        ZAP_SPIT_POISON,
        "splash of poison",
        50,
        new dicedef_calculator<1, 4, 1, 2>,
        new tohit_calculator<5, 1, 6>,
        GREEN,
        false,
        BEAM_POISON,
        DCHAR_FIRED_ZAP,
        true,
        false,
        false,
        1
    },

    {
        ZAP_DEBUGGING_RAY,
        "debugging ray",
        10000,
        new dicedef_calculator<1500, 1, 0, 1>,
        new tohit_calculator<1500>,
        WHITE,
        false,
        BEAM_MMISSILE,
        DCHAR_FIRED_DEBUG,
        false,
        false,
        false,
        0
    },

    // XXX: How loud should breath be?
    {
        ZAP_BREATHE_FIRE,
        "fiery breath",
        50,
        new dicedef_calculator<3, 4, 1, 3>,
        new tohit_calculator<8, 1, 6>,
        RED,
        false,
        BEAM_FIRE,
        DCHAR_FIRED_ZAP,
        true,
        true,
        false,
        6
    },


    {
        ZAP_BREATHE_FROST,
        "freezing breath",
        50,
        new dicedef_calculator<3, 4, 1, 3>,
        new tohit_calculator<8, 1, 6>,
        WHITE,
        false,
        BEAM_COLD,
        DCHAR_FIRED_ZAP,
        true,
        true,
        false,
        6
    },

    {
        ZAP_BREATHE_ACID,
        "acid",
        50,
        new dicedef_calculator<3, 3, 1, 3>,
        new tohit_calculator<5, 1, 6>,
        YELLOW,
        false,
        BEAM_ACID,
        DCHAR_FIRED_ZAP,
        true,
        true,
        false,
        6
    },

    {
        ZAP_BREATHE_POISON,
        "poison gas",
        50,
        new dicedef_calculator<3, 2, 1, 6>,
        new tohit_calculator<6, 1, 6>,
        GREEN,
        false,
        BEAM_POISON,
        DCHAR_FIRED_ZAP,
        true,
        true,
        false,
        0 // Explosion does the noise.
    },

    {
        ZAP_BREATHE_POWER,
        "bolt of energy",
        50,
        new dicedef_calculator<3, 3, 1, 3>,
        new tohit_calculator<5, 1, 6>,
        BLUE,
        false,
        BEAM_MMISSILE,
        DCHAR_FIRED_ZAP,
        true,
        true,
        false,
        6
    },

    {
        ZAP_ENSLAVE_UNDEAD,
        "0",
        100,
        NULL,
        NULL,
        BLACK,
        true,
        BEAM_ENSLAVE_UNDEAD,
        DCHAR_SPACE,
        false,
        false,
        false,
        0
    },

    {
        ZAP_ENSLAVE_SOUL,
        "0",
        100,
        NULL,
        NULL,
        BLACK,
        true,
        BEAM_ENSLAVE_SOUL,
        DCHAR_SPACE,
        false,
        false,
        false,
        0
    },

    {
        ZAP_AGONY,
        "0agony",
        100,
        NULL,
        new tohit_calculator<0, 5, 1>,
        BLACK,
        true,
        BEAM_PAIN,
        DCHAR_SPACE,
        false,
        false,
        false,
        0
    },

    {
        ZAP_DISINTEGRATION,
        "0",
        100,
        new calcdice_calculator<3, 15, 3, 4>,
        new tohit_calculator<0, 5, 2>,
        BLACK,
        true,
        BEAM_DISINTEGRATION,
        DCHAR_SPACE,
        false,
        true,
        false,
        6
    },

    {
        ZAP_BREATHE_STEAM,
        "ball of steam",
        50,
        new dicedef_calculator<3, 4, 1, 5>,
        new tohit_calculator<10, 1, 10>,
        LIGHTGREY,
        false,
        BEAM_STEAM,
        DCHAR_FIRED_ZAP,
        true,
        true,
        false,
        0 // Explosion does the noise.
    },

    {
        ZAP_CONTROL_DEMON,
        "0",
        100,
        NULL,
        new tohit_calculator<0, 3, 2>,
        BLACK,
        true,
        BEAM_ENSLAVE_DEMON,
        DCHAR_SPACE,
        false,
        false,
        false,
        0
    },

    {
        ZAP_ORB_OF_FRAGMENTATION,
        "metal orb",
        200,
        new calcdice_calculator<3, 30, 3, 4>,
        new tohit_calculator<20>,
        CYAN,
        false,
        BEAM_FRAG,
        DCHAR_FIRED_ZAP,
        false,
        false,
        true,
        5 // XXX: Seems like it might be louder than this.
    },

    {
        ZAP_THROW_ICICLE,
        "shard of ice",
        100,
        new calcdice_calculator<3, 10, 1, 2>,
        new tohit_calculator<9, 1, 12>,
        WHITE,
        false,
        BEAM_ICE,
        DCHAR_FIRED_ZAP,
        false,
        false,
        false,
        4
    },

    {                           // ench_power controls radius
        ZAP_ICE_STORM,
        "great blast of cold",
        200,
        new calcdice_calculator<7, 22, 1, 1>,
        new tohit_calculator<20, 1, 10>,
        BLUE,
        false,
        BEAM_ICE,
        DCHAR_FIRED_ZAP,
        true,
        false,
        true,
        9 // XXX: Should a storm be louder?
    },

    {
        ZAP_CORONA,
        "0",
        100,
        NULL,
        NULL,
        BLUE,
        true,
        BEAM_CORONA,
        DCHAR_SPACE,
        false,
        false,
        false,
        0
    },

    {
        ZAP_HIBERNATION,
        "0",
        100,
        NULL,
        NULL,
        BLACK,
        true,
        BEAM_HIBERNATION,
        DCHAR_SPACE,
        false,
        false,
        false,
        0
    },

    {
        ZAP_FLAME_TONGUE,
        "flame",
        25,
        new dicedef_calculator<1, 8, 1, 4>,
        new tohit_calculator<7, 1, 6>,
        RED,
        false,
        BEAM_FIRE,
        DCHAR_FIRED_BOLT,
        true,
        false,
        false,
        1
    },

    {
        ZAP_SANDBLAST,
        "rocky blast",
        50,
        new dicedef_calculator<2, 4, 1, 3>,
        new tohit_calculator<13, 1, 10>,
        BROWN,
        false,
        BEAM_FRAG,
        DCHAR_FIRED_BOLT,
        true,
        false,
        false,
        2 // XXX: Sound 2 for level one spell?
    },

    {
        ZAP_SMALL_SANDBLAST,
        "blast of sand",
        25,
        new dicedef_calculator<1, 8, 1, 4>,
        new tohit_calculator<8, 1, 5>,
        BROWN,
        false,
        BEAM_FRAG,
        DCHAR_FIRED_BOLT,
        true,
        false,
        false,
        1
    },

    {
        ZAP_MAGMA,
        "bolt of magma",
        200,
        new calcdice_calculator<4, 10, 3, 5>,
        new tohit_calculator<8, 1, 25>,
        RED,
        false,
        BEAM_LAVA,
        DCHAR_FIRED_ZAP,
        true,
        true,
        false,
        5
    },

    {
        ZAP_POISON_ARROW,
        "poison arrow",
        200,
        new calcdice_calculator<4, 15, 1, 1>,
        new tohit_calculator<5, 1, 10>,
        LIGHTGREEN,
        false,
        BEAM_POISON_ARROW,             // extra damage
        DCHAR_FIRED_MISSILE,
        true,
        false,
        false,
        6 // XXX: Less noise because it's poison?
    },

    {
        ZAP_PETRIFY,
        "0",
        100,
        NULL,
        NULL,
        BLACK,
        true,
        BEAM_PETRIFY,
        DCHAR_SPACE,
        false,
        false,
        false,
        0
    },

    {
        ZAP_PORKALATOR,
        "porkalator",
        100,
        NULL,
        NULL,
        RED,
        true,
        BEAM_PORKALATOR,
        DCHAR_SPACE,
        false,
        false,
        false,
        0
    },

    {
        ZAP_SLEEP,
        "0",
        100,
        NULL,
        NULL,
        BLACK,
        true,
        BEAM_SLEEP,
        DCHAR_SPACE,
        false,
        false,
        false,
        0
    }
};

static void _zappy(zap_type z_type, int power, bolt &pbolt)
{
    const zap_info* zinfo = NULL;

    // Find the appropriate zap info.
    for (unsigned int i = 0; i < ARRAYSZ(zap_data); ++i)
    {
        if (zap_data[i].ztype == z_type)
        {
            zinfo = &zap_data[i];
            break;
        }
    }

    // None found?
    if (zinfo == NULL)
    {
#ifdef DEBUG_DIAGNOSTICS
        mprf(MSGCH_ERROR, "Couldn't find zap type %d", z_type);
#endif
        return;
    }

    // Fill
    pbolt.name           = zinfo->name;
    pbolt.flavour        = zinfo->flavour;
    pbolt.real_flavour   = zinfo->flavour;
    pbolt.colour         = zinfo->colour;
    pbolt.type           = dchar_glyph(zinfo->glyph);
    pbolt.obvious_effect = zinfo->always_obvious;
    pbolt.is_beam        = zinfo->can_beam;
    pbolt.is_explosion   = zinfo->is_explosion;

    if (zinfo->power_cap > 0)
        power = std::min(zinfo->power_cap, power);

    ASSERT(zinfo->is_enchantment == pbolt.is_enchantment());

    if (zinfo->is_enchantment)
    {
        pbolt.ench_power = (zinfo->tohit ? (*zinfo->tohit)(power) : power);
        pbolt.hit = AUTOMATIC_HIT;
    }
    else
    {
        pbolt.hit = (*zinfo->tohit)(power);
        if (wearing_amulet(AMU_INACCURACY))
            pbolt.hit = std::max(0, pbolt.hit - 5);
    }

    if (zinfo->damage)
        pbolt.damage = (*zinfo->damage)(power);

    // One special case
    if (z_type == ZAP_ICE_STORM)
        pbolt.ench_power = power; // used for radius

    if (pbolt.loudness == 0)
        pbolt.loudness = zinfo->hit_loudness;
}

// Affect monster in wall unless it can shield itself using the wall.
// The wall will always shield the monster if the beam bounces off the
// wall, and a monster can't use a metal wall to shield itself from
// electricity.
bool bolt::can_affect_wall_monster(const monsters* mon) const
{
    if (is_enchantment())
        return (true);

    const bool superconductor = (grd(mon->pos()) == DNGN_METAL_WALL
                                 && flavour == BEAM_ELECTRICITY);
    if (mons_wall_shielded(mon) && !superconductor)
        return (false);

    if (!is_explosion && !is_big_cloud)
        return (true);

    if (is_bouncy(grd(mon->pos())))
        return (false);

    return (false);
}

static beam_type _chaos_beam_flavour()
{
    const beam_type flavour = static_cast<beam_type>(
        random_choose_weighted(
            10, BEAM_FIRE,
            10, BEAM_COLD,
            10, BEAM_ELECTRICITY,
            10, BEAM_POISON,
            10, BEAM_NEG,
            10, BEAM_ACID,
            10, BEAM_HELLFIRE,
            10, BEAM_NAPALM,
            10, BEAM_SLOW,
            10, BEAM_HASTE,
            10, BEAM_MIGHT,
            10, BEAM_BERSERK,
            10, BEAM_HEALING,
            10, BEAM_PARALYSIS,
            10, BEAM_CONFUSION,
            10, BEAM_INVISIBILITY,
            10, BEAM_POLYMORPH,
            10, BEAM_BANISH,
            10, BEAM_DISINTEGRATION,
            0));

    return (flavour);
}

static void _munge_bounced_bolt(bolt &old_bolt, bolt &new_bolt,
                                ray_def &old_ray, ray_def &new_ray)
{
    if (new_bolt.real_flavour != BEAM_CHAOS)
        return;

    double old_deg = old_ray.get_degrees();
    double new_deg = new_ray.get_degrees();
    double angle   = fabs(old_deg - new_deg);

    if (angle >= 180.0)
        angle -= 180.0;

    double max =  90.0 + (angle / 2.0);
    double min = -90.0 + (angle / 2.0);

    double shift;

    ray_def temp_ray = new_ray;
    for (int tries = 0; tries < 20; tries++)
    {
        shift = (double) random_range((int)(min * 10000),
                                      (int)(max * 10000)) / 10000.0;

        if (new_deg < old_deg)
            shift = -shift;
        temp_ray.set_degrees(new_deg + shift);

        // Don't bounce straight into another wall.  Can happen if the beam
        // is shot into an inside corner.
        ray_def test_ray = temp_ray;
        test_ray.advance();
        if (in_bounds(test_ray.pos()) && !cell_is_solid(test_ray.pos()))
            break;

        shift    = 0.0;
        temp_ray = new_ray;
    }

    new_ray = temp_ray;
#if DEBUG_DIAGNOSTICS || DEBUG_BEAM || DEBUG_CHAOS_BOUNCE
    mprf(MSGCH_DIAGNOSTICS,
         "chaos beam: old_deg = %5.2f, new_deg = %5.2f, shift = %5.2f",
         (float) old_deg, (float) new_deg, (float) shift);
#endif

    // Don't use up range in bouncing off walls, so that chaos beams have
    // as many chances as possible to bounce.  They're like demented
    // ping-pong balls on caffeine.
    int range_spent = new_bolt.range_used - old_bolt.range_used;
    new_bolt.range += range_spent;
}

bool bolt::invisible() const
{
    return (type == 0 || is_enchantment());
}

void bolt::initialise_fire()
{
    // Fix some things which the tracer might have set.
    range_used         = 0;
    in_explosion_phase = false;
    use_target_as_pos  = false;

    if (special_explosion != NULL)
    {
        ASSERT(!is_explosion);
        ASSERT(special_explosion->is_explosion);
        ASSERT(special_explosion->special_explosion == NULL);
        special_explosion->in_explosion_phase = false;
        special_explosion->use_target_as_pos  = false;
    }

    if (chose_ray)
    {
        ASSERT(in_bounds(ray.pos()));

        if (source == coord_def())
            source = ray.pos();
    }

    if (target == source)
    {
        range             = 0;
        aimed_at_feet     = true;
        auto_hit          = true;
        aimed_at_spot     = true;
        use_target_as_pos = true;
    }

    if (range == -1)
    {
#ifdef DEBUG
        if (is_tracer)
        {
            mpr("Tracer with range == -1, skipping.", MSGCH_ERROR);
            return;
        }

        std::string item_name   = item ? item->name(DESC_PLAIN, false, true)
                                       : "none";

        std::string dbg_source_name = "unknown";
        if (beam_source == NON_MONSTER && source == you.pos())
            dbg_source_name = "player";
        else if (!invalid_monster_index(beam_source))
            dbg_source_name = menv[beam_source].name(DESC_PLAIN, true);

        mprf(MSGCH_ERROR, "beam '%s' (source '%s', item '%s') has range -1; "
                          "setting to LOS_RADIUS",
             name.c_str(), dbg_source_name.c_str(), item_name.c_str());
#endif
        range = LOS_RADIUS;
    }

    ASSERT(!name.empty() || is_tracer);
    ASSERT(in_bounds(source));
    ASSERT(flavour > BEAM_NONE && flavour < BEAM_FIRST_PSEUDO);
    ASSERT(!drop_item || item && item->is_valid());
    ASSERT(range >= 0);
    ASSERT(!aimed_at_feet || source == target);

    real_flavour = flavour;

    message_cache.clear();

    // seen might be set by caller to supress this.
    if (!seen && you.see_cell(source) && range > 0 && !invisible() )
    {
        seen = true;
        const monsters* mon = monster_at(source);

        if (flavour != BEAM_VISUAL
            && !is_tracer
            && !YOU_KILL(thrower)
            && !crawl_state.is_god_acting()
            && (!mon || !mon->observable()))
        {
            mprf("%s appears from out of thin air!",
                 article_a(name, false).c_str());
        }
    }

    // Visible self-targeted beams are always seen, even though they don't
    // leave a path.
    if (you.see_cell(source) && target == source && !invisible())
        seen = true;

    // Scale draw_delay to match change in arena_delay.
    if (crawl_state.arena && !is_tracer)
    {
        draw_delay *= Options.arena_delay;
        draw_delay /= 600;
    }

#ifdef DEBUG_DIAGNOSTICS
    mprf( MSGCH_DIAGNOSTICS, "%s%s%s [%s] (%d,%d) to (%d,%d): "
          "ty=%d col=%d flav=%d hit=%d dam=%dd%d range=%d",
          (is_beam) ? "beam" : "missile",
          (is_explosion) ? "*" :
          (is_big_cloud) ? "+" : "",
          (is_tracer) ? " tracer" : "",
          name.c_str(),
          source.x, source.y,
          target.x, target.y,
          type, colour, flavour,
          hit, damage.num, damage.size,
          range);
#endif
}

void bolt::apply_beam_conducts()
{
    if (!is_tracer && YOU_KILL(thrower))
    {
        switch (flavour)
        {
        case BEAM_HELLFIRE:
            did_god_conduct(DID_UNHOLY, 2 + random2(3), effect_known);
            break;
        default:
            break;
        }
    }
}

void bolt::choose_ray()
{
    if (!chose_ray || reflections > 0)
    {
        if (!find_ray(source, target, ray))
            fallback_ray(source, target, ray);
    }
}

// Draw the bolt at p if needed.
void bolt::draw(const coord_def& p)
{
    if (is_tracer || is_enchantment() || !you.see_cell(p))
        return;

    // We don't clean up the old position.
    // First, most people like to see the full path,
    // and second, it is hard to do it right with
    // respect to killed monsters, cloud trails, etc.

    const coord_def drawpos = grid2view(p);

#ifdef USE_TILE
    if (tile_beam == -1)
    {
        if (effect_known)
            tile_beam = tileidx_bolt(*this);
        else
            tile_beam = tileidx_zap(ETC_MAGIC);
    }

    if (tile_beam != -1 && in_los_bounds(drawpos))
    {
        tiles.add_overlay(p, tile_beam);
        delay(draw_delay);
    }
    else
#endif
    {
        // bounds check
        if (in_los_bounds(drawpos))
        {
#ifndef USE_TILE
            cgotoxy(drawpos.x, drawpos.y);
            put_colour_ch(colour == BLACK ? random_colour()
                                          : element_colour(colour),
                          type);
#endif
            // Get curses to update the screen so we can see the beam.
            update_screen();
            delay(draw_delay);
        }
    }
}

// Bounce a bolt off a solid feature.
// The ray is assumed to have just been advanced into
// the feature.
void bolt::bounce()
{
    ray_def old_ray  = ray;
    bolt    old_bolt = *this;

    do
        ray.regress();
    while (feat_is_solid(grd(ray.pos())));

    bounce_pos = ray.pos();
    reflect_grid rg;
    for (adjacent_iterator ai(ray.pos(), false); ai; ++ai)
        rg(*ai - ray.pos()) = feat_is_solid(grd(*ai));
    ray.bounce(rg);
    range_used += 2;

    ASSERT(!feat_is_solid(grd(ray.pos())));
    _munge_bounced_bolt(old_bolt, *this, old_ray, ray);
}

void bolt::fake_flavour()
{
    if (real_flavour == BEAM_RANDOM)
        flavour = static_cast<beam_type>(random_range(BEAM_FIRE, BEAM_ACID));
    else if (real_flavour == BEAM_CHAOS)
        flavour = _chaos_beam_flavour();
}

void bolt::digging_wall_effect()
{
    const dungeon_feature_type feat = grd(pos());
    if (feat == DNGN_ROCK_WALL || feat == DNGN_CLEAR_ROCK_WALL)
    {
        grd(pos()) = DNGN_FLOOR;
        // Mark terrain as changed so travel excludes can be updated
        // as necessary.
        // XXX: This doesn't work for some reason: after digging
        //      the wrong grids are marked excluded.
        set_terrain_changed(pos());

        // Blood does not transfer onto floor.
        if (is_bloodcovered(pos()))
            env.pgrid(pos()) &= ~(FPROP_BLOODY);

        if (!msg_generated)
        {
            if (!silenced(you.pos()))
            {
                mpr("You hear a grinding noise.", MSGCH_SOUND);
                obvious_effect = true;
            }

            msg_generated = true;
        }
    }
    else if (feat_is_wall(feat))
        finish_beam();
}

void bolt::fire_wall_effect()
{
    dungeon_feature_type feat;
    // Fire only affects wax walls and trees.
    if ((feat = grd(pos())) != DNGN_WAX_WALL && feat != DNGN_TREES)
    {
        finish_beam();
        return;
    }

    if (feat == DNGN_WAX_WALL)
    {
        if (!is_superhot())
        {
            // No actual effect.
            if (flavour != BEAM_HELLFIRE && feat == DNGN_WAX_WALL)
            {
                if (you.see_cell(pos()))
                {
                    emit_message(MSGCH_PLAIN,
                                 "The wax appears to soften slightly.");
                }
                else if (you.can_smell())
                    emit_message(MSGCH_PLAIN, "You smell warm wax.");
            }
        }
        else
        {
            // Destroy the wall.
            grd(pos()) = DNGN_FLOOR;
            if (you.see_cell(pos()))
                emit_message(MSGCH_PLAIN, "The wax bubbles and burns!");
            else if (you.can_smell())
                emit_message(MSGCH_PLAIN, "You smell burning wax.");
            place_cloud(CLOUD_FIRE, pos(), random2(10)+15, whose_kill(), killer());
            obvious_effect = true;
        }
    }
    else
    {
        if (is_superhot())
        {
            // Destroy the wall.
            grd(pos()) = DNGN_FLOOR;
            if (you.see_cell(pos()))
                emit_message(MSGCH_PLAIN, "The tree burns like a torch!");
            else if (you.can_smell())
                emit_message(MSGCH_PLAIN, "You smell burning wood.");
            if (whose_kill() == KC_YOU)
                did_god_conduct(DID_KILL_PLANT, 1, effect_known);
            else if (whose_kill() == KC_FRIENDLY)
                did_god_conduct(DID_ALLY_KILLED_PLANT, 1, effect_known, 0);
            place_cloud(CLOUD_FOREST_FIRE, pos(), random2(30)+25, whose_kill(), killer(), 5);
            obvious_effect = true;
        }
    }
    finish_beam();
}

void bolt::nuke_wall_effect()
{
    if (env.markers.property_at(pos(), MAT_ANY, "veto_disintegrate") == "veto")
    {
        finish_beam();
        return;
    }

    const dungeon_feature_type feat = grd(pos());

    if (feat == DNGN_ROCK_WALL
        || feat == DNGN_WAX_WALL
        || feat == DNGN_CLEAR_ROCK_WALL
        || feat == DNGN_GRANITE_STATUE)
    {
        // Blood does not transfer onto floor.
        if (is_bloodcovered(pos()))
            env.pgrid(pos()) &= ~(FPROP_BLOODY);

        grd(pos()) = DNGN_FLOOR;
        if (player_can_hear(pos()))
        {
            mpr("You hear a grinding noise.", MSGCH_SOUND);
            obvious_effect = true;
        }
    }
    else if (feat == DNGN_ORCISH_IDOL)
    {
        grd(pos()) = DNGN_FLOOR;

        // Blood does not transfer onto floor.
        if (is_bloodcovered(pos()))
            env.pgrid(pos()) &= ~(FPROP_BLOODY);

        if (player_can_hear(pos()))
        {
            if (!you.see_cell(pos()))
                mpr("You hear a hideous screaming!", MSGCH_SOUND);
            else
            {
                mpr("The idol screams as its substance crumbles away!",
                    MSGCH_SOUND);
            }
        }
        else if (you.see_cell(pos()))
            mpr("The idol twists and shakes as its substance crumbles away!");

        if (beam_source == NON_MONSTER)
            did_god_conduct(DID_DESTROY_ORCISH_IDOL, 8);

        obvious_effect = true;
    }
    finish_beam();
}

void bolt::finish_beam()
{
    range_used = range;
}

void bolt::affect_wall()
{
    if (is_tracer)
        return;

    if (flavour == BEAM_DIGGING)
        digging_wall_effect();
    else if (is_fiery())
        fire_wall_effect();
    else if (flavour == BEAM_DISINTEGRATION || flavour == BEAM_NUKE)
        nuke_wall_effect();

    if (cell_is_solid(pos()))
        finish_beam();
}

coord_def bolt::pos() const
{
    if (in_explosion_phase || use_target_as_pos)
        return target;
    else
        return ray.pos();
}

// Returns true if the beam ended due to hitting the wall.
bool bolt::hit_wall()
{
    const dungeon_feature_type feat = grd(pos());
    ASSERT( feat_is_solid(feat) );

    if (is_tracer && YOU_KILL(thrower) && in_bounds(target) && !passed_target
        && pos() != target  && pos() != source && foe_info.count == 0
        && flavour != BEAM_DIGGING && flavour <= BEAM_LAST_REAL
        && bounces == 0 && reflections == 0 && you.see_cell(target)
        && !feat_is_solid(grd(target)))
    {
        // Okay, with all those tests passed, this is probably an instance
        // of the player manually targetting something whose line of fire
        // is blocked, even though its line of sight isn't blocked.  Give
        // a warning about this fact.
        std::string prompt = "Your line of fire to ";
        const monsters* mon = monster_at(target);

        if (mon && mon->observable())
            prompt += mon->name(DESC_NOCAP_THE);
        else
        {
            prompt += "the targeted "
                    + feature_description(target, false, DESC_PLAIN, false);
        }

        prompt += " is blocked by "
                + feature_description(pos(), false, DESC_NOCAP_A, false);

        prompt += ". Continue anyway?";

        if (!yesno(prompt.c_str(), false, 'n'))
        {
            beam_cancelled = true;
            finish_beam();
            return (false);
        }

        // Well, we warned them.
    }

    // Press trigger/switch/button in wall if hit by something solid
    // or solid-ish.
    if (!is_explosion && !is_tracer && !monster_at(pos())
        && (flavour == BEAM_MISSILE || flavour == BEAM_MMISSILE))
    {
        dgn_event event(DET_WALL_HIT, pos());;
        event.arg1  = beam_source;

        dungeon_events.fire_vetoable_position_event(event, target);
    }

    if (affects_wall(feat))
        affect_wall();
    else if (is_bouncy(feat) && !in_explosion_phase)
        bounce();
    else
    {
        // Regress for explosions: blow up in an open grid (if regressing
        // makes any sense).  Also regress when dropping items.
        if (pos() != source
            && ((is_explosion && !in_explosion_phase) || drop_item))
        {
            do
                ray.regress();
            while (ray.pos() != source && cell_is_solid(ray.pos()));

            // target is where the explosion is centered, so update it.
            if (is_explosion && !is_tracer)
                target = ray.pos();
        }
        finish_beam();

        return (true);
    }

    return (false);
}

void bolt::affect_cell(bool avoid_self)
{
    // Shooting through clouds affects accuracy.
    if (env.cgrid(pos()) != EMPTY_CLOUD)
        hit = std::max(hit - 2, 0);

    fake_flavour();

    const coord_def old_pos = pos();
    const bool was_solid = feat_is_solid(grd(pos()));

    bool avoid_monster = false;
    bool avoid_player = false;

    if (avoid_self)
    {
        if (YOU_KILL(thrower))
            avoid_player = true;
        else if (MON_KILL(thrower))
            avoid_monster = true;
    }

    if (was_solid)
    {
        // Some special casing.
        if (monsters* mon = monster_at(pos()))
        {
            if (can_affect_wall_monster(mon) && !avoid_monster)
                affect_monster(mon);
            else if (!avoid_monster)
            {
                mprf("The %s protects %s from harm.",
                     raw_feature_description(grd(mon->pos())).c_str(),
                     mon->name(DESC_NOCAP_THE).c_str());
            }
        }

        // Note that this can change the ray position and the solidity
        // of the wall.
        if (hit_wall())
            // Beam ended due to hitting wall, so don't hit the player
            // or monster with the regressed beam.
            return;
    }

    const bool still_wall = (was_solid && old_pos == pos());

    bool hit_player = false;
    // If the player can ever walk through walls, this will need
    // special-casing too.
    if (found_player() && !avoid_player)
    {
        affect_player();
        hit_player = true;
    }

    // We don't want to hit a monster in a wall square twice.  Also,
    // stop single target beams from affecting a monster if they already
    // affected the player on this square. -cao
    if ((!hit_player || is_beam || is_explosion)
         && !still_wall && !avoid_monster)
    {
        if (monsters* m = monster_at(pos()) )
            affect_monster(m);
    }

    if (!feat_is_solid(grd(pos())))
        affect_ground();
}

bool bolt::apply_hit_funcs(actor* victim, int dmg, int corpse)
{
    bool affected = false;
    for (unsigned int i = 0; i < hit_funcs.size(); ++i)
        affected = (*hit_funcs[i])(*this, victim, dmg, corpse) || affected;

    return (affected);
}

bool bolt::apply_dmg_funcs(actor* victim, int &dmg,
                           std::vector<std::string> &messages)
{
    for (unsigned int i = 0; i < damage_funcs.size(); ++i)
    {
        std::string dmg_msg;

        if ( (*damage_funcs[i])(*this, victim, dmg, dmg_msg) )
            return (false);
        if (!dmg_msg.empty())
            messages.push_back(dmg_msg);
    }
    return (true);
}

static void _undo_tracer(bolt &orig, bolt &copy)
{
    // FIXME: we should have a better idea of what gets changed!
    orig.target        = copy.target;
    orig.source        = copy.source;
    orig.aimed_at_spot = copy.aimed_at_spot;
    orig.range_used    = copy.range_used;
    orig.auto_hit      = copy.auto_hit;
    orig.ray           = copy.ray;
    orig.colour        = copy.colour;
    orig.flavour       = copy.flavour;
    orig.real_flavour  = copy.real_flavour;
}

// This saves some important things before calling fire().
void bolt::fire()
{
    path_taken.clear();

    if (special_explosion)
        special_explosion->is_tracer = is_tracer;

    if (is_tracer)
    {
        bolt boltcopy = *this;
        if (special_explosion != NULL)
            boltcopy.special_explosion = new bolt(*special_explosion);

        do_fire();

        if (special_explosion != NULL)
        {
            _undo_tracer(*special_explosion, *boltcopy.special_explosion);
            delete boltcopy.special_explosion;
        }

        _undo_tracer(*this, boltcopy);
    }
    else
        do_fire();

    if (special_explosion != NULL)
    {
        seen           = seen  || special_explosion->seen;
        heard          = heard || special_explosion->heard;
        beam_cancelled = beam_cancelled || special_explosion->beam_cancelled;
        foe_info      += special_explosion->foe_info;
        friend_info   += special_explosion->friend_info;
    }
}

void bolt::do_fire()
{
    initialise_fire();

    if (range <= range_used && range > 0)
    {
#ifdef DEBUG
        mprf(MSGCH_DIAGNOSTICS, "fire_beam() called on already done beam "
             "'%s' (item = '%s')", name.c_str(),
             item ? item->name(DESC_PLAIN).c_str() : "none");
#endif
        return;
    }

    apply_beam_conducts();
    cursor_control coff(false);

#ifdef USE_TILE
    tile_beam = -1;

    if (item && !is_tracer && flavour == BEAM_MISSILE)
    {
        const coord_def diff = target - source;
        tile_beam = tileidx_item_throw(*item, diff.x, diff.y);
    }
#endif

    bool avoid_self = (!aimed_at_feet && (!is_explosion || !in_explosion_phase));

    msg_generated = false;
    if (!aimed_at_feet)
    {
        choose_ray();
    }

#if defined(TARGET_OS_WINDOWS) && !defined(USE_TILE)
    // Before we start drawing the beam, turn buffering off.
    bool oldValue = true;
    if (!is_tracer)
        oldValue = set_buffering(false);
#endif

    while (in_bounds(pos()))
    {
        path_taken.push_back(pos());

        if (!affects_nothing)
            affect_cell(avoid_self);

        if (!avoid_self)
            range_used++;

        if (range_used >= range)
            break;

        if (beam_cancelled)
            return;

        if (pos() == target)
        {
            passed_target = true;
            if (stop_at_target())
                break;
        }

        ASSERT((!feat_is_solid(grd(pos())) || avoid_self)
               || is_tracer && affects_wall(grd(pos())));

        const bool was_seen = seen;
        if (!was_seen && range > 0 && !invisible() && you.see_cell(pos()))
            seen = true;

        if (flavour != BEAM_VISUAL && !was_seen && seen && !is_tracer)
        {
            mprf("%s appears from out of your range of vision.",
                 article_a(name, false).c_str());
        }

        // Reset chaos beams so that it won't be considered an invisible
        // enchantment beam for the purposes of animation.
        if (real_flavour == BEAM_CHAOS)
            flavour = real_flavour;

        // Actually draw the beam/missile/whatever, if the player can see
        // the cell.
        draw(pos());

        ray.advance();

        avoid_self = false;
    }

    if (!in_bounds(pos()))
    {
        ASSERT(!aimed_at_spot);

        int tries = std::max(GXM, GYM);
        while (!in_bounds(ray.pos()) && tries-- > 0)
            ray.regress();

        // Something bizarre happening if we can't get back onto the map.
        ASSERT(in_bounds(pos()));
    }

    // The beam has terminated.
    if (!affects_nothing)
        affect_endpoint();

    // Tracers need nothing further.
    if (is_tracer || affects_nothing)
        return;

    // Canned msg for enchantments that affected no-one, but only if the
    // enchantment is yours (and it wasn't a chaos beam, since with chaos
    // enchantments are entirely random, and if it randomly attempts
    // something which ends up having no obvious effect then the player
    // isn't going to realise it).
    if (!msg_generated && !obvious_effect && is_enchantment()
        && real_flavour != BEAM_CHAOS && YOU_KILL(thrower))
    {
        canned_msg(MSG_NOTHING_HAPPENS);
    }

    // Reactions if a monster zapped the beam.
    if (!invalid_monster_index(beam_source))
    {
        if (foe_info.hurt == 0 && friend_info.hurt > 0)
            xom_is_stimulated(128);
        else if (foe_info.helped > 0 && friend_info.helped == 0)
            xom_is_stimulated(128);

        // Allow friendlies to react to projectiles, except when in
        // sanctuary when pet_target can only be explicitly changed by
        // the player.
        const monsters *mon = &menv[beam_source];
        if (foe_info.hurt > 0 && !mon->wont_attack() && !crawl_state.arena
            && you.pet_target == MHITNOT && env.sanctuary_time <= 0)
        {
            you.pet_target = beam_source;
        }
    }

    // That's it!
#if defined(TARGET_OS_WINDOWS) && !defined(USE_TILE)
    set_buffering(oldValue);
#endif
}

// Returns damage taken by a monster from a "flavoured" (fire, ice, etc.)
// attack -- damage from clouds and branded weapons handled elsewhere.
int mons_adjust_flavoured(monsters *monster, bolt &pbolt, int hurted,
                          bool doFlavouredEffects)
{
    // If we're not doing flavoured effects, must be preliminary
    // damage check only.
    // Do not print messages or apply any side effects!
    int resist = 0;
    int original = hurted;

    switch (pbolt.flavour)
    {
    case BEAM_FIRE:
    case BEAM_STEAM:
        hurted = resist_adjust_damage(
                    monster,
                    pbolt.flavour,
                    (pbolt.flavour == BEAM_FIRE) ? monster->res_fire()
                                                 : monster->res_steam(),
                    hurted, true);

        if (!hurted)
        {
            if (doFlavouredEffects)
            {
                simple_monster_message(monster,
                                       (original > 0) ? " completely resists."
                                                      : " appears unharmed.");
            }
        }
        else if (original > hurted)
        {
            if (doFlavouredEffects)
                simple_monster_message(monster, " resists.");
        }
        else if (original < hurted && doFlavouredEffects)
        {
            if (monster->is_icy())
                simple_monster_message(monster, " melts!");
            else if (monster->type == MONS_BUSH)
                simple_monster_message(monster, " is on fire!");
            else if (pbolt.flavour == BEAM_FIRE)
                simple_monster_message(monster, " is burned terribly!");
            else
                simple_monster_message(monster, " is scalded terribly!");
        }
        break;

    case BEAM_COLD:
        hurted = resist_adjust_damage(monster, pbolt.flavour,
                                      monster->res_cold(),
                                      hurted, true);
        if (!hurted)
        {
            if (doFlavouredEffects)
            {
                simple_monster_message(monster,
                                       (original > 0) ? " completely resists."
                                                      : " appears unharmed.");
            }
        }
        else if (original > hurted)
        {
            if (doFlavouredEffects)
                simple_monster_message(monster, " resists.");
        }
        else if (original < hurted)
        {
            if (doFlavouredEffects)
                simple_monster_message(monster, " is frozen!");
        }
        break;

    case BEAM_ELECTRICITY:
        hurted = resist_adjust_damage(monster, pbolt.flavour,
                                      monster->res_elec(),
                                      hurted, true);
        if (!hurted)
        {
            if (doFlavouredEffects)
            {
                simple_monster_message(monster,
                                       (original > 0) ? " completely resists."
                                                      : " appears unharmed.");
            }
        }
        break;

    case BEAM_ACID:
        hurted = resist_adjust_damage(monster, pbolt.flavour,
                                      monster->res_acid(),
                                      hurted, true);
        if (!hurted)
        {
            if (doFlavouredEffects)
            {
                simple_monster_message(monster,
                                       (original > 0) ? " completely resists."
                                                      : " appears unharmed.");
            }
        }
        break;

    case BEAM_POISON:
    {
        int res = monster->res_poison();
        hurted  = resist_adjust_damage(monster, pbolt.flavour, res,
                                       hurted, true);
        if (!hurted && res > 0)
        {
            if (doFlavouredEffects)
            {
                simple_monster_message(monster,
                                       (original > 0) ? " completely resists."
                                                      : " appears unharmed.");
            }
        }
        else if (res <= 0 && doFlavouredEffects && !one_chance_in(3))
            poison_monster(monster, pbolt.whose_kill());

        break;
    }

    case BEAM_POISON_ARROW:
        hurted = resist_adjust_damage(monster, pbolt.flavour,
                                      monster->res_poison(),
                                      hurted);
        if (hurted < original)
        {
            if (doFlavouredEffects)
            {
                simple_monster_message(monster, " partially resists.");

                // Poison arrow can poison any living thing regardless of
                // poison resistance. - bwr
                if (mons_has_lifeforce(monster))
                    poison_monster(monster, pbolt.whose_kill(), 2, true);
            }
        }
        else if (doFlavouredEffects)
            poison_monster(monster, pbolt.whose_kill(), 4);

        break;

    case BEAM_NEG:
        if (monster->res_negative_energy() == 3)
        {
            if (doFlavouredEffects)
                simple_monster_message(monster, " completely resists.");

            hurted = 0;
        }
        else
        {
            // Early out if no side effects.
            if (!doFlavouredEffects)
                return (hurted);

            if (monster->observable())
                pbolt.obvious_effect = true;

            monster->drain_exp(pbolt.agent());

            if (YOU_KILL(pbolt.thrower))
                did_god_conduct(DID_NECROMANCY, 2, pbolt.effect_known);
        }
        break;

    case BEAM_MIASMA:
        if (monster->res_rotting())
        {
            if (doFlavouredEffects)
                simple_monster_message(monster, " completely resists.");

            hurted = 0;
        }
        else
        {
            // Early out for tracer/no side effects.
            if (!doFlavouredEffects)
                return (hurted);

            miasma_monster(monster, pbolt.whose_kill());

            if (YOU_KILL(pbolt.thrower))
                did_god_conduct(DID_UNCLEAN, 2, pbolt.effect_known);
        }
        break;

    case BEAM_HOLY:
    {
        // Cleansing flame.
        const int rhe = monster->res_holy_energy(pbolt.agent());
        if (rhe > 0)
            hurted = 0;
        else if (rhe == 0)
            hurted /= 2;
        else if (rhe < -1)
            hurted = (hurted * 3) / 2;

        if (doFlavouredEffects)
        {
            simple_monster_message(monster,
                                   hurted == 0 ? " appears unharmed."
                                               : " writhes in agony!");
        }
        break;
    }

    case BEAM_ICE:
        // ice - about 50% of damage is cold, other 50% is impact and
        // can't be resisted (except by AC, of course)
        hurted = resist_adjust_damage(monster, pbolt.flavour,
                                      monster->res_cold(), hurted,
                                      true);
        if (hurted < original)
        {
            if (doFlavouredEffects)
                simple_monster_message(monster, " partially resists.");
        }
        else if (hurted > original)
        {
            if (doFlavouredEffects)
                simple_monster_message(monster, " is frozen!");
        }
        break;

    case BEAM_LAVA:
        hurted = resist_adjust_damage(monster, pbolt.flavour,
                                      monster->res_fire(), hurted, true);

        if (hurted < original)
        {
            if (doFlavouredEffects)
                simple_monster_message(monster, " partially resists.");
        }
        else if (hurted > original)
        {
            if (monster->is_icy())
            {
                if (doFlavouredEffects)
                    simple_monster_message(monster, " melts!");
            }
            else
            {
                if (doFlavouredEffects)
                    simple_monster_message(monster, " is burned terribly!");
            }
        }
        break;

    case BEAM_HELLFIRE:
        resist = monster->res_fire();
        if (resist > 2)
        {
            if (doFlavouredEffects)
            {
                simple_monster_message(monster,
                                       (original > 0) ? " completely resists."
                                                      : " appears unharmed.");
            }

            hurted = 0;
        }
        else if (resist > 0)
        {
            if (doFlavouredEffects)
                simple_monster_message(monster, " partially resists.");

            hurted /= 2;
        }
        else if (resist < 0)
        {
            if (monster->is_icy())
            {
                if (doFlavouredEffects)
                    simple_monster_message(monster, " melts!");
            }
            else
            {
                if (doFlavouredEffects)
                    simple_monster_message(monster, " is burned terribly!");
            }

            hurted *= 12;       // hellfire
            hurted /= 10;
        }
        break;

    case BEAM_SPORE:
        if (monster->type == MONS_BALLISTOMYCETE)
            hurted = 0;
        break;

    default:
        break;
    }

    return (hurted);
}

static bool _monster_resists_mass_enchantment(monsters *monster,
                                              enchant_type wh_enchant,
                                              int pow)
{
    // Assuming that the only mass charm is control undead.
    if (wh_enchant == ENCH_CHARM)
    {
        if (monster->friendly())
            return (true);

        if (monster->holiness() != MH_UNDEAD)
            return (true);

        if (monster->check_res_magic(pow))
        {
            simple_monster_message(monster,
                                   mons_immune_magic(monster)? " is unaffected."
                                                             : " resists.");
            return (true);
        }
    }
    else if (wh_enchant == ENCH_CONFUSION
             || monster->holiness() == MH_NATURAL)
    {
        if (wh_enchant == ENCH_CONFUSION
            && !mons_class_is_confusable(monster->type))
        {
            return (true);
        }

        if (monster->check_res_magic(pow))
        {
            simple_monster_message(monster,
                                   mons_immune_magic(monster)? " is unaffected."
                                                             : " resists.");
            return (true);
        }
    }
    else  // trying to enchant an unnatural creature doesn't work
    {
        simple_monster_message(monster, " is unaffected.");
        return (true);
    }

    return (false);
}

// Enchants all monsters in player's sight.
// If m_succumbed is non-NULL, will be set to the number of monsters that
// were enchanted. If m_attempted is non-NULL, will be set to the number of
// monsters that we tried to enchant.
bool mass_enchantment( enchant_type wh_enchant, int pow, int origin,
                       int *m_succumbed, int *m_attempted )
{
    bool msg_generated = false;

    if (m_succumbed)
        *m_succumbed = 0;
    if (m_attempted)
        *m_attempted = 0;

    pow = std::min(pow, 200);

    const kill_category kc = (origin == MHITYOU ? KC_YOU : KC_OTHER);

    for (monster_iterator mi(&you.get_los()); mi; ++mi)
    {
        if (mi->has_ench(wh_enchant))
            continue;

        if (m_attempted)
            ++*m_attempted;

        if (_monster_resists_mass_enchantment(*mi, wh_enchant, pow))
            continue;

        if (mi->add_ench(mon_enchant(wh_enchant, 0, kc)))
        {
            if (m_succumbed)
                ++*m_succumbed;

            // Do messaging.
            const char* msg;
            switch (wh_enchant)
            {
            case ENCH_FEAR:      msg = " looks frightened!";      break;
            case ENCH_CONFUSION: msg = " looks rather confused."; break;
            case ENCH_CHARM:     msg = " submits to your will.";  break;
            default:             msg = NULL;                      break;
            }
            if (msg)
                msg_generated = simple_monster_message(*mi, msg);

            // Extra check for fear (monster needs to reevaluate behaviour).
            if (wh_enchant == ENCH_FEAR)
                behaviour_event(*mi, ME_SCARE, origin);
        }
    }

    if (!msg_generated)
        canned_msg(MSG_NOTHING_HAPPENS);

    return (msg_generated);
}

void bolt::apply_bolt_paralysis(monsters *monster)
{
    if (!monster->paralysed()
        && monster->add_ench(ENCH_PARALYSIS)
        && (!monster->petrified()
            || monster->has_ench(ENCH_PETRIFYING)))
    {
        if (simple_monster_message(monster, " suddenly stops moving!"))
            obvious_effect = true;

        mons_check_pool(monster, monster->pos(), killer(), beam_source);
    }
}

// Petrification works in two stages. First the monster is slowed down in
// all of its actions and cannot move away (petrifying), and when that times
// out it remains properly petrified (no movement or actions). The second
// part is similar to paralysis, except that insubstantial monsters can't be
// affected and that stabbing damage is drastically reduced.
void bolt::apply_bolt_petrify(monsters *monster)
{
    int petrifying = monster->has_ench(ENCH_PETRIFYING);
    if (monster->petrified())
    {
        // If the petrifying is not yet finished, we can force it to happen
        // right away by casting again. Otherwise, the spell has no further
        // effect.
        if (petrifying > 0)
        {
            monster->del_ench(ENCH_PETRIFYING, true);
            if (!monster->has_ench(ENCH_PARALYSIS)
                && simple_monster_message(monster, " stops moving altogether!"))
            {
                obvious_effect = true;
            }
        }
    }
    else if (monster->add_ench(ENCH_PETRIFIED)
             && !monster->has_ench(ENCH_PARALYSIS))
    {
        // Add both the petrifying and the petrified enchantment. The former
        // will run out sooner and result in plain petrification behaviour.
        monster->add_ench(ENCH_PETRIFYING);
        if (simple_monster_message(monster, " is moving more slowly."))
            obvious_effect = true;

        mons_check_pool(monster, monster->pos(), killer(), beam_source);
    }
}

bool curare_hits_monster(actor *agent, monsters *monster, kill_category who,
                         int levels)
{
    poison_monster(monster, who, levels, false);

    int hurted = 0;

    if (!monster->res_asphyx())
    {
        hurted = roll_dice(2, 6);

        // Note that the hurtage is halved by poison resistance.
        if (monster->res_poison() > 0)
            hurted /= 2;

        if (hurted)
        {
            simple_monster_message(monster, " convulses.");
            monster->hurt(agent, hurted, BEAM_POISON);
        }

        if (monster->alive())
            enchant_monster_with_flavour(monster, agent, BEAM_SLOW);
    }

    // Deities take notice.
    if (who == KC_YOU)
        did_god_conduct(DID_POISON, 5 + random2(3));

    return (hurted > 0);
}

// Actually poisons a monster (with message).
bool poison_monster(monsters *monster, kill_category who, int levels,
                    bool force, bool verbose)
{
    if (!monster->alive())
        return (false);

    if ((!force && monster->res_poison() > 0) || levels <= 0)
        return (false);

    const mon_enchant old_pois = monster->get_ench(ENCH_POISON);
    monster->add_ench(mon_enchant(ENCH_POISON, levels, who));
    const mon_enchant new_pois = monster->get_ench(ENCH_POISON);

    // Actually do the poisoning.  The order is important here.
    if (new_pois.degree > old_pois.degree)
    {
        if (verbose)
        {
            simple_monster_message(monster,
                                   old_pois.degree > 0 ? " looks even sicker."
                                                       : " is poisoned.");
        }
        behaviour_event(monster, ME_ANNOY, (who == KC_YOU) ? MHITYOU : MHITNOT);
    }

    // Finally, take care of deity preferences.
    if (who == KC_YOU)
        did_god_conduct(DID_POISON, 5 + random2(3));

    return (new_pois.degree > old_pois.degree);
}

// Actually poisons, rots, and/or slows a monster with miasma (with
// message).
bool miasma_monster(monsters *monster, kill_category who)
{
    if (!monster->alive())
        return (false);

    if (monster->res_rotting())
        return (false);

    bool success = poison_monster(monster, who);

    if (monster->max_hit_points > 4 && coinflip())
    {
        monster->max_hit_points--;
        monster->hit_points = std::min(monster->max_hit_points,
                                       monster->hit_points);
        success = true;
    }

    if (one_chance_in(3))
    {
        bolt beam;
        beam.flavour = BEAM_SLOW;
        beam.apply_enchantment_to_monster(monster);
        success = true;
    }

    return (success);
}

// Actually napalms a monster (with message).
bool napalm_monster(monsters *monster, kill_category who, int levels,
                    bool verbose)
{
    if (!monster->alive())
        return (false);

    if (monster->res_sticky_flame() || levels <= 0)
        return (false);

    const mon_enchant old_flame = monster->get_ench(ENCH_STICKY_FLAME);
    monster->add_ench(mon_enchant(ENCH_STICKY_FLAME, levels, who));
    const mon_enchant new_flame = monster->get_ench(ENCH_STICKY_FLAME);

    // Actually do the napalming.  The order is important here.
    if (new_flame.degree > old_flame.degree)
    {
        if (verbose)
            simple_monster_message(monster, " is covered in liquid flames!");
        behaviour_event(monster, ME_WHACK, who == KC_YOU ? MHITYOU : MHITNOT);
    }

    return (new_flame.degree > old_flame.degree);
}

//  Used by monsters in "planning" which spell to cast. Fires off a "tracer"
//  which tells the monster what it'll hit if it breathes/casts etc.
//
//  The output from this tracer function is written into the
//  tracer_info variables (friend_info and foe_info.)
//
//  Note that beam properties must be set, as the tracer will take them
//  into account, as well as the monster's intelligence.
void fire_tracer(const monsters *monster, bolt &pbolt, bool explode_only)
{
    // Don't fiddle with any input parameters other than tracer stuff!
    pbolt.is_tracer     = true;
    pbolt.source        = monster->pos();
    pbolt.beam_source   = monster->mindex();
    pbolt.can_see_invis = monster->can_see_invisible();
    pbolt.smart_monster = (mons_intel(monster) >= I_NORMAL);
    pbolt.attitude      = mons_attitude(monster);

    // Init tracer variables.
    pbolt.foe_info.reset();
    pbolt.friend_info.reset();

    // Clear misc
    pbolt.reflections   = 0;
    pbolt.bounces       = 0;

    // If there's a specifically requested foe_ratio, honour it.
    if (!pbolt.foe_ratio)
    {
        pbolt.foe_ratio     = 80;        // default - see mons_should_fire()

        // Foe ratio for summoning greater demons & undead -- they may be
        // summoned, but they're hostile and would love nothing better
        // than to nuke the player and his minions.
        if (mons_att_wont_attack(pbolt.attitude)
            && !mons_att_wont_attack(monster->attitude))
        {
            pbolt.foe_ratio = 25;
        }
    }

    pbolt.in_explosion_phase = false;

    // Fire!
    if (explode_only)
        pbolt.explode(false);
    else
        pbolt.fire();

    // Unset tracer flag (convenience).
    pbolt.is_tracer = false;
}

// When a mimic is hit by a ranged attack, it teleports away (the slow
// way) and changes its appearance - the appearance change is in
// monster_teleport() in mon-stuff.cc.
void mimic_alert(monsters *mimic)
{
    if (!mimic->alive())
        return;

    bool should_id = !testbits(mimic->flags, MF_KNOWN_MIMIC)
                     && mimic->observable();

    // If we got here, we at least got a resists message, if not
    // a full wounds printing. Thus, might as well id the mimic.
    if (mimic->has_ench(ENCH_TP))
    {
        if (should_id)
            mimic->flags |= MF_KNOWN_MIMIC;

        return;
    }

    const bool instant_tele = !one_chance_in(3);
    monster_teleport( mimic, instant_tele );

    // At least for this short while, we know it's a mimic.
    if (!instant_tele && should_id)
        mimic->flags |= MF_KNOWN_MIMIC;
}

bool bolt::is_bouncy(dungeon_feature_type feat) const
{
    if (real_flavour == BEAM_CHAOS && feat_is_solid(feat))
        return (true);

    if (is_enchantment())
        return (false);

    if (flavour == BEAM_ELECTRICITY && feat != DNGN_METAL_WALL)
        return (true);

    if ((flavour == BEAM_FIRE || flavour == BEAM_COLD)
        && feat == DNGN_GREEN_CRYSTAL_WALL )
    {
        return (true);
    }

    return (false);
}

static int _potion_beam_flavour_to_colour(beam_type flavour)
{
    switch (flavour)
    {
    case BEAM_POTION_STINKING_CLOUD:
        return (GREEN);

    case BEAM_POTION_POISON:
        return (coinflip() ? GREEN : LIGHTGREEN);

    case BEAM_POTION_MIASMA:
    case BEAM_POTION_BLACK_SMOKE:
        return (DARKGREY);

    case BEAM_POTION_STEAM:
    case BEAM_POTION_GREY_SMOKE:
        return (LIGHTGREY);

    case BEAM_POTION_FIRE:
        return (coinflip() ? RED : LIGHTRED);

    case BEAM_POTION_COLD:
        return (coinflip() ? BLUE : LIGHTBLUE);

    case BEAM_POTION_BLUE_SMOKE:
        return (LIGHTBLUE);

    case BEAM_POTION_PURPLE_SMOKE:
        return (MAGENTA);

    case BEAM_POTION_RANDOM:
    default:
        // Leave it the colour of the potion, the clouds will colour
        // themselves on the next refresh. -- bwr
        return (-1);
    }
    return (-1);
}

void bolt::affect_endpoint()
{
    if (special_explosion)
    {
        special_explosion->refine_for_explosion();
        special_explosion->target = pos();
        special_explosion->explode();
    }

    // Leave an object, if applicable.
    if (drop_item && item)
        drop_object();

    if (is_explosion)
    {
        refine_for_explosion();
        target = pos();
        explode();
        return;
    }

    if (is_tracer)
        return;

    // FIXME: why don't these just have is_explosion set?
    // They don't explode in tracers: why not?
    if  (name == "orb of electricity"
        || name == "metal orb"
        || name == "great blast of cold")
    {
        target = pos();
        refine_for_explosion();
        explode();
    }

    if (name == "blast of poison")
        big_cloud(CLOUD_POISON, whose_kill(), killer(), pos(), 0, 7+random2(5));

    if (name == "foul vapour")
    {
        // death drake; swamp drakes handled earlier
        ASSERT(flavour == BEAM_MIASMA);
        big_cloud(CLOUD_MIASMA, whose_kill(), killer(), pos(), 0, 9);
    }

    if (name == "freezing blast")
    {
        big_cloud(CLOUD_COLD, whose_kill(), killer(), pos(),
                  random_range(10, 15), 9);
    }
}

bool bolt::stop_at_target() const
{
    return (is_explosion || is_big_cloud || aimed_at_spot);
}

void bolt::drop_object()
{
    ASSERT( item != NULL && item->is_valid() );

    // Conditions: beam is missile and not tracer.
    if (is_tracer || !was_missile)
        return;

    // Summoned creatures' thrown items disappear.
    if (item->flags & ISFLAG_SUMMONED)
    {
        if (you.see_cell(pos()))
        {
            mprf("%s %s!",
                 item->name(DESC_CAP_THE).c_str(),
                 summoned_poof_msg(beam_source, *item).c_str());
        }
        item_was_destroyed(*item, beam_source);
        return;
    }

    if (!thrown_object_destroyed(item, pos(), false))
    {
        if (item->sub_type == MI_THROWING_NET)
        {
            monsters* m = monster_at(pos());
            // Player or monster at position is caught in net.
            if (you.pos() == pos() && you.attribute[ATTR_HELD]
                || m && m->caught())
            {
                // If no trapping net found mark this one.
                if (get_trapping_net(pos(), true) == NON_ITEM)
                    set_item_stationary(*item);
            }
        }
        copy_item_to_grid(*item, pos(), 1);
    }
    else if (item->sub_type == MI_LARGE_ROCK
             && !feat_destroys_items(grd(pos())))
    {
        // Large rocks mulch to stone.
        std::string sound_msg = "You hear a cracking sound!";
        if (you.see_cell(pos()))
        {
            mprf("%s shatters into pieces!",
                 item->name(DESC_CAP_THE).c_str());
            sound_msg = "";
        }
        noisy(12, pos(), sound_msg.c_str());

        item->sub_type = MI_STONE;
        item->quantity = 10 + random2(41);
        // Remove thrown flag: we might not want to pick up the stones.
        item->flags &= ~ISFLAG_THROWN;

        copy_item_to_grid(*item, pos(), item->quantity);
    }
}

// Returns true if the beam hits the player, fuzzing the beam if necessary
// for monsters without see invis firing tracers at the player.
bool bolt::found_player() const
{
    const bool needs_fuzz = (is_tracer && !can_see_invis
                             && you.invisible() && !YOU_KILL(thrower));
    const int dist = needs_fuzz? 2 : 0;

    return (grid_distance(pos(), you.pos()) <= dist);
}

void bolt::affect_ground()
{
    // Explosions only have an effect during their explosion phase.
    // Special cases can be handled here.
    if (is_explosion && !in_explosion_phase)
        return;

    if (is_tracer)
        return;

    // Spore explosions might spawn a fungus.  The spore explosion
    // covers 21 tiles in open space, so the expected number of spores
    // produced is the x in x_chance_in_y() in the conditional below.
    if (is_explosion && flavour == BEAM_SPORE
        && x_chance_in_y(2, 21)
        && mons_class_can_pass(MONS_BALLISTOMYCETE, env.grid(pos()))
        && !actor_at(pos()))
    {
        beh_type beh;
        // Half the fungi in arena mode are friendly.
        if (crawl_state.arena)
        {
            beh = coinflip() ? BEH_FRIENDLY : BEH_HOSTILE;
        }
        else
        {
            switch (this->attitude)
            {
            case ATT_NEUTRAL:
                beh = BEH_NEUTRAL;
                break;

            case ATT_FRIENDLY:
            case ATT_GOOD_NEUTRAL:
                beh = BEH_GOOD_NEUTRAL;
                break;

            default:
                beh = BEH_HOSTILE;
                break;
            }
        }

        int rc = create_monster(mgen_data(MONS_BALLISTOMYCETE,
                                          beh,
                                          agent(),
                                          0,
                                          0,
                                          pos(),
                                          MHITNOT,
                                          MG_FORCE_PLACE));

        if (rc != -1 && you.see_cell(pos()))
            mpr("A fungus suddenly grows.");
    }

    if (affects_items)
    {
        const int burn_power = is_explosion ? 5 :
                               is_beam      ? 3
                                            : 2;
        expose_items_to_element(flavour, pos(), burn_power);
        affect_place_clouds();
    }
}

bool bolt::is_fiery() const
{
    return (flavour == BEAM_FIRE
            || flavour == BEAM_HELLFIRE
            || flavour == BEAM_LAVA);
}

bool bolt::is_superhot() const
{
    if (!is_fiery())
        return (false);

    return (name == "bolt of fire"
            || name == "bolt of magma"
            || name == "fireball"
            || name.find("hellfire") != std::string::npos
               && in_explosion_phase);
}

bool bolt::affects_wall(dungeon_feature_type wall) const
{
    // digging
    if (flavour == BEAM_DIGGING)
        return (true);

    if (flavour == BEAM_DISINTEGRATION && damage.num >= 3)
        return (true);

    if (is_fiery() && (wall == DNGN_WAX_WALL || wall == DNGN_TREES))
        return (true);

    // eye of devastation?
    if (flavour == BEAM_NUKE)
        return (true);

    // Lee's Rapid Deconstruction
    if (flavour == BEAM_FRAG)
        return (true);

    return (false);
}

void bolt::affect_place_clouds()
{
    if (in_explosion_phase)
        affect_place_explosion_clouds();

    const coord_def p = pos();

    // Is there already a cloud here?
    const int cloudidx = env.cgrid(p);
    if (cloudidx != EMPTY_CLOUD)
    {
        cloud_type& ctype = env.cloud[cloudidx].type;
        // Polymorph randomly changes clouds in its path
        if (flavour == BEAM_POLYMORPH)
        {
            cloud_type new_type = static_cast<cloud_type>(1 + random2(8));

            if (new_type == ctype)
                return;

            if (p == you.pos())
            {
                mprf("The %s you are in turns into %s!",
                     cloud_name(ctype).c_str(), cloud_name(new_type).c_str());
                obvious_effect = true;
            }
            else if (you.see_cell(p))
            {
                mprf("A cloud of %s turns into %s.",
                     cloud_name(ctype).c_str(), cloud_name(new_type).c_str());
                obvious_effect = true;
            }

            ctype = new_type;
            return;
        }

        // fire cancelling cold & vice versa
        if ((ctype == CLOUD_COLD
             && (flavour == BEAM_FIRE || flavour == BEAM_LAVA))
            || (ctype == CLOUD_FIRE && flavour == BEAM_COLD))
        {
            if (player_can_hear(p))
                mpr("You hear a sizzling sound!", MSGCH_SOUND);

            delete_cloud(cloudidx);
            range_used += 5;
        }
        return;
    }

    // No clouds here, free to make new ones.
    const dungeon_feature_type feat = grd(p);

    if (name == "blast of poison")
        place_cloud(CLOUD_POISON, p, random2(4) + 2, whose_kill(), killer());

    // Fire/cold over water/lava
    if (feat == DNGN_LAVA && flavour == BEAM_COLD
        || feat_is_watery(feat) && is_fiery())
    {
        place_cloud(CLOUD_STEAM, p, 2 + random2(5), whose_kill(), killer());
    }

    if (feat_is_watery(feat) && flavour == BEAM_COLD
        && damage.num * damage.size > 35)
    {
        place_cloud(CLOUD_COLD, p, damage.num * damage.size / 30 + 1,
                    whose_kill(), killer());
    }

    if (name == "great blast of cold")
        place_cloud(CLOUD_COLD, p, random2(5) + 3, whose_kill(), killer());

    if (name == "ball of steam")
        place_cloud(CLOUD_STEAM, p, random2(5) + 2, whose_kill(), killer());

    if (flavour == BEAM_MIASMA)
        place_cloud(CLOUD_MIASMA, p, random2(5) + 2, whose_kill(), killer());

    if (name == "poison gas")
        place_cloud(CLOUD_POISON, p, random2(4) + 3, whose_kill(), killer());

}

void bolt::affect_place_explosion_clouds()
{
    const coord_def p = pos();

    // First check: fire/cold over water/lava.
    if (grd(p) == DNGN_LAVA && flavour == BEAM_COLD
        || feat_is_watery(grd(p)) && is_fiery())
    {
        place_cloud(CLOUD_STEAM, p, 2 + random2(5), whose_kill(), killer());
        return;
    }

    if (flavour >= BEAM_POTION_STINKING_CLOUD && flavour <= BEAM_POTION_RANDOM)
    {
        const int duration = roll_dice(2, 3 + ench_power / 20);
        cloud_type cl_type;

        switch (flavour)
        {
        case BEAM_POTION_STINKING_CLOUD:
        case BEAM_POTION_POISON:
        case BEAM_POTION_MIASMA:
        case BEAM_POTION_STEAM:
        case BEAM_POTION_FIRE:
        case BEAM_POTION_COLD:
        case BEAM_POTION_BLACK_SMOKE:
        case BEAM_POTION_GREY_SMOKE:
        case BEAM_POTION_BLUE_SMOKE:
        case BEAM_POTION_PURPLE_SMOKE:
        case BEAM_POTION_RAIN:
        case BEAM_POTION_MUTAGENIC:
            cl_type = beam2cloud(flavour);
            break;

        case BEAM_POTION_RANDOM:
            switch (random2(10))
            {
            case 0:  cl_type = CLOUD_FIRE;           break;
            case 1:  cl_type = CLOUD_STINK;          break;
            case 2:  cl_type = CLOUD_COLD;           break;
            case 3:  cl_type = CLOUD_POISON;         break;
            case 4:  cl_type = CLOUD_BLACK_SMOKE;    break;
            case 5:  cl_type = CLOUD_GREY_SMOKE;     break;
            case 6:  cl_type = CLOUD_BLUE_SMOKE;     break;
            case 7:  cl_type = CLOUD_PURPLE_SMOKE;   break;
            default: cl_type = CLOUD_STEAM;          break;
            }
            break;

        default:
            cl_type = CLOUD_STEAM;
            break;
        }

        place_cloud(cl_type, p, duration, whose_kill(), killer());
    }

    // then check for more specific explosion cloud types.
    if (name == "ice storm")
        place_cloud(CLOUD_COLD, p, 2 + random2avg(5,2), whose_kill(), killer());

    if (name == "stinking cloud")
    {
        const int duration =  1 + random2(4) + random2((ench_power / 50) + 1);
        place_cloud( CLOUD_STINK, p, duration, whose_kill(), killer() );
    }

    if (name == "great blast of fire")
    {
        int duration = 1 + random2(5) + roll_dice(2, ench_power / 5);

        if (duration > 20)
            duration = 20 + random2(4);

        place_cloud( CLOUD_FIRE, p, duration, whose_kill(), killer() );

        if (grd(p) == DNGN_FLOOR && !monster_at(p) && one_chance_in(4))
        {
            const god_type god =
                (crawl_state.is_god_acting()) ? crawl_state.which_god_acting()
                                              : GOD_NO_GOD;
            const beh_type att =
                (whose_kill() == KC_OTHER ? BEH_HOSTILE : BEH_FRIENDLY);

            actor* summ = agent();
            mgen_data mg(MONS_FIRE_VORTEX, att, summ, 2, SPELL_FIRE_STORM,
                         p, MHITNOT, 0, god);

            // Spell-summoned monsters need to have a live summoner.
            if (summ == NULL || !summ->alive())
            {
                if (!source_name.empty())
                    mg.non_actor_summoner = source_name;
                else if (god != GOD_NO_GOD)
                    mg.non_actor_summoner = god_name(god);
            }

            mons_place(mg);
        }
    }
}

// A little helper function to handle the calling of ouch()...
void bolt::internal_ouch(int dam)
{
    monsters *monst = NULL;
    if (!invalid_monster_index(beam_source) && menv[beam_source].type != -1)
        monst = &menv[beam_source];

    // The order of this is important.
    if (monst && (monst->type == MONS_GIANT_SPORE
                  || monst->type == MONS_BALL_LIGHTNING))
    {
        ouch(dam, beam_source, KILLED_BY_SPORE, aux_source.c_str());
    }
    else if (YOU_KILL(thrower) && aux_source.empty())
    {
        if (reflections > 0)
            ouch(dam, reflector, KILLED_BY_REFLECTION, name.c_str());
        else if (bounces > 0)
            ouch(dam, NON_MONSTER, KILLED_BY_BOUNCE, name.c_str());
        else
        {
            if (aimed_at_feet && effect_known)
                ouch(dam, NON_MONSTER, KILLED_BY_SELF_AIMED, name.c_str());
            else
                ouch(dam, NON_MONSTER, KILLED_BY_TARGETTING);
        }
    }
    else if (flavour == BEAM_DISINTEGRATION || flavour == BEAM_NUKE)
        ouch(dam, beam_source, KILLED_BY_DISINT, aux_source.c_str());
    else if (MON_KILL(thrower))
        ouch(dam, beam_source, KILLED_BY_BEAM, aux_source.c_str());
    else // KILL_MISC || (YOU_KILL && aux_source)
        ouch(dam, beam_source, KILLED_BY_WILD_MAGIC, aux_source.c_str());
}

// [ds] Apply a fuzz if the monster lacks see invisible and is trying to target
// an invisible player. This makes invisibility slightly more powerful.
bool bolt::fuzz_invis_tracer()
{
    // Did the monster have a rough idea of where you are?
    int dist = grid_distance(target, you.pos());

    // No, ditch this.
    if (dist > 2)
        return (false);

    const int beam_src = beam_source_as_target();
    if (beam_src != MHITNOT && beam_src != MHITYOU)
    {
        // Monsters that can sense invisible
        const monsters *mon = &menv[beam_src];
        if (mons_sense_invis(mon))
            return (dist == 0);
    }

    // Apply fuzz now.
    coord_def fuzz( random_range(-2, 2), random_range(-2, 2) );
    coord_def newtarget = target + fuzz;

    if (in_bounds(newtarget))
        target = newtarget;

    // Fire away!
    return (true);
}

// A first step towards to-hit sanity for beams. We're still being
// very kind to the player, but it should be fairer to monsters than
// 4.0.
static bool _test_beam_hit(int attack, int defence, bool is_beam,
                           bool deflect, bool repel, defer_rand &r)
{
    if (is_beam && deflect)
    {
        attack = r[0].random2(attack * 2) / 3;
    }
    else if (is_beam && repel)
    {
        if (attack >= 2)
            attack = r[0].random_range((attack + 1) / 2 + 1, attack);
    }
    else if (deflect)
    {
        attack = r[0].random2(attack / 2);
    }
    else if (repel)
    {
        attack = r[0].random2(attack);
    }

#ifdef DEBUG_DIAGNOSTICS
    mprf(MSGCH_DIAGNOSTICS, "Beam attack: %d, defence: %d", attack, defence);
#endif
    // Reproducing old behavior here; magic dart is dodgable with DMsl
    if (attack == AUTOMATIC_HIT)
        return (true);

    attack = r[1].random2(attack);
    defence = r[2].random2avg(defence, 2);

#ifdef DEBUG_DIAGNOSTICS
    mprf(MSGCH_DIAGNOSTICS, "Beam new attack: %d, defence: %d", attack, defence);
#endif

    return (attack >= defence);
}

std::string bolt::zapper() const
{
    const int beam_src = beam_source_as_target();
    if (beam_src == MHITYOU)
        return ("self");
    else if (beam_src == MHITNOT)
        return ("");
    else
        return menv[beam_src].name(DESC_PLAIN);
}

bool bolt::is_harmless(const monsters *mon) const
{
    // For enchantments, this is already handled in nasty_to().
    if (is_enchantment())
        return (!nasty_to(mon));

    // The others are handled here.
    switch (flavour)
    {
    case BEAM_VISUAL:
    case BEAM_DIGGING:
        return (true);

    case BEAM_HOLY:
        return (mon->res_holy_energy(agent()) > 0);

    case BEAM_STEAM:
        return (mon->res_steam() >= 3);

    case BEAM_FIRE:
        return (mon->res_fire() >= 3);

    case BEAM_COLD:
        return (mon->res_cold() >= 3);

    case BEAM_MIASMA:
        return (mon->res_rotting());

    case BEAM_NEG:
        return (mon->res_negative_energy() == 3);

    case BEAM_ELECTRICITY:
        return (mon->res_elec() >= 3);

    case BEAM_POISON:
        return (mon->res_poison() >= 3);

    case BEAM_ACID:
        return (mon->res_acid() >= 3);

    default:
        return (false);
    }
}

bool bolt::harmless_to_player() const
{
#ifdef DEBUG_DIAGNOSTICS
    mprf(MSGCH_DIAGNOSTICS, "beam flavour: %d", flavour);
#endif

    switch (flavour)
    {
    case BEAM_VISUAL:
    case BEAM_DIGGING:
        return (true);

    // Positive enchantments.
    case BEAM_HASTE:
    case BEAM_HEALING:
    case BEAM_INVISIBILITY:
        return (true);

    case BEAM_HOLY:
        return (is_good_god(you.religion));

    case BEAM_STEAM:
        return (player_res_steam(false) >= 3);

    case BEAM_MIASMA:
        return (you.res_rotting());

    case BEAM_NEG:
        return (player_prot_life(false) >= 3);

    case BEAM_POISON:
        return (player_res_poison(false));

    case BEAM_POTION_STINKING_CLOUD:
        return (player_res_poison(false) || player_mental_clarity(false));

    case BEAM_ELECTRICITY:
        return (player_res_electricity(false));

    case BEAM_FIRE:
    case BEAM_COLD:
    case BEAM_ACID:
        // Fire and ice can destroy inventory items, acid damage equipment.
        return (false);

    default:
        return (false);
    }
}

bool bolt::is_reflectable(const item_def *it) const
{
    if (range_used >= range)
        return (false);

    return (it && is_shield(*it) && shield_reflects(*it));
}

static void _ident_reflector(item_def *item)
{
    if (!is_artefact(*item))
        set_ident_flags(*item, ISFLAG_KNOW_TYPE);
}

// Reflect a beam back the direction it came. This is used
// by shields of reflection.
void bolt::reflect()
{
    reflections++;

    // If it bounced off a wall before being reflected then head back towards
    // the wall.
    if (bounces > 0 && in_bounds(bounce_pos))
        target = bounce_pos;
    else
        target = source;

    source = pos();

    // Reset bounce_pos, so that if we somehow reflect again before reaching
    // the wall that we won't keep heading towards the wall.
    bounce_pos.reset();

    if (pos() == you.pos())
        reflector = NON_MONSTER;
    else if (monsters* m = monster_at(pos()))
        reflector = m->mindex();
    else
    {
        reflector = -1;
#ifdef DEBUG
        mprf(MSGCH_DIAGNOSTICS, "Bolt reflected by neither player nor "
             "monster (bolt = %s, item = %s)", name.c_str(),
             item ? item->name(DESC_PLAIN).c_str() : "none");
#endif
    }

    flavour = real_flavour;
    choose_ray();
}

void bolt::tracer_affect_player()
{
    // Check whether thrower can see player, unless thrower == player.
    if (YOU_KILL(thrower))
    {
        // Don't ask if we're aiming at ourselves.
        if (!aimed_at_feet && !dont_stop_player && !harmless_to_player())
        {
            if (yesno("That beam is likely to hit you. Continue anyway?",
                      false, 'n'))
            {
                friend_info.count++;
                friend_info.power += you.experience_level;
                dont_stop_player = true;
            }
            else
            {
                beam_cancelled = true;
                finish_beam();
            }
        }
    }
    else if (can_see_invis || !you.invisible() || fuzz_invis_tracer())
    {
        if (mons_att_wont_attack(attitude))
        {
            friend_info.count++;
            friend_info.power += you.experience_level;
        }
        else
        {
            foe_info.count++;
            foe_info.power += you.experience_level;
        }
    }

    std::vector<std::string> messages;
    int dummy = 0;

    apply_dmg_funcs(&you, dummy, messages);

    for (unsigned int i = 0; i < messages.size(); ++i)
        mpr(messages[i].c_str(), MSGCH_WARN);

    apply_hit_funcs(&you, 0);
    range_used += range_used_on_hit(&you);
}

bool bolt::misses_player()
{
    if (is_explosion || aimed_at_feet || auto_hit || is_enchantment())
        return (false);

    const int dodge = player_evasion();
    const int dodge_less = player_evasion(EV_IGNORE_PHASESHIFT);
    int real_tohit  = hit;

    // Monsters shooting at an invisible player are very inaccurate.
    if (you.invisible() && !can_see_invis)
        real_tohit /= 2;

    if (you.backlit())
        real_tohit += 2 + random2(8);

    // Wow, what a horrid test.  These cannot be blocked or dodged
    if (!is_beam && !is_blockable())
        return (false);

    bool train_shields_more = false;

    if (is_blockable()
        && you.shield()
        && !aimed_at_feet
        && player_shield_class() > 0)
    {
        // We use the original to-hit here.
        const int testhit = random2(hit * 130 / 100
                                    + you.shield_block_penalty());

        const int block = you.shield_bonus();

#ifdef DEBUG_DIAGNOSTICS
        mprf(MSGCH_DIAGNOSTICS, "Beamshield: hit: %d, block %d",
             testhit, block);
#endif
        if (testhit < block)
        {
            if (is_reflectable(you.shield()))
            {
                mprf( "Your %s reflects the %s!",
                      you.shield()->name(DESC_PLAIN).c_str(),
                      name.c_str() );
                _ident_reflector(you.shield());
                reflect();
            }
            else
            {
                mprf( "You block the %s.", name.c_str() );
                finish_beam();
            }
            you.shield_block_succeeded(agent());
            return (true);
        }

        // Some training just for the "attempt".
        train_shields_more = true;
    }

    if (player_light_armour(true) && !aimed_at_feet && coinflip())
        exercise(SK_DODGING, 1);

    defer_rand r;
    bool miss = true;

    bool dmsl = you.duration[DUR_DEFLECT_MISSILES];
    bool rmsl = dmsl || you.duration[DUR_REPEL_MISSILES]
                || player_mutation_level(MUT_REPULSION_FIELD) == 3;

    if (!_test_beam_hit(real_tohit, dodge_less, is_beam, false, false, r))
    {
        mprf("The %s misses you.", name.c_str());
    }
    else if (!_test_beam_hit(real_tohit, dodge_less, is_beam, false, rmsl, r))
    {
        mprf("The %s is repelled.", name.c_str());
    }
    else if (!_test_beam_hit(real_tohit, dodge_less, is_beam, dmsl, rmsl, r))
    {
        // active voice to imply stronger effect
        mprf("You deflect the %s!", name.c_str());
    }
    else if (!_test_beam_hit(real_tohit, dodge, is_beam, dmsl, rmsl, r))
    {
        mprf("You momentarily phase out as the %s "
             "passes through you.", name.c_str());
    }
    else
    {
        const bool engulfs = is_explosion || is_big_cloud;
        int dodge_more = player_evasion(EV_IGNORE_HELPLESS);

        if (hit_verb.empty())
            hit_verb = engulfs ? "engulfs" : "hits";

        if (_test_beam_hit(real_tohit, dodge_more, is_beam, dmsl, rmsl, r))
        {
            mprf("The %s %s you!", name.c_str(), hit_verb.c_str());
        }
        else
        {
            mprf("Helpless, you fail to dodge the %s.", name.c_str());
        }

        miss = false;
    }

    if (coinflip() && train_shields_more)
            exercise(SK_SHIELDS, one_chance_in(3) ? 1 : 0);

    return (miss);
}

void bolt::affect_player_enchantment()
{
    if (flavour != BEAM_POLYMORPH && has_saving_throw()
        && you.check_res_magic(ench_power))
    {
        // You resisted it.

        // Give a message.
        bool need_msg = true;
        if (thrower != KILL_YOU_MISSILE && !invalid_monster_index(beam_source))
        {
            monsters *mon = &menv[beam_source];
            if (!mon->observable())
            {
                mpr("Something tries to affect you, but you resist.");
                need_msg = false;
            }
        }
        if (need_msg)
            canned_msg(MSG_YOU_RESIST);

        // You *could* have gotten a free teleportation in the Abyss,
        // but no, you resisted.
        if (flavour == BEAM_TELEPORT && you.level_type == LEVEL_ABYSS)
            xom_is_stimulated(255);

        range_used += range_used_on_hit(&you);
        return;
    }

    // You didn't resist it.
    if (effect_known)
        _ench_animation(real_flavour);
    else
        _zap_animation(-1);

    bool nasty = true, nice = false;

    switch (flavour)
    {
    case BEAM_HIBERNATION:
        you.hibernate(ench_power);
        break;

    case BEAM_SLEEP:
        you.put_to_sleep(ench_power);
        break;

    case BEAM_CORONA:
        you.backlight();
        obvious_effect = true;
        break;

    case BEAM_POLYMORPH:
        if (MON_KILL(thrower))
        {
            mpr("Strange energies course through your body.");
            you.mutate();
            obvious_effect = true;
        }
        else if (get_ident_type(OBJ_WANDS, WAND_POLYMORPH_OTHER)
                 == ID_KNOWN_TYPE)
        {
            mpr("This is polymorph other only!");
        }
        else
            canned_msg(MSG_NOTHING_HAPPENS);
        break;

    case BEAM_SLOW:
        potion_effect( POT_SLOWING, ench_power );
        obvious_effect = true;
        break;

    case BEAM_HASTE:
        potion_effect( POT_SPEED, ench_power, false, thrower == KILL_YOU_MISSILE );
        contaminate_player( 1, effect_known );
        obvious_effect = true;
        nasty = false;
        nice  = true;
        break;

    case BEAM_HEALING:
        potion_effect( POT_HEAL_WOUNDS, ench_power );
        obvious_effect = true;
        nasty = false;
        nice  = true;
        break;

    case BEAM_PARALYSIS:
        potion_effect( POT_PARALYSIS, ench_power );
        obvious_effect = true;
        break;

    case BEAM_PETRIFY:
        you.petrify( agent(), ench_power );
        obvious_effect = true;
        break;

    case BEAM_CONFUSION:
        potion_effect( POT_CONFUSION, ench_power );
        obvious_effect = true;
        break;

    case BEAM_INVISIBILITY:
        potion_effect( POT_INVISIBILITY, ench_power );
        contaminate_player( 1 + random2(2), effect_known );
        obvious_effect = true;
        nasty = false;
        nice  = true;
        break;

    case BEAM_TELEPORT:
        you_teleport();

        // An enemy helping you escape while in the Abyss, or an
        // enemy stabilizing a teleport that was about to happen.
        if (!mons_att_wont_attack(attitude)
            && you.level_type == LEVEL_ABYSS)
        {
            xom_is_stimulated(255);
        }

        obvious_effect = true;
        break;

    case BEAM_BLINK:
        random_blink(false);
        obvious_effect = true;
        break;

    case BEAM_BLINK_CLOSE:
        blink_other_close(&you, source);
        obvious_effect = true;
        break;

    case BEAM_CHARM:
        potion_effect( POT_CONFUSION, ench_power );
        obvious_effect = true;
        break;     // enslavement - confusion?

    case BEAM_BANISH:
        if (YOU_KILL(thrower))
        {
            mpr("This spell isn't strong enough to banish yourself.");
            break;
        }
        if (you.level_type == LEVEL_ABYSS)
        {
            mpr("You feel trapped.");
            break;
        }
        you.banished        = true;
        you.banished_by     = zapper();
        obvious_effect = true;
        break;

    case BEAM_PAIN:
        if (player_res_torment())
        {
            mpr("You are unaffected.");
            break;
        }

        if (aux_source.empty())
            aux_source = "by nerve-wracking pain";

        if (name.find("agony") != std::string::npos)
        {
            if (you.res_negative_energy()) // Agony has no effect with rN.
            {
                mpr("You are unaffected.");
                break;
            }

            mpr("Your body is wracked with pain!");

            // On the player, Agony acts like single-target torment.
            internal_ouch(std::max(0, you.hp / 2 - 1));
        }
        else
        {
            mpr("Pain shoots through your body!");

            internal_ouch(damage.roll());
        }
        obvious_effect = true;
        break;

    case BEAM_DISPEL_UNDEAD:
        if (!you.is_undead)
        {
            mpr("You are unaffected.");
            break;
        }

        mpr("You convulse!");

        if (aux_source.empty())
            aux_source = "by dispel undead";

        if (you.is_undead == US_SEMI_UNDEAD)
        {
            if (you.hunger_state == HS_ENGORGED)
                damage.size /= 2;
            else if (you.hunger_state > HS_SATIATED)
            {
                damage.size *= 2;
                damage.size /= 3;
            }
        }
        internal_ouch(damage.roll());
        obvious_effect = true;
        break;

    case BEAM_DISINTEGRATION:
        mpr("You are blasted!");

        if (aux_source.empty())
            aux_source = "a disintegration bolt";

        {
            int amt = damage.roll();
            internal_ouch(amt);

            if (you.can_bleed())
                blood_spray(you.pos(), MONS_PLAYER, amt / 5);
        }

        obvious_effect = true;
        break;

    case BEAM_PORKALATOR:
        if (!transform(ench_power, TRAN_PIG, true))
        {
            mpr("You feel like a pig.");
            break;
        }
        obvious_effect = true;
        break;

    default:
        // _All_ enchantments should be enumerated here!
        mpr("Software bugs nibble your toes!");
        break;
    }

    if (nasty)
    {
        if (mons_att_wont_attack(attitude))
        {
            friend_info.hurt++;
            if (beam_source == NON_MONSTER)
            {
                // Beam from player rebounded and hit player.
                if (!aimed_at_feet)
                    xom_is_stimulated(255);
            }
            else
            {
                // Beam from an ally or neutral.
                xom_is_stimulated(128);
            }
        }
        else
            foe_info.hurt++;
    }

    if (nice)
    {
        if (mons_att_wont_attack(attitude))
            friend_info.helped++;
        else
        {
            foe_info.helped++;
            xom_is_stimulated(128);
        }
    }

    apply_hit_funcs(&you, 0);

    // Regardless of effect, we need to know if this is a stopper
    // or not - it seems all of the above are.
    range_used += range_used_on_hit(&you);
}


void bolt::affect_player()
{
    // Explosions only have an effect during their explosion phase.
    // Special cases can be handled here.
    if (is_explosion && !in_explosion_phase)
    {
        // Trigger the explosion.
        finish_beam();
        return;
    }

    // Digging -- don't care.
    if (flavour == BEAM_DIGGING)
        return;

    if (is_tracer)
    {
        tracer_affect_player();
        return;
    }

    // Trigger an interrupt, so travel will stop on misses which
    // generate smoke.
    if (!YOU_KILL(thrower))
        interrupt_activity(AI_MONSTER_ATTACKS);

    if (is_enchantment())
    {
        affect_player_enchantment();
        return;
    }

    msg_generated = true;

    if (misses_player())
        return;

    const bool engulfs = is_explosion || is_big_cloud;

    // FIXME: Lots of duplicated code here (compare handling of
    // monsters)
    int hurted = 0;
    int burn_power = (is_explosion) ? 5 : (is_beam) ? 3 : 2;

    // Roll the damage.
    hurted += damage.roll();

#if DEBUG_DIAGNOSTICS
    int roll = hurted;
#endif

    std::vector<std::string> messages;
    apply_dmg_funcs(&you, hurted, messages);

    int armour_damage_reduction = random2( 1 + you.armour_class() );
    if (flavour == BEAM_ELECTRICITY)
        armour_damage_reduction /= 2;
    hurted -= armour_damage_reduction;

    // shrapnel has triple AC reduction
    if (flavour == BEAM_FRAG && !player_light_armour())
    {
        hurted -= random2( 1 + you.armour_class() );
        hurted -= random2( 1 + you.armour_class() );
    }

#if DEBUG_DIAGNOSTICS
    mprf(MSGCH_DIAGNOSTICS,
         "Player damage: rolled=%d; after AC=%d", roll, hurted );
#endif

    if (you.equip[EQ_BODY_ARMOUR] != -1)
    {
        if (!player_light_armour(false) && one_chance_in(4)
            && x_chance_in_y(item_mass(you.inv[you.equip[EQ_BODY_ARMOUR]]) + 1,
                             1000))
        {
            exercise( SK_ARMOUR, 1 );
        }
    }

    bool was_affected = false;
    int  old_hp       = you.hp;

    hurted = std::max(0, hurted);

    // If the beam is an actual missile or of the MMISSILE type (Earth magic)
    // we might bleed on the floor.
    if (!engulfs
        && (flavour == BEAM_MISSILE || flavour == BEAM_MMISSILE))
    {
        // assumes DVORP_PIERCING, factor: 0.5
        int blood = std::min(you.hp, hurted / 2);
        bleed_onto_floor(you.pos(), MONS_PLAYER, blood, true);
    }

    hurted = check_your_resists(hurted, flavour);

    if (flavour == BEAM_MIASMA && hurted > 0)
        was_affected = miasma_player();

    if (flavour == BEAM_NUKE) // DISINTEGRATION already handled
        blood_spray(you.pos(), MONS_PLAYER, hurted / 5);

    // Confusion effect for spore explosions
    if (flavour == BEAM_SPORE && hurted && you.holiness() != MH_UNDEAD)
        potion_effect( POT_CONFUSION, 1);

    // handling of missiles
    if (item && item->base_type == OBJ_MISSILES)
    {
        // SPMSL_POISONED is handled via callback _poison_hit_victim()
        // in item_use.cc.
        if (item->sub_type == MI_THROWING_NET)
        {
            if (player_caught_in_net())
            {
                if (beam_source != NON_MONSTER)
                    xom_is_stimulated(64);
                was_affected = true;
            }
        }
        else if (item->special == SPMSL_CURARE)
        {
            if (x_chance_in_y(90 - 3 * you.armour_class(), 100))
            {
                curare_hits_player(actor_to_death_source(agent()),
                                   1 + random2(3));
                was_affected = true;
            }
        }
    }

    // Sticky flame.
    if (name == "sticky flame")
    {
        if (!player_res_sticky_flame())
        {
            napalm_player(random2avg(7, 3) + 1);
            was_affected = true;
        }
    }

    // Acid.
    if (flavour == BEAM_ACID)
        splash_with_acid(5, affects_items);

    if (affects_items)
    {
        // Simple cases for scroll burns.
        if (flavour == BEAM_LAVA || name.find("hellfire") != std::string::npos)
            expose_player_to_element(BEAM_LAVA, burn_power);

        // More complex (geez..)
        if (flavour == BEAM_FIRE && name != "ball of steam")
            expose_player_to_element(BEAM_FIRE, burn_power);

        // Potions exploding.
        if (flavour == BEAM_COLD)
            expose_player_to_element(BEAM_COLD, burn_power);

        // Spore pops.
        if (in_explosion_phase && flavour == BEAM_SPORE)
            expose_player_to_element(BEAM_SPORE, burn_power);
    }

#if DEBUG_DIAGNOSTICS
    mprf(MSGCH_DIAGNOSTICS, "Damage: %d", hurted );
#endif

    was_affected = apply_hit_funcs(&you, hurted) || was_affected;

    if (hurted > 0 || old_hp < you.hp || was_affected)
    {
        if (mons_att_wont_attack(attitude))
        {
            friend_info.hurt++;

            // Beam from player rebounded and hit player.
            // Xom's amusement at the player's being damaged is handled
            // elsewhere.
            if (beam_source == NON_MONSTER)
            {
                if (!aimed_at_feet)
                    xom_is_stimulated(255);
            }
            else if (was_affected)
                xom_is_stimulated(128);
        }
        else
            foe_info.hurt++;
    }

    if (hurted > 0)
    {
        for (unsigned int i = 0; i < messages.size(); ++i)
            mpr(messages[i].c_str(), MSGCH_WARN);
    }

    internal_ouch(hurted);

    range_used += range_used_on_hit(&you);
}

int bolt::beam_source_as_target() const
{
    return (MON_KILL(thrower)     ? beam_source :
            thrower == KILL_MISC  ? MHITNOT
                                  : MHITYOU);
}

void bolt::update_hurt_or_helped(monsters *mon)
{
    if (!mons_atts_aligned(attitude, mons_attitude(mon)))
    {
        if (nasty_to(mon))
            foe_info.hurt++;
        else if (nice_to(mon))
        {
            foe_info.helped++;
            // Accidentally helped a foe.
            if (!is_tracer && !effect_known)
            {
                int interest = 128;
                if (flavour == BEAM_INVISIBILITY && can_see_invis)
                    interest = 32;

                xom_is_stimulated(interest);
            }
        }
    }
    else
    {
        if (nasty_to(mon))
        {
            friend_info.hurt++;

            // Harmful beam from this monster rebounded and hit the monster.
            if (!is_tracer && mon->mindex() == beam_source)
                xom_is_stimulated(128);
        }
        else if (nice_to(mon))
            friend_info.helped++;
    }
}

void bolt::tracer_enchantment_affect_monster(monsters* mon)
{
    // Update friend or foe encountered.
    if (!mons_atts_aligned(attitude, mons_attitude(mon)))
    {
        foe_info.count++;
        foe_info.power += mons_power(mon->type);
    }
    else
    {
        friend_info.count++;
        friend_info.power += mons_power(mon->type);
    }

    handle_stop_attack_prompt(mon);
    if (!beam_cancelled)
    {
        apply_hit_funcs(mon, 0);
        range_used += range_used_on_hit(mon);
    }
}

// Return false if we should skip handling this monster.
bool bolt::determine_damage(monsters* mon, int& preac, int& postac, int& final,
                            std::vector<std::string>& messages)
{
    // Fedhas worshippers can fire through monsters of the same
    // alignment.  This means Fedhas-worshipping players can fire through
    // allied plants, and also means that Fedhas-worshipping oklob plants
    // can fire through plants with the same attitude.
    bool originator_worships_fedhas = false;

    // Checking beam_source to decide whether the player or a monster
    // fired the beam (so we can check their religion).  This is
    // complicated by the fact that this beam may in fact be an
    // explosion caused by a miscast effect.  In that case, the value of
    // beam_source may be negative (god-induced miscast) or greater than
    // NON_MONSTER (various other miscast sources).  So we check whether
    // or not this is an explosion, and also the range of beam_source
    // before attempting to reference env.mons with it. -cao
    if (!is_explosion && beam_source == NON_MONSTER)
        originator_worships_fedhas = (you.religion == GOD_FEDHAS);
    else if (!is_explosion && beam_source >= 0 && beam_source < MAX_MONSTERS)
        originator_worships_fedhas = (env.mons[beam_source].god == GOD_FEDHAS);

    if (!is_enchantment()
        && attitude == mon->attitude
        && originator_worships_fedhas
        && fedhas_protects(mon))
    {
        if (!is_tracer)
        {
            // FIXME: Could use a better message, something about
            // dodging that doesn't sound excessively weird would be
            // nice.
            mprf(MSGCH_GOD, "Fedhas protects %s plant from harm.",
                 attitude == ATT_FRIENDLY ? "your" : "a");
        }
        return (false);
    }

    // preac: damage before AC modifier
    // postac: damage after AC modifier
    // final: damage after AC and resists
    // All these are invalid if we return false.

    // Tracers get the mean.
    if (is_tracer)
        preac = (damage.num * (damage.size + 1)) / 2;
    else
        preac = damage.roll();

    if (!apply_dmg_funcs(mon, preac, messages))
        return (false);

    // Submerged monsters get some perks.
    if (mon->submerged())
    {
        // The beam will pass overhead unless it's aimed at them.
        if (!aimed_at_spot)
            return (false);

        // Electricity is ineffective.
        if (flavour == BEAM_ELECTRICITY)
        {
            if (!is_tracer && you.see_cell(mon->pos()))
                mprf("The %s arcs harmlessly into the water.", name.c_str());
            finish_beam();
            return (false);
        }

        // Otherwise, 1/3 damage reduction.
        preac = (preac * 2) / 3;
    }

    postac = preac - maybe_random2(1 + mon->ac, !is_tracer);

    // Fragmentation has triple AC reduction.
    if (flavour == BEAM_FRAG)
    {
        postac -= maybe_random2(1 + mon->ac, !is_tracer);
        postac -= maybe_random2(1 + mon->ac, !is_tracer);
    }

    postac = std::max(postac, 0);

    // Don't do side effects (beam might miss or be a tracer).
    final = mons_adjust_flavoured(mon, *this, postac, false);

    return (true);
}

void bolt::handle_stop_attack_prompt(monsters* mon)
{
    if ((thrower == KILL_YOU_MISSILE || thrower == KILL_YOU)
        && !is_harmless(mon))
    {
        if (friend_info.count == 1 && !friend_info.dont_stop
            || foe_info.count == 1 && !foe_info.dont_stop)
        {
            if (stop_attack_prompt(mon, true, target))
            {
                beam_cancelled = true;
                finish_beam();
            }
            else
            {
                if (friend_info.count == 1)
                    friend_info.dont_stop = true;
                else if (foe_info.count == 1)
                    foe_info.dont_stop = true;
            }
        }
    }
}

void bolt::tracer_nonenchantment_affect_monster(monsters* mon)
{
    std::vector<std::string> messages;
    int preac, post, final;
    if (!determine_damage(mon, preac, post, final, messages))
        return;

    // Check only if actual damage.
    if (final > 0)
    {
        // Monster could be hurt somewhat, but only apply the
        // monster's power based on how badly it is affected.
        // For example, if a fire giant (power 16) threw a
        // fireball at another fire giant, and it only took
        // 1/3 damage, then power of 5 would be applied.

        // Counting foes is only important for monster tracers.
        if (!mons_atts_aligned(attitude, mons_attitude(mon)))
        {
            foe_info.power += 2 * final * mons_power(mon->type) / preac;
            foe_info.count++;
        }
        else
        {
            friend_info.power += 2 * final * mons_power(mon->type) / preac;
            friend_info.count++;
        }
    }

    // Maybe the user wants to cancel at this point.
    handle_stop_attack_prompt(mon);
    if (beam_cancelled)
        return;

    // Check only if actual damage.
    if (!is_tracer && final > 0)
    {
        for (unsigned int i = 0; i < messages.size(); ++i)
            mpr(messages[i].c_str(), MSGCH_MONSTER_DAMAGE);
    }

    apply_hit_funcs(mon, final);

    // Either way, we could hit this monster, so update range used.
    range_used += range_used_on_hit(mon);
}

void bolt::tracer_affect_monster(monsters* mon)
{
    // Ignore unseen monsters.
    if (!mon->visible_to(&you)
        || (YOU_KILL(thrower) && !you.see_cell(mon->pos())))
    {
        return;
    }

    // Trigger explosion on exploding beams.
    if (is_explosion && !in_explosion_phase)
    {
        finish_beam();
        return;
    }

    if (is_enchantment())
        tracer_enchantment_affect_monster(mon);
    else
        tracer_nonenchantment_affect_monster(mon);
}

void bolt::enchantment_affect_monster(monsters* mon)
{
    // Submerged monsters are unaffected by enchantments.
    if (mon->submerged())
        return;

    god_conduct_trigger conducts[3];
    disable_attack_conducts(conducts);

    bool hit_woke_orc = false;

    // Nasty enchantments will annoy the monster, and are considered
    // naughty (even if a monster might resist).
    if (nasty_to(mon))
    {
        if (YOU_KILL(thrower))
        {
            if (is_sanctuary(mon->pos()) || is_sanctuary(you.pos()))
                remove_sanctuary(true);

            set_attack_conducts(conducts, mon, you.can_see(mon));

            if (you.religion == GOD_BEOGH
                && mons_species(mon->type) == MONS_ORC
                && mon->asleep() && !player_under_penance()
                && you.piety >= piety_breakpoint(2) && mons_near(mon))
            {
                hit_woke_orc = true;
            }
        }
        behaviour_event(mon, ME_ANNOY, beam_source_as_target());
    }
    else
        behaviour_event(mon, ME_ALERT, beam_source_as_target());

    enable_attack_conducts(conducts);

    // Doing this here so that the player gets to see monsters
    // "flicker and vanish" when turning invisible....
    if (effect_known)
        _ench_animation( real_flavour, mon );
    else
        _zap_animation(-1, mon, false);

    // Try to hit the monster with the enchantment.
    const mon_resist_type ench_result = try_enchant_monster(mon);

    if (mon->alive())           // Aftereffects.
    {
        // Mimics become known.
        if (mons_is_mimic(mon->type))
            mimic_alert(mon);

        // Message or record the success/failure.
        switch (ench_result)
        {
        case MON_RESIST:
            if (simple_monster_message(mon, " resists."))
                msg_generated = true;
            break;
        case MON_UNAFFECTED:
            if (simple_monster_message(mon, " is unaffected."))
                msg_generated = true;
            break;
        case MON_AFFECTED:
        case MON_OTHER:         // Should this really be here?
            update_hurt_or_helped(mon);
            break;
        }

        if (hit_woke_orc)
            beogh_follower_convert(mon, true);
    }

    apply_hit_funcs(mon, 0);
    range_used += range_used_on_hit(mon);
}

void bolt::monster_post_hit(monsters* mon, int dmg)
{
    if (YOU_KILL(thrower) && mons_near(mon))
        print_wounds(mon);

    // Don't annoy friendlies or good neutrals if the player's beam
    // did no damage.  Hostiles will still take umbrage.
    if (dmg > 0 || !mon->wont_attack() || !YOU_KILL(thrower))
        behaviour_event(mon, ME_ANNOY, beam_source_as_target());

    // Sticky flame.
    if (name == "sticky flame")
    {
        const int levels = std::min(4, 1 + random2(mon->hit_dice) / 2);
        napalm_monster(mon, whose_kill(), levels);
    }

    bool wake_mimic = true;

    // Handle missile effects.
    if (item && item->base_type == OBJ_MISSILES)
    {
        // SPMSL_POISONED handled via callback _poison_hit_victim() in
        // item_use.cc
        if (item->special == SPMSL_CURARE)
        {
            if (ench_power == AUTOMATIC_HIT
                && curare_hits_monster(agent(), mon, whose_kill(), 2)
                && !mon->alive())
            {
                wake_mimic = false;
            }
        }
    }

    if (wake_mimic && mons_is_mimic(mon->type))
        mimic_alert(mon);
    else if (dmg)
        beogh_follower_convert(mon, true);
}

// Return true if the block succeeded (including reflections.)
bool bolt::attempt_block(monsters* mon)
{
    const int shield_block = mon->shield_bonus();
    bool rc = false;
    if (shield_block > 0)
    {
        const int ht = random2(hit * 130 / 100 + mon->shield_block_penalty());
        if (ht < shield_block)
        {
            rc = true;
            item_def *shield = mon->mslot_item(MSLOT_SHIELD);
            if (is_reflectable(shield))
            {
                if (mon->observable())
                {
                    mprf("%s reflects the %s off %s %s!",
                         mon->name(DESC_CAP_THE).c_str(),
                         name.c_str(),
                         mon->pronoun(PRONOUN_NOCAP_POSSESSIVE).c_str(),
                         shield->name(DESC_PLAIN).c_str());
                    _ident_reflector(shield);
                }
                else if (you.see_cell(pos()))
                    mprf("The %s bounces off of thin air!", name.c_str());

                reflect();
            }
            else
            {
                mprf("%s blocks the %s.",
                     mon->name(DESC_CAP_THE).c_str(),
                     name.c_str());
                finish_beam();
            }

            mon->shield_block_succeeded(agent());
        }
    }

    return (rc);
}

bool bolt::handle_statue_disintegration(monsters* mon)
{
    bool rc = false;
    if ((flavour == BEAM_DISINTEGRATION || flavour == BEAM_NUKE)
        && mons_is_statue(mon->type, true))
    {
        rc = true;
        // Disintegrate the statue.
        if (!silenced(you.pos()))
        {
            if (!you.see_cell(mon->pos()))
                mpr("You hear a hideous screaming!", MSGCH_SOUND);
            else
            {
                mpr("The statue screams as its substance crumbles away!",
                    MSGCH_SOUND);
            }
        }
        else if (you.see_cell(mon->pos()))
        {
            mpr("The statue twists and shakes as its substance "
                "crumbles away!");
        }
        obvious_effect = true;
        update_hurt_or_helped(mon);
        mon->hurt(agent(), INSTANT_DEATH);
        apply_hit_funcs(mon, INSTANT_DEATH);
        // Stop here.
        finish_beam();
    }
    return (rc);
}

void bolt::affect_monster(monsters* mon)
{
    // Don't hit dead monsters.
    if (!mon->alive())
    {
        apply_hit_funcs(mon, 0);
        return;
    }

    // First some special cases.

    // Digging doesn't affect monsters (should it harm earth elementals?)
    if (flavour == BEAM_DIGGING)
    {
        apply_hit_funcs(mon, 0);
        return;
    }

    // Missiles go past bushes.
    if (mon->type == MONS_BUSH && !is_beam && !is_explosion
        && (flavour == BEAM_MISSILE || flavour == BEAM_MMISSILE))
    {
        apply_hit_funcs(mon, 0);
        return;
    }

    // Fire storm creates these, so we'll avoid affecting them
    if (name == "great blast of fire" && mon->type == MONS_FIRE_VORTEX)
    {
        apply_hit_funcs(mon, 0);
        return;
    }

    // Handle tracers separately.
    if (is_tracer)
    {
        tracer_affect_monster(mon);
        return;
    }

    // Visual - wake monsters.
    if (flavour == BEAM_VISUAL)
    {
        behaviour_event(mon, ME_DISTURB, beam_source, source);
        apply_hit_funcs(mon, 0);
        return;
    }

    // Special case: disintegrate (or Shatter) a statue.
    // Since disintegration is an enchantment, it has to be handled
    // here.
    if (handle_statue_disintegration(mon))
        return;

    if (is_enchantment())
    {
        // no to-hit check
        enchantment_affect_monster(mon);
        return;
    }

    if (mon->submerged() && !aimed_at_spot)
        return;                 // passes overhead

    if (is_explosion && !in_explosion_phase)
    {
        // It hit a monster, so the beam should terminate.
        // Don't actually affect the monster; the explosion
        // will take care of that.
        finish_beam();
        return;
    }

    // We need to know how much the monster _would_ be hurt by this,
    // before we decide if it actually hits.
    std::vector<std::string> messages;
    int preac, postac, final;
    if (!determine_damage(mon, preac, postac, final, messages))
        return;

#if DEBUG_DIAGNOSTICS
    mprf(MSGCH_DIAGNOSTICS,
         "Monster: %s; Damage: pre-AC: %d; post-AC: %d; post-resist: %d",
         mon->name(DESC_PLAIN).c_str(), preac, postac, final);
#endif

    // Player beams which hit friendlies or good neutrals will annoy
    // them and be considered naughty if they do damage (this is so as
    // not to penalise players that fling fireballs into a melee with
    // fire elementals on their side - the elementals won't give a sh*t,
    // after all).

    god_conduct_trigger conducts[3];
    disable_attack_conducts(conducts);

    bool hit_woke_orc = false;
    if (nasty_to(mon))
    {
        if (YOU_KILL(thrower) && final > 0)
        {
            // It's not the player's fault if he didn't see the monster
            // or the monster was caught in an unexpected blast of
            // ?immolation.
            const bool okay =
                (!you.can_see(mon)
                    || aux_source == "scroll of immolation" && !effect_known);

            if (is_sanctuary(mon->pos()) || is_sanctuary(you.pos()))
                remove_sanctuary(true);

            set_attack_conducts(conducts, mon, !okay);
        }

        if (you.religion == GOD_BEOGH && mons_species(mon->type) == MONS_ORC
            && mon->asleep() && YOU_KILL(thrower)
            && !player_under_penance() && you.piety >= piety_breakpoint(2)
            && mons_near(mon))
        {
            hit_woke_orc = true;
        }
    }

    // Explosions always 'hit'.
    const bool engulfs = (is_explosion || is_big_cloud);

    if (engulfs && flavour == BEAM_SPORE
        && mon->holiness() == MH_NATURAL)
    {
        apply_enchantment_to_monster(mon);
    }

    // Make a copy of the to-hit before we modify it.
    int beam_hit = hit;
    if (mon->invisible() && !can_see_invis)
        beam_hit /= 2;

    if (mon->backlit())
        beam_hit += 2 + random2(8);

    defer_rand r;
    int rand_ev = random2(mon->ev);
    bool dmsl = mon->type == MONS_KIRKE;

    // FIXME: We're randomising mon->evasion, which is further
    // randomised inside test_beam_hit.  This is so we stay close to the
    // 4.0 to-hit system (which had very little love for monsters).
    if (!engulfs && !_test_beam_hit(beam_hit, rand_ev, is_beam, dmsl, false, r))
    {
        // If the PLAYER cannot see the monster, don't tell them anything!
        if (mon->observable())
        {
            // if it would have hit otherwise...
            if (_test_beam_hit(beam_hit, rand_ev, is_beam, false, false, r))
            {
                msg::stream << mon->name(DESC_CAP_THE) << " deflects the "
                            << name << '!' << std::endl;
            } else {
                msg::stream << "The " << name << " misses "
                            << mon->name(DESC_NOCAP_THE) << '.' << std::endl;
            }
        }
        return;
    }

    // The monster may block the beam.
    if (!engulfs && is_blockable() && attempt_block(mon))
        return;

    update_hurt_or_helped(mon);
    enable_attack_conducts(conducts);

    // We'll say giant spore explosions don't trigger the ally attack conduct
    // for Fedhas worshipers.  Mostly because you can accidentally blow up a
    // group of 8 plants and get placed under penance until the end of time
    // otherwise.  I'd prefer to do this elsewhere but the beam information
    // goes out of scope.
    //
    // Also exempting miscast explosions from this conduct -cao
    if (you.religion == GOD_FEDHAS
        && (flavour == BEAM_SPORE
            || beam_source == NON_MONSTER
               && aux_source.find("your miscasting") != std::string::npos))
    {
        conducts[0].enabled = false;
    }

    if (!is_explosion)
        heard = noisy(loudness, pos(), beam_source) || heard;

    // The beam hit.
    if (mons_near(mon))
    {
        // Monsters don't currently use Phase Shift and are never currently
        // helpless in ranged combat.
        if (hit_verb.empty())
            hit_verb = engulfs ? "engulfs" : "hits";

        mprf("The %s %s %s.",
             name.c_str(),
             hit_verb.c_str(),
             mon->observable() ?
                 mon->name(DESC_NOCAP_THE).c_str() : "something");

    }
    else if (heard && !noise_msg.empty())
        mprf(MSGCH_SOUND, "%s", noise_msg.c_str());
    // The player might hear something, if _they_ fired a missile
    // (not magic beam).
    else if (!silenced(you.pos()) && flavour == BEAM_MISSILE
             && YOU_KILL(thrower))
    {
        mprf(MSGCH_SOUND, "The %s hits something.", name.c_str());
    }

    // handling of missiles
    if (item
        && item->base_type == OBJ_MISSILES
        && item->sub_type == MI_THROWING_NET)
    {
        monster_caught_in_net(mon, *this);
    }

    if (final > 0)
    {
        for (unsigned int i = 0; i < messages.size(); ++i)
            mpr(messages[i].c_str(), MSGCH_MONSTER_DAMAGE);
    }

    // Apply flavoured specials.
    mons_adjust_flavoured(mon, *this, postac, true);

    // If the beam is an actual missile or of the MMISSILE type (Earth magic)
    // we might bleed on the floor.
    if (!engulfs
        && (flavour == BEAM_MISSILE || flavour == BEAM_MMISSILE)
        && !mon->is_summoned() && !mon->submerged())
    {
        // Using raw_damage instead of the flavoured one!
        // assumes DVORP_PIERCING, factor: 0.5
        const int blood = std::min(postac/2, mon->hit_points);
        bleed_onto_floor(mon->pos(), mon->type, blood, true);
    }

    // Now hurt monster.
    mon->hurt(agent(), final, flavour, false);

    int      corpse = -1;
    monsters orig   = *mon;

    if (mon->alive())
        monster_post_hit(mon, final);
    else
    {
        // Preserve name of the source monster if it winds up killing
        // itself.
        if (mon->mindex() == beam_source && source_name.empty())
            source_name = orig.name(DESC_NOCAP_A, true);

        // Prevent spore explosions killing plants from being registered
        // as a Fedhas misconduct.  Deaths can trigger the ally dying or
        // plant dying conducts, but spore explosions shouldn't count
        // for either of those.
        //
        // FIXME: Should be a better way of doing this.  For now, we are
        // just falsifying the death report... -cao
        if (you.religion == GOD_FEDHAS && flavour == BEAM_SPORE
            && fedhas_protects(mon))
        {
            if (mon->attitude == ATT_FRIENDLY)
                mon->attitude = ATT_HOSTILE;
            corpse = monster_die(mon, KILL_MON, beam_source_as_target());
        }
        else
            corpse = monster_die(mon, thrower, beam_source_as_target());
    }

    // Give the callbacks a dead-but-valid monster object.
    if (mon->type == MONS_NO_MONSTER)
    {
        orig.hit_points = -1;
        mon = &orig;
    }

    apply_hit_funcs(mon, final, corpse);
    range_used += range_used_on_hit(mon);
}

bool bolt::has_saving_throw() const
{
    if (aimed_at_feet)
        return (false);

    switch (flavour)
    {
    case BEAM_HASTE:
    case BEAM_MIGHT:
    case BEAM_BERSERK:
    case BEAM_HEALING:
    case BEAM_INVISIBILITY:
    case BEAM_DISPEL_UNDEAD:
    case BEAM_ENSLAVE_SOUL:     // has a different saving throw
    case BEAM_ENSLAVE_DEMON:    // ditto
        return (false);
    default:
        return (true);
    }
}

bool _ench_flavour_affects_monster(beam_type flavour, const monsters* mon)
{
    bool rc = true;
    switch (flavour)
    {
    case BEAM_POLYMORPH:
        rc = mon->can_mutate();
        break;

    case BEAM_DEGENERATE:
        rc = (mon->holiness() == MH_NATURAL
              && mon->type != MONS_PULSATING_LUMP);
        break;

    case BEAM_ENSLAVE_UNDEAD:
        rc = (mon->holiness() == MH_UNDEAD && mon->attitude != ATT_FRIENDLY);
        break;

    case BEAM_ENSLAVE_SOUL:
        rc = (mon->holiness() == MH_NATURAL && mon->attitude != ATT_FRIENDLY);
        break;

    case BEAM_DISPEL_UNDEAD:
        rc = (mon->holiness() == MH_UNDEAD);
        break;

    case BEAM_ENSLAVE_DEMON:
        rc = (mon->holiness() == MH_DEMONIC && !mon->friendly());
        break;

    case BEAM_PAIN:
        rc = !mon->res_negative_energy();
        break;

    case BEAM_HIBERNATION:
        rc = mon->can_hibernate();
        break;

    case BEAM_PORKALATOR:
        rc = (mon->holiness() == MH_DEMONIC && mon->type != MONS_HELL_HOG)
              || (mon->holiness() == MH_NATURAL && mon->type != MONS_HOG);
        break;

    default:
        break;
    }

    return rc;
}

bool enchant_monster_with_flavour(monsters* mon, actor *foe,
                                  beam_type flavour, int powc)
{
    bolt dummy;
    dummy.flavour = flavour;
    dummy.ench_power = powc;
    dummy.set_agent(foe);
    dummy.apply_enchantment_to_monster(mon);
    return dummy.obvious_effect;
}

mon_resist_type bolt::try_enchant_monster(monsters *mon)
{
    // Early out if the enchantment is meaningless.
    if (!_ench_flavour_affects_monster(flavour, mon))
        return (MON_UNAFFECTED);

    // Check magic resistance.
    if (has_saving_throw())
    {
        if (mons_immune_magic(mon))
            return (MON_UNAFFECTED);

        // (Very) ugly things and shapeshifters will never resist
        // polymorph beams.
        if (flavour == BEAM_POLYMORPH
            && (mon->type == MONS_UGLY_THING
                || mon->type == MONS_VERY_UGLY_THING
                || mon->is_shapeshifter()))
        {
            ;
        }
        else
        {
            if (mon->check_res_magic(ench_power))
                return (MON_RESIST);
        }
    }

    return (apply_enchantment_to_monster(mon));
}

mon_resist_type bolt::apply_enchantment_to_monster(monsters* mon)
{
    // Gigantic-switches-R-Us
    switch (flavour)
    {
    case BEAM_TELEPORT:
        if (mon->observable())
            obvious_effect = true;
        monster_teleport(mon, false);
        return (MON_AFFECTED);

    case BEAM_BLINK:
        if (mon->observable())
            obvious_effect = true;
        monster_blink(mon);
        return (MON_AFFECTED);

    case BEAM_BLINK_CLOSE:
        if (mon->observable())
            obvious_effect = true;
        blink_other_close(mon, source);
        return (MON_AFFECTED);

    case BEAM_POLYMORPH:
        if (mon->mutate())
            obvious_effect = true;
        if (YOU_KILL(thrower))
        {
            did_god_conduct(DID_DELIBERATE_MUTATING, 2 + random2(3),
                            effect_known);
        }
        return (MON_AFFECTED);

    case BEAM_BANISH:
        if (you.level_type == LEVEL_ABYSS)
            simple_monster_message(mon, " wobbles for a moment.");
        else
            mon->banish();
        obvious_effect = true;
        return (MON_AFFECTED);

    case BEAM_DEGENERATE:
        if (monster_polymorph(mon, MONS_PULSATING_LUMP))
            obvious_effect = true;
        return (MON_AFFECTED);

    case BEAM_DISPEL_UNDEAD:
        if (simple_monster_message(mon, " convulses!"))
            obvious_effect = true;
        mon->hurt(agent(), damage.roll());
        return (MON_AFFECTED);

    case BEAM_ENSLAVE_UNDEAD:
    {
        const god_type god =
            (crawl_state.is_god_acting()) ? crawl_state.which_god_acting()
                                          : GOD_NO_GOD;
#if DEBUG_DIAGNOSTICS
        mprf(MSGCH_DIAGNOSTICS,
             "HD: %d; pow: %d", mon->hit_dice, ench_power);
#endif

        obvious_effect = true;
        if (player_will_anger_monster(mon))
        {
            simple_monster_message(mon, " is repulsed!");
            return (MON_OTHER);
        }

        simple_monster_message(mon, " is enslaved.");

        // Wow, permanent enslaving!
        mon->attitude = ATT_FRIENDLY;
        behaviour_event(mon, ME_ALERT, MHITNOT);

        mons_make_god_gift(mon, god);

        return (MON_AFFECTED);
    }

    case BEAM_ENSLAVE_SOUL:
    {
#if DEBUG_DIAGNOSTICS
        mprf(MSGCH_DIAGNOSTICS,
             "HD: %d; pow: %d", mon->hit_dice, ench_power);
#endif

        if (!mons_can_be_zombified(mon) || mons_intel(mon) < I_NORMAL)
        {
            simple_monster_message(mon, " is unaffected.");
            return (MON_OTHER);
        }

        // The monster can be no more than lightly wounded/damaged,
        // using the formula from mon-stuff.cc:mons_get_damage_level().
        if (mon->hit_points <= mon->max_hit_points * 3 / 4)
        {
            simple_monster_message(mon, "'s soul is too badly injured.");
            return (MON_OTHER);
        }

        obvious_effect = true;
        const int duration = you.skills[SK_INVOCATIONS] * 3 / 4 + 2;
        mon->add_ench(mon_enchant(ENCH_SOUL_RIPE, 0, KC_YOU, duration * 10));
        simple_monster_message(mon, "'s soul is now ripe for the taking.");
        return (MON_AFFECTED);
    }

    case BEAM_ENSLAVE_DEMON:
#if DEBUG_DIAGNOSTICS
        mprf(MSGCH_DIAGNOSTICS,
             "HD: %d; pow: %d", mon->hit_dice, ench_power);
#endif

        if (mon->hit_dice * 11 / 2 >= random2(ench_power)
            || mons_is_unique(mon->type))
        {
            return (MON_RESIST);
        }

        obvious_effect = true;
        if (player_will_anger_monster(mon))
        {
            simple_monster_message(mon, " is repulsed!");
            return (MON_OTHER);
        }

        simple_monster_message(mon, " is enslaved.");

        // Wow, permanent enslaving! (sometimes)
        if (one_chance_in(2 + mon->hit_dice / 4))
            mon->attitude = ATT_FRIENDLY;
        else
            mon->add_ench(ENCH_CHARM);
        behaviour_event(mon, ME_ALERT, MHITNOT);
        return (MON_AFFECTED);

    case BEAM_PAIN:             // pain/agony
        if (simple_monster_message(mon, " convulses in agony!"))
            obvious_effect = true;

        if (name.find("agony") != std::string::npos) // agony
            mon->hit_points = std::max(mon->hit_points/2, 1);
        else                    // pain
            mon->hurt(agent(), damage.roll(), flavour);
        return (MON_AFFECTED);

    case BEAM_DISINTEGRATION:   // disrupt/disintegrate
        if (simple_monster_message(mon, " is blasted."))
            obvious_effect = true;
        mon->hurt(agent(), damage.roll(), flavour);
        return (MON_AFFECTED);

    case BEAM_HIBERNATION:
        if (mon->can_hibernate())
        {
            if (simple_monster_message(mon, " looks drowsy..."))
                obvious_effect = true;
            mon->hibernate();
            return (MON_AFFECTED);
        }
        return (MON_UNAFFECTED);

    case BEAM_CORONA:
        if (backlight_monsters(mon->pos(), hit, 0))
        {
            obvious_effect = true;
            return (MON_AFFECTED);
        }
        return (MON_UNAFFECTED);

    case BEAM_SLOW:
        obvious_effect = do_slow_monster(mon, whose_kill());
        return (MON_AFFECTED);

    case BEAM_HASTE:
        if (mon->del_ench(ENCH_SLOW, true))
        {
            if (simple_monster_message(mon, " is no longer moving slowly."))
                obvious_effect = true;

            return (MON_AFFECTED);
        }

        // Not slowed, haste it.
        if (!mon->has_ench(ENCH_HASTE)
            && !mons_is_stationary(mon)
            && mon->add_ench(ENCH_HASTE))
        {
            if (!mon->paralysed() && !mon->petrified()
                && simple_monster_message(mon, " seems to speed up."))
            {
                obvious_effect = true;
            }
        }
        return (MON_AFFECTED);

    case BEAM_MIGHT:
        if (!mon->has_ench(ENCH_MIGHT)
            && !mons_is_stationary(mon)
            && mon->add_ench(ENCH_MIGHT))
        {
            if (simple_monster_message(mon, " seems to grow stronger."))
                obvious_effect = true;
        }
        return (MON_AFFECTED);

    case BEAM_BERSERK:
        if (!mon->berserk())
        {
            // currently from potion, hence voluntary
            mon->go_berserk(true);
            // can't return this from go_berserk, unfortunately
            obvious_effect = mons_near(mon);
        }
        return (MON_AFFECTED);

    case BEAM_HEALING:
        if (YOU_KILL(thrower))
        {
            if (cast_healing(5 + damage.roll(), false, mon->pos()) > 0)
                obvious_effect = true;
            msg_generated = true; // to avoid duplicate "nothing happens"
        }
        else if (mon->heal(5 + damage.roll()))
        {
            if (mon->hit_points == mon->max_hit_points)
            {
                if (simple_monster_message(mon, "'s wounds heal themselves!"))
                    obvious_effect = true;
            }
            else if (simple_monster_message(mon, " is healed somewhat."))
                obvious_effect = true;
        }
        return (MON_AFFECTED);

    case BEAM_PARALYSIS:
        apply_bolt_paralysis(mon);
        return (MON_AFFECTED);

    case BEAM_PETRIFY:
        apply_bolt_petrify(mon);
        return (MON_AFFECTED);

    case BEAM_SPORE:
    case BEAM_CONFUSION:
        if (!mons_class_is_confusable(mon->type))
            return (MON_UNAFFECTED);

        if (mon->add_ench(mon_enchant(ENCH_CONFUSION, 0, whose_kill())))
        {
            // FIXME: Put in an exception for things you won't notice
            // becoming confused.
            if (simple_monster_message(mon, " appears confused."))
                obvious_effect = true;
        }
        return (MON_AFFECTED);

    case BEAM_SLEEP:
        if (mon->has_ench(ENCH_SLEEPY))
            return (MON_UNAFFECTED);

        if (mon->add_ench(mon_enchant(ENCH_SLEEPY, 0, whose_kill())))
        {
            if (simple_monster_message(mon, " falls asleep!"))
                obvious_effect = true;
        }
        return (MON_AFFECTED);

    case BEAM_INVISIBILITY:
    {
        // Store the monster name before it becomes an "it" -- bwr
        const std::string monster_name = mon->name(DESC_CAP_THE);

        if (!mon->has_ench(ENCH_INVIS) && mon->add_ench(ENCH_INVIS))
        {
            // A casting of invisibility erases backlight.
            mon->del_ench(ENCH_CORONA);

            // Can't use simple_monster_message() here, since it checks
            // for visibility of the monster (and it's now invisible).
            // -- bwr
            if (mons_near(mon))
            {
                mprf("%s flickers %s",
                     monster_name.c_str(),
                     mon->visible_to(&you) ? "for a moment."
                                                 : "and vanishes!" );

                if (!mon->visible_to(&you))
                    autotoggle_autopickup(true);
            }

            obvious_effect = true;
        }
        return (MON_AFFECTED);
    }
    case BEAM_CHARM:
        if (player_will_anger_monster(mon))
        {
            simple_monster_message(mon, " is repulsed!");
            return (MON_OTHER);
        }

        if (!mon->has_ench(ENCH_CHARM))
        {
            // XXX: Another hackish thing for Pikel's band neutrality.
            if (mon->type == MONS_PIKEL)
                pikel_band_neutralise();

            if (simple_monster_message(mon, " is charmed."))
                obvious_effect = true;
            mon->add_ench(ENCH_CHARM);
        }
        return (MON_AFFECTED);

    case BEAM_PORKALATOR:
    {
        // Monster's which use the ghost structure can't be properly
        // restored from hog form.
        if (mons_is_ghost_demon(mon->type))
            return (MON_UNAFFECTED);

        monsters orig_mon(*mon);
        if (monster_polymorph(mon, (mon->holiness() == MH_DEMONIC ?
                                        MONS_HELL_HOG : MONS_HOG)))
        {
            obvious_effect = true;

            // Don't restore items to monster if it reverts.
            orig_mon.inv = mon->inv;

            // For monster reverting to original form.
            mon->props[ORIG_MONSTER_KEY] = orig_mon;
        }


        return (MON_AFFECTED);
    }

    default:
        break;
    }

    return (MON_AFFECTED);
}


// Extra range used on hit.
int bolt::range_used_on_hit(const actor* victim) const
{
    int used = 0;

    // Non-beams can only affect one thing (player/monster).
    if (!is_beam)
        used = BEAM_STOP;
    else if (is_enchantment())
        used = (flavour == BEAM_DIGGING ? 0 : BEAM_STOP);
    // Hellfire stops for nobody!
    else if (name.find("hellfire") != std::string::npos)
        used = 0;
    // Generic explosion.
    else if (is_explosion || is_big_cloud)
        used = BEAM_STOP;
    // Plant spit.
    else if (flavour == BEAM_ACID)
        used = BEAM_STOP;
    // Lightning goes through things.
    else if (flavour == BEAM_ELECTRICITY)
        used = 0;
    else
        used = 1;

    // Assume we didn't hit, after all.
    if (is_tracer && beam_source == NON_MONSTER && used == BEAM_STOP)
        return 1;

    if (in_explosion_phase)
        return (used);

    for (unsigned int i = 0; i < range_funcs.size(); ++i)
        if ( (*range_funcs[i])(*this, victim, used) )
            break;

    return (used);
}

// Takes a bolt and refines it for use in the explosion function.
// Explosions which do not follow from beams (e.g., scrolls of
// immolation) bypass this function.
void bolt::refine_for_explosion()
{
    ASSERT(!special_explosion);

    const char *seeMsg  = NULL;
    const char *hearMsg = NULL;

    if (ex_size == 0)
        ex_size = 1;

    // Assume that the player can see/hear the explosion, or
    // gets burned by it anyway.  :)
    msg_generated = true;

    // tmp needed so that what c_str() points to doesn't go out of scope
    // before the function ends.
    std::string tmp;
    if (item != NULL)
    {
        tmp  = "The " + item->name(DESC_PLAIN, false, false, false)
               + " explodes!";

        seeMsg  = tmp.c_str();
        hearMsg = "You hear an explosion.";

        type    = dchar_glyph(DCHAR_FIRED_BURST);
    }

    if (name.find("hellfire") != std::string::npos)
    {
        seeMsg  = "The hellfire explodes!";
        hearMsg = "You hear a strangely unpleasant explosion.";

        type    = dchar_glyph(DCHAR_FIRED_BURST);
        flavour = BEAM_HELLFIRE;
    }

    if (name == "fireball")
    {
        seeMsg  = "The fireball explodes!";
        hearMsg = "You hear an explosion.";

        type    = dchar_glyph(DCHAR_FIRED_BURST);
        flavour = BEAM_FIRE;
        ex_size = 1;
    }

    if (name == "orb of electricity")
    {
        seeMsg  = "The orb of electricity explodes!";
        hearMsg = "You hear a clap of thunder!";

        type       = dchar_glyph(DCHAR_FIRED_BURST);
        flavour    = BEAM_ELECTRICITY;
        colour     = LIGHTCYAN;
        damage.num = 1;
        ex_size    = 2;
    }

    if (name == "orb of energy")
    {
        seeMsg  = "The orb of energy explodes.";
        hearMsg = "You hear an explosion.";
    }

    if (name == "metal orb")
    {
        seeMsg  = "The orb explodes into a blast of deadly shrapnel!";
        hearMsg = "You hear an explosion!";

        name    = "blast of shrapnel";
        type    = dchar_glyph(DCHAR_FIRED_ZAP);
        flavour = BEAM_FRAG;     // Sets it from pure damage to shrapnel
                                 // (which is absorbed extra by armour).
    }

    if (name == "great blast of cold")
    {
        seeMsg  = "The blast explodes into a great storm of ice!";
        hearMsg = "You hear a raging storm!";

        name       = "ice storm";
        type       = dchar_glyph(DCHAR_FIRED_ZAP);
        colour     = WHITE;
        ex_size    = is_tracer ? 3 : (2 + (random2(ench_power) > 75));
    }

    if (name == "stinking cloud")
    {
        seeMsg     = "The beam expands into a vile cloud!";
        hearMsg    = "You hear a gentle \'poof\'.";
    }

    if (name == "foul vapour")
    {
        seeMsg     = "The ball expands into a vile cloud!";
        hearMsg    = "You hear a gentle \'poof\'.";
        if (!is_tracer)
            name = "stinking cloud";
    }

    if (name == "potion")
    {
        seeMsg     = "The potion explodes!";
        hearMsg    = "You hear an explosion!";
        if (!is_tracer)
        {

            name = "cloud";
            ASSERT(flavour >= BEAM_POTION_STINKING_CLOUD
                   && flavour <= BEAM_POTION_RANDOM);
            const int newcolour = _potion_beam_flavour_to_colour(flavour);
            if (newcolour >= 0)
                colour = newcolour;
        }
    }

    if (seeMsg == NULL)
    {
        seeMsg  = "The beam explodes into a cloud of software bugs!";
        hearMsg = "You hear the sound of one hand clapping!";
    }


    if (!is_tracer && *seeMsg && *hearMsg)
    {
        heard = player_can_hear(target);
        // Check for see/hear/no msg.
        if (you.see_cell(target) || target == you.pos())
            mpr(seeMsg);
        else
        {
            if (!heard)
                msg_generated = false;
            else
                mpr(hearMsg, MSGCH_SOUND);
        }
    }
}

typedef std::vector< std::vector<coord_def> > sweep_type;

static sweep_type _radial_sweep(int r)
{
    sweep_type result;
    sweep_type::value_type work;

    // Center first.
    work.push_back( coord_def(0,0) );
    result.push_back(work);

    for (int rad = 1; rad <= r; ++rad)
    {
        work.clear();

        for (int d = -rad; d <= rad; ++d)
        {
            // Don't put the corners in twice!
            if (d != rad && d != -rad)
            {
                work.push_back( coord_def(-rad, d) );
                work.push_back( coord_def(+rad, d) );
            }

            work.push_back( coord_def(d, -rad) );
            work.push_back( coord_def(d, +rad) );
        }
        result.push_back(work);
    }
    return result;
}

#define MAX_EXPLOSION_RADIUS 9

// Returns true if we saw something happening.
bool bolt::explode(bool show_more, bool hole_in_the_middle)
{
    ASSERT(!special_explosion);
    ASSERT(!in_explosion_phase);
    ASSERT(ex_size > 0);

    // explode() can be called manually without setting real_flavour.
    // FIXME: The entire flavour/real_flavour thing needs some
    // rewriting!
    if (real_flavour == BEAM_CHAOS || real_flavour == BEAM_RANDOM)
        flavour = real_flavour;
    else
        real_flavour = flavour;

    const int r = std::min(ex_size, MAX_EXPLOSION_RADIUS);
    in_explosion_phase = true;

    if (is_sanctuary(pos()))
    {
        if (!is_tracer && you.see_cell(pos()) && !name.empty())
        {
            mprf(MSGCH_GOD, "By Zin's power, the %s is contained.",
                 name.c_str());
            return (true);
        }
        return (false);
    }

#if DEBUG_DIAGNOSTICS
    mprf(MSGCH_DIAGNOSTICS,
         "explosion at (%d, %d) : t=%d c=%d f=%d hit=%d dam=%dd%d r=%d",
         pos().x, pos().y, type, colour, flavour, hit, damage.num, damage.size, r);
#endif

    if (!is_tracer)
    {
        loudness = 10 + 5 * r;

        bool heard_expl = noisy(loudness, pos(), beam_source);
        heard = heard || heard_expl;

        if (heard_expl && !noise_msg.empty() && !you.see_cell(pos()))
            mprf(MSGCH_SOUND, "%s", noise_msg.c_str());
    }

    // Run DFS to determine which cells are influenced
    explosion_map exp_map;
    exp_map.init(INT_MAX);
    determine_affected_cells(exp_map, coord_def(), 0, r, true, true);

#if defined(TARGET_OS_WINDOWS) && !defined(USE_TILE)
    // turn buffering off
    bool oldValue = true;
    if (!is_tracer)
        oldValue = set_buffering(false);
#endif

    // We get a bit fancy, drawing all radius 0 effects, then radius
    // 1, radius 2, etc.  It looks a bit better that way.
    const std::vector< std::vector<coord_def> > sweep = _radial_sweep(r);
    const coord_def centre(9,9);

    typedef sweep_type::const_iterator siter;
    typedef sweep_type::value_type::const_iterator viter;

    // Draw pass.
    if (!is_tracer)
    {
        for (siter ci = sweep.begin(); ci != sweep.end(); ++ci)
        {
            for (viter cci = ci->begin(); cci != ci->end(); ++cci)
            {
                const coord_def delta = *cci;

                if (delta.origin() && hole_in_the_middle)
                    continue;

                if (exp_map(delta + centre) < INT_MAX)
                    explosion_draw_cell(delta + pos());
            }
            update_screen();

            int explode_delay = 50;
            // Scale delay to match change in arena_delay.
            if (crawl_state.arena)
            {
                explode_delay *= Options.arena_delay;
                explode_delay /= 600;
            }

            delay(explode_delay);
        }
    }

    // Affect pass.
    int cells_seen = 0;
    for (siter ci = sweep.begin(); ci != sweep.end(); ++ci)
    {
        for (viter cci = ci->begin(); cci != ci->end(); ++cci)
        {
            const coord_def delta = *cci;

            if (delta.origin() && hole_in_the_middle)
                continue;

            if (exp_map(delta + centre) < INT_MAX)
            {
                if (you.see_cell(delta + pos()))
                    ++cells_seen;

                explosion_affect_cell(delta + pos());
            }
        }
    }

#if defined(TARGET_OS_WINDOWS) && !defined(USE_TILE)
    if (!is_tracer)
        set_buffering(oldValue);
#endif

    // Delay after entire explosion has been drawn.
    if (!is_tracer && cells_seen > 0 && show_more)
    {
        int explode_delay = 150;
        // Scale delay to match change in arena_delay.
        if (crawl_state.arena)
        {
            explode_delay *= Options.arena_delay;
            explode_delay /= 600;
        }

        delay(explode_delay);
    }

    return (cells_seen > 0);
}

void bolt::explosion_draw_cell(const coord_def& p)
{
    if (you.see_cell(p))
    {
        const coord_def drawpos = grid2view(p);
#ifdef USE_TILE
        if (in_los_bounds(drawpos))
            tiles.add_overlay(p, tileidx_bolt(*this));
#else
        // bounds check
        if (in_los_bounds(drawpos))
        {
            cgotoxy(drawpos.x, drawpos.y, GOTO_DNGN);
            put_colour_ch(colour == BLACK ? random_colour() : colour,
                          dchar_glyph(DCHAR_EXPLOSION));
        }
#endif
    }
}

void bolt::explosion_affect_cell(const coord_def& p)
{
    // pos() = target during an explosion, so restore it after affecting
    // the cell.
    const coord_def orig_pos = target;

    fake_flavour();
    target = p;
    affect_cell();
    flavour = real_flavour;

    target = orig_pos;
}

// Uses DFS
void bolt::determine_affected_cells(explosion_map& m, const coord_def& delta,
                                    int count, int r,
                                    bool stop_at_statues, bool stop_at_walls)
{
    const coord_def centre(9,9);
    const coord_def loc = pos() + delta;

    // A bunch of tests for edge cases.
    if (delta.rdist() > centre.rdist()
        || (delta.abs() > r*(r+1))
        || (count > 10*r)
        || !map_bounds(loc)
        || is_sanctuary(loc))
    {
        return;
    }

    const dungeon_feature_type dngn_feat = grd(loc);

    // Check to see if we're blocked by a wall.
    if (feat_is_wall(dngn_feat)
        || dngn_feat == DNGN_SECRET_DOOR
        || feat_is_closed_door(dngn_feat))
    {
        // Special case: explosion originates from rock/statue
        // (e.g. Lee's Rapid Deconstruction) - in this case, ignore
        // solid cells at the center of the explosion.
        if (stop_at_walls && !(delta.origin() && affects_wall(dngn_feat)))
            return;
    }

    if (feat_is_solid(dngn_feat) && !feat_is_wall(dngn_feat) && stop_at_statues)
        return;

    // Check if it passes the callback functions.
    bool hits = true;
    for (unsigned int i = 0; i < aoe_funcs.size(); ++i)
        hits = (*aoe_funcs[i])(*this, loc) && hits;

    if (hits) {
        // Hmm, I think we're OK.
        m(delta + centre) = std::min(count, m(delta + centre));
    }

    // Now recurse in every direction.
    for (int i = 0; i < 8; ++i)
    {
        const coord_def new_delta = delta + Compass[i];

        if (new_delta.rdist() > centre.rdist())
            continue;

        // Is that cell already covered?
        if (m(new_delta + centre) <= count)
            continue;

        int cadd = 5;
        // Changing direction (e.g. looking around a wall) costs more.
        if (delta.x * Compass[i].x < 0 || delta.y * Compass[i].y < 0)
            cadd = 17;

        determine_affected_cells(m, new_delta, count + cadd, r,
                                 stop_at_statues, stop_at_walls);
    }
}

// Returns true if the beam is harmful (ignoring monster
// resists) -- mon is given for 'special' cases where,
// for example, "Heal" might actually hurt undead, or
// "Holy Word" being ignored by holy monsters, etc.
//
// Only enchantments should need the actual monster type
// to determine this; non-enchantments are pretty
// straightforward.
bool bolt::nasty_to(const monsters *mon) const
{
    // Cleansing flame.
    if (flavour == BEAM_HOLY)
        return (mon->res_holy_energy(agent()) <= 0);

    // Take care of other non-enchantments.
    if (!is_enchantment())
        return (true);

    // Now for some non-hurtful enchantments.
    if (flavour == BEAM_DIGGING)
        return (false);

    // Positive effects.
    if (nice_to(mon))
        return (false);

    // No charming holy beings!
    if (flavour == BEAM_CHARM)
        return (mon->is_holy());

    // Friendly and good neutral monsters don't mind being teleported.
    if (flavour == BEAM_TELEPORT)
        return (!mon->wont_attack());

    // degeneration / sleep / enslave soul
    if (flavour == BEAM_DEGENERATE
        || flavour == BEAM_HIBERNATION
        || flavour == BEAM_ENSLAVE_SOUL)
    {
        return (mon->holiness() == MH_NATURAL);
    }

    // dispel undead / control undead
    if (flavour == BEAM_DISPEL_UNDEAD || flavour == BEAM_ENSLAVE_UNDEAD)
        return (mon->holiness() == MH_UNDEAD);

    // pain / agony
    if (flavour == BEAM_PAIN)
        return (!mon->res_negative_energy());

    // control demon
    if (flavour == BEAM_ENSLAVE_DEMON)
        return (mon->holiness() == MH_DEMONIC);

    // everything else is considered nasty by everyone
    return (true);
}

// Return true if the bolt is considered nice by mon.
// This is not the inverse of nasty_to(): the bolt needs to be
// actively positive.
bool bolt::nice_to(const monsters *mon) const
{
    // Polymorphing a (very) ugly thing will mutate it into a different
    // (very) ugly thing.
    if (flavour == BEAM_POLYMORPH)
    {
        return (mon->type == MONS_UGLY_THING
                || mon->type == MONS_VERY_UGLY_THING);
    }

    if (flavour == BEAM_HASTE
        || flavour == BEAM_HEALING
        || flavour == BEAM_INVISIBILITY)
    {
        return (true);
    }

    return (false);
}

////////////////////////////////////////////////////////////////////////////
// bolt

// A constructor for bolt to help guarantee that we start clean (this has
// caused way too many bugs).  Putting it here since there's no good place to
// put it, and it doesn't do anything other than initialise its members.
//
// TODO: Eventually it'd be nice to have a proper factory for these things
// (extended from setup_mons_cast() and zapping() which act as limited ones).
bolt::bolt() : range(-2), type('*'), colour(BLACK), flavour(BEAM_MAGIC),
    real_flavour(BEAM_MAGIC), drop_item(false), item(NULL), source(), target(),
    damage(0, 0), ench_power(0), hit(0), thrower(KILL_MISC), ex_size(0),
    beam_source(MHITNOT), source_name(), name(), short_name(), hit_verb(),
    loudness(0), noise_msg(), is_beam(false), is_explosion(false),
    is_big_cloud(false), aimed_at_spot(false), aux_source(),
    affects_nothing(false), affects_items(true), effect_known(true),
    draw_delay(15), special_explosion(NULL), range_funcs(), damage_funcs(),
    hit_funcs(), aoe_funcs(), obvious_effect(false), seen(false), heard(false),
    path_taken(), range_used(0), is_tracer(false), aimed_at_feet(false),
    msg_generated(false), passed_target(false), in_explosion_phase(false),
    smart_monster(false), can_see_invis(false), attitude(ATT_HOSTILE),
    foe_ratio(0), chose_ray(false), beam_cancelled(false),
    dont_stop_player(false), bounces(false), bounce_pos(), reflections(0),
    reflector(-1), auto_hit(false)
{
}

killer_type bolt::killer() const
{
    if (flavour == BEAM_BANISH)
        return (KILL_RESET);

    switch (thrower)
    {
    case KILL_YOU:
    case KILL_YOU_MISSILE:
        return (flavour == BEAM_PARALYSIS
                || flavour == BEAM_PETRIFY) ? KILL_YOU : KILL_YOU_MISSILE;

    case KILL_MON:
    case KILL_MON_MISSILE:
        return (KILL_MON_MISSILE);

    case KILL_YOU_CONF:
        return (KILL_YOU_CONF);

    default:
        return (KILL_MON_MISSILE);
    }
}

void bolt::set_target(const dist &d)
{
    if (!d.isValid)
        return;

    target = d.target;

    chose_ray = d.choseRay;
    if (d.choseRay)
        ray = d.ray;

    if (d.isEndpoint)
        aimed_at_spot = true;
}

void bolt::setup_retrace()
{
    if (pos().x && pos().y)
        target = pos();

    std::swap(source, target);
    chose_ray       = false;
    affects_nothing = true;
    aimed_at_spot   = true;
    range_used      = 0;
}

void bolt::set_agent(actor *actor)
{
    // NULL actor is fine by us.
    if (!actor)
        return;

    if (actor->atype() == ACT_PLAYER)
    {
        thrower = KILL_YOU_MISSILE;
    }
    else
    {
        thrower = KILL_MON_MISSILE;
        beam_source = actor->mindex();
    }
}

actor* bolt::agent() const
{
    if (YOU_KILL(thrower))
        return (&you);
    else if (!invalid_monster_index(beam_source))
        return (&menv[beam_source]);
    else
        return (NULL);
}

bool bolt::is_enchantment() const
{
    return (flavour >= BEAM_FIRST_ENCHANTMENT
            && flavour <= BEAM_LAST_ENCHANTMENT);
}

std::string bolt::get_short_name() const
{
    if (!short_name.empty())
        return (short_name);

    if (item != NULL && item->is_valid())
        return item->name(DESC_NOCAP_A, false, false, false, false,
                          ISFLAG_IDENT_MASK | ISFLAG_COSMETIC_MASK
                          | ISFLAG_RACIAL_MASK);

    if (real_flavour == BEAM_RANDOM || real_flavour == BEAM_CHAOS)
        return beam_type_name(real_flavour);

    if (flavour == BEAM_FIRE && name == "sticky fire")
        return ("sticky fire");

    if (flavour == BEAM_ELECTRICITY && is_beam)
        return ("lightning");

    if (flavour == BEAM_NONE || flavour == BEAM_MISSILE
        || flavour == BEAM_MMISSILE)
    {
        return (name);
    }

    return beam_type_name(flavour);
}

std::string beam_type_name(beam_type type)
{
    switch (type)
    {
    case BEAM_NONE:                 return ("none");
    case BEAM_MISSILE:              return ("missile");
    case BEAM_MMISSILE:             return ("magic missile");

    case BEAM_POTION_FIRE:          // fall through
    case BEAM_FIRE:                 return ("fire");

    case BEAM_POTION_COLD:          // fall through
    case BEAM_COLD:                 return ("cold");

    case BEAM_MAGIC:                return ("magic");
    case BEAM_ELECTRICITY:          return ("electricity");

    case BEAM_POTION_STINKING_CLOUD:
    case BEAM_POTION_POISON:        // fall through
    case BEAM_POISON:               return ("poison");

    case BEAM_NEG:                  return ("negative energy");
    case BEAM_ACID:                 return ("acid");

    case BEAM_MIASMA:               // fall through
    case BEAM_POTION_MIASMA:        return ("miasma");

    case BEAM_SPORE:                return ("spores");
    case BEAM_POISON_ARROW:         return ("poison arrow");
    case BEAM_HELLFIRE:             return ("hellfire");
    case BEAM_NAPALM:               return ("sticky fire");

    case BEAM_POTION_STEAM:         // fall through
    case BEAM_STEAM:                return ("steam");

    case BEAM_ENERGY:               return ("energy");
    case BEAM_HOLY:                 return ("holy energy");
    case BEAM_FRAG:                 return ("fragments");
    case BEAM_LAVA:                 return ("magma");
    case BEAM_ICE:                  return ("ice");
    case BEAM_NUKE:                 return ("nuke");
    case BEAM_RANDOM:               return ("random");
    case BEAM_CHAOS:                return ("chaos");
    case BEAM_SLOW:                 return ("slow");
    case BEAM_HASTE:                return ("haste");
    case BEAM_MIGHT:                return ("might");
    case BEAM_HEALING:              return ("healing");
    case BEAM_PARALYSIS:            return ("paralysis");
    case BEAM_CONFUSION:            return ("confusion");
    case BEAM_INVISIBILITY:         return ("invisibility");
    case BEAM_DIGGING:              return ("digging");
    case BEAM_TELEPORT:             return ("teleportation");
    case BEAM_POLYMORPH:            return ("polymorph");
    case BEAM_CHARM:                return ("enslave");
    case BEAM_BANISH:               return ("banishment");
    case BEAM_DEGENERATE:           return ("degeneration");
    case BEAM_ENSLAVE_UNDEAD:       return ("enslave undead");
    case BEAM_ENSLAVE_SOUL:         return ("enslave soul");
    case BEAM_PAIN:                 return ("pain");
    case BEAM_DISPEL_UNDEAD:        return ("dispel undead");
    case BEAM_DISINTEGRATION:       return ("disintegration");
    case BEAM_ENSLAVE_DEMON:        return ("enslave demon");
    case BEAM_BLINK:                return ("blink");
    case BEAM_BLINK_CLOSE:          return ("blink close");
    case BEAM_PETRIFY:              return ("petrify");
    case BEAM_CORONA:               return ("backlight");
    case BEAM_PORKALATOR:           return ("porkalator");
    case BEAM_HIBERNATION:          return ("hibernation");
    case BEAM_SLEEP:                return ("sleep");
    case BEAM_BERSERK:              return ("berserk");
    case BEAM_POTION_BLACK_SMOKE:   return ("black smoke");
    case BEAM_POTION_GREY_SMOKE:    return ("grey smoke");
    case BEAM_POTION_BLUE_SMOKE:    return ("blue smoke");
    case BEAM_POTION_PURPLE_SMOKE:  return ("purple smoke");
    case BEAM_POTION_RAIN:          return ("rain");
    case BEAM_POTION_RANDOM:        return ("random potion");
    case BEAM_POTION_MUTAGENIC:     return ("mutagenic fog");
    case BEAM_VISUAL:               return ("visual effects");
    case BEAM_TORMENT_DAMAGE:       return ("torment damage");
    case BEAM_STEAL_FOOD:           return ("steal food");

    case NUM_BEAMS:                 DEBUGSTR("invalid beam type");
                                    return ("INVALID");
    }
    DEBUGSTR("unknown beam type");
    return("UNKNOWN");
}

void clear_zap_info_on_exit()
{
    const unsigned int zap_size = sizeof(zap_data) / sizeof(zap_info);
    for (unsigned int i = 0; i < zap_size; ++i)
    {
        delete zap_data[i].damage;
        delete zap_data[i].tohit;
    }
}
