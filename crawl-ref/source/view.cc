/*
 *  File:       view.cc
 *  Summary:    Misc function used to render the dungeon.
 *  Written by: Linley Henzell
 */

#include "AppHdr.h"

#include "view.h"
#include "shout.h"

#include <stdint.h>
#include <string.h>
#include <cmath>
#include <sstream>
#include <algorithm>
#include <memory>

#ifdef TARGET_OS_DOS
#include <conio.h>
#endif

#include "externs.h"

#include "map_knowledge.h"
#include "viewchar.h"
#include "viewgeom.h"
#include "viewmap.h"
#include "showsymb.h"

#include "attitude-change.h"
#include "branch.h"
#include "cio.h"
#include "cloud.h"
#include "clua.h"
#include "colour.h"
#include "database.h"
#include "debug.h"
#include "delay.h"
#include "directn.h"
#include "dungeon.h"
#include "exclude.h"
#include "feature.h"
#include "files.h"
#include "godabil.h"
#include "itemprop.h"
#include "los.h"
#include "macro.h"
#include "message.h"
#include "misc.h"
#include "mon-behv.h"
#include "mon-iter.h"
#include "mon-place.h"
#include "mon-stuff.h"
#include "mon-util.h"
#include "newgame.h"
#include "options.h"
#include "notes.h"
#include "output.h"
#include "overmap.h"
#include "player.h"
#include "random.h"
#include "stuff.h"
#include "spells3.h"
#include "stash.h"
#include "tiles.h"
#include "travel.h"
#include "state.h"
#include "terrain.h"
#include "tilemcache.h"
#include "tilesdl.h"
#include "travel.h"
#include "tutorial.h"
#include "xom.h"

#define DEBUG_PANE_BOUNDS 0

crawl_view_geometry crawl_view;

bool is_notable_terrain(dungeon_feature_type ftype)
{
    return (get_feature_def(ftype).is_notable());
}

void handle_seen_interrupt(monsters* monster)
{
    if (mons_is_unknown_mimic(monster))
        return;

    activity_interrupt_data aid(monster);
    if (!monster->seen_context.empty())
        aid.context = monster->seen_context;
    else if (testbits(monster->flags, MF_WAS_IN_VIEW))
        aid.context = "already seen";
    else
        aid.context = "newly seen";

    if (!mons_is_safe(monster)
        && !mons_class_flag(monster->type, M_NO_EXP_GAIN))
    {
        interrupt_activity(AI_SEE_MONSTER, aid);
    }
    seen_monster( monster );
}

void flush_comes_into_view()
{
    if (!you.turn_is_over
        || (!you_are_delayed() && !crawl_state.is_repeating_cmd()))
    {
        return;
    }

    monsters* mon = crawl_state.which_mon_acting();

    if (!mon || !mon->alive() || (mon->flags & MF_WAS_IN_VIEW)
        || !you.can_see(mon))
    {
        return;
    }

    handle_seen_interrupt(mon);
}

void monster_grid_updates()
{
    for (monster_iterator mi(&you.get_los()); mi; ++mi)
    {
        if ((mi->asleep() || mons_is_wandering(*mi))
            && check_awaken(*mi))
        {
            behaviour_event(*mi, ME_ALERT, MHITYOU, you.pos(), false);
            handle_monster_shouts(*mi);
        }

        fedhas_neutralise(*mi);
        if (!mi->visible_to(&you))
            continue;

        good_god_follower_attitude_change(*mi);
        beogh_follower_convert(*mi);
        slime_convert(*mi);
        // XXX: Probably quite hackish. Allows for monsters going berserk when
        //      they see the player. Currently only used for Duvessa, see the
        //      function _elven_twin_dies in mon-stuff.cc.
        if (mi->flags & MF_GOING_BERSERK)
        {
            mi->flags &= ~MF_GOING_BERSERK;
            mi->go_berserk(true);
        }
    }
}

static void _check_monster_pos(const monsters* monster)
{
    int s = monster->mindex();
    ASSERT(mgrd(monster->pos()) == s);

    // [rob] The following in case asserts aren't enabled.
    // [enne] - It's possible that mgrd and monster->x/y are out of
    // sync because they are updated separately.  If we can see this
    // monster, then make sure that the mgrd is set correctly.
    if (mgrd(monster->pos()) != s)
    {
        // If this mprf triggers for you, please note any special
        // circumstances so we can track down where this is coming
        // from.
        mprf(MSGCH_ERROR, "monster %s (%d) at (%d, %d) was "
             "improperly placed.  Updating mgrd.",
             monster->name(DESC_PLAIN, true).c_str(), s,
             monster->pos().x, monster->pos().y);
        ASSERT(!monster_at(monster->pos()));
        mgrd(monster->pos()) = s;
    }
}

void monster_grid()
{
    for (monster_iterator mi(&you.get_los()); mi; ++mi)
    {
        _check_monster_pos(*mi);
        env.show.update_monster(*mi);

#ifdef USE_TILE
        if (mi->visible_to(&you))
            tile_place_monster(mi->pos().x, mi->pos().y, mi->mindex(), true);
#endif
    }
}

void update_monsters_in_view()
{
    unsigned int num_hostile = 0;

    for (monster_iterator mi; mi; ++mi)
    {
        if (mons_near(*mi))
        {
            if (mi->attitude == ATT_HOSTILE)
                num_hostile++;

            if (mons_is_unknown_mimic(*mi))
            {
                // For unknown mimics, don't mark as seen,
                // but do mark it as in view for later messaging.
                // FIXME: is this correct?
                mi->flags |= MF_WAS_IN_VIEW;
            }
            else if (mi->visible_to(&you))
            {
                handle_seen_interrupt(*mi);
                seen_monster(*mi);
            }
            else
                mi->flags &= ~MF_WAS_IN_VIEW;
        }
        else
            mi->flags &= ~MF_WAS_IN_VIEW;

        // If the monster hasn't been seen by the time that the player
        // gets control back then seen_context is out of date.
        mi->seen_context.clear();
    }

    // Xom thinks it's hilarious the way the player picks up an ever
    // growing entourage of monsters while running through the Abyss.
    // To approximate this, if the number of hostile monsters in view
    // is greater than it ever was for this particular trip to the
    // Abyss, Xom is stimulated in proportion to the number of
    // hostile monsters.  Thus if the entourage doesn't grow, then
    // Xom becomes bored.
    if (you.level_type == LEVEL_ABYSS
        && you.attribute[ATTR_ABYSS_ENTOURAGE] < num_hostile)
    {
        you.attribute[ATTR_ABYSS_ENTOURAGE] = num_hostile;
        xom_is_stimulated(16 * num_hostile);
    }
}

// We logically associate a difficulty parameter with each tile on each level,
// to make deterministic magic mapping work.  This function returns the
// difficulty parameters for each tile on the current level, whose difficulty
// is less than a certain amount.
//
// Random difficulties are used in the few cases where we want repeated maps
// to give different results; scrolls and cards, since they are a finite
// resource.
static const FixedArray<char, GXM, GYM>& _tile_difficulties(bool random)
{
    // We will often be called with the same level parameter and cutoff, so
    // cache this (DS with passive mapping autoexploring could be 5000 calls
    // in a second or so).
    static FixedArray<char, GXM, GYM> cache;
    static int cache_seed = -1;

    int seed = random ? -1 :
        (static_cast<int>(you.where_are_you) << 8) + you.your_level - 1731813538;

    if (seed == cache_seed && !random)
    {
        return cache;
    }

    if (!random)
    {
        push_rng_state();
        seed_rng(cache_seed);
    }

    cache_seed = seed;

    for (int y = Y_BOUND_1; y <= Y_BOUND_2; ++y)
        for (int x = X_BOUND_1; x <= X_BOUND_2; ++x)
            cache[x][y] = random2(100);

    if (!random)
    {
        pop_rng_state();
    }

    return cache;
}

static std::auto_ptr<FixedArray<bool, GXM, GYM> > _tile_detectability()
{
    std::auto_ptr<FixedArray<bool, GXM, GYM> > map(new FixedArray<bool, GXM, GYM>);

    std::vector<coord_def> flood_from;

    for (int x = X_BOUND_1; x <= X_BOUND_2; ++x)
        for (int y = Y_BOUND_1; y <= Y_BOUND_2; ++y)
        {
            (*map)(coord_def(x,y)) = false;

            if (feat_is_stair(grd[x][y]))
            {
                flood_from.push_back(coord_def(x, y));
            }
        }

    flood_from.push_back(you.pos());

    while (!flood_from.empty())
    {
        coord_def p = flood_from.back();
        flood_from.pop_back();

        if ((*map)(p))
            continue;

        (*map)(p) = true;

        if (grd(p) < DNGN_MINSEE && grd(p) != DNGN_CLOSED_DOOR)
            continue;

        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx)
                flood_from.push_back(p + coord_def(dx,dy));
    }

    return map;
}

// Returns true if it succeeded.
bool magic_mapping(int map_radius, int proportion, bool suppress_msg,
                   bool force, bool deterministic, bool circular,
                   coord_def pos)
{
    if (!in_bounds(pos))
        pos = you.pos();

    if (!force
        && (testbits(env.level_flags, LFLAG_NO_MAGIC_MAP)
            || testbits(get_branch_flags(), BFLAG_NO_MAGIC_MAP)))
    {
        if (!suppress_msg)
            mpr("You feel momentarily disoriented.");

        return (false);
    }

    const bool wizard_map = (you.wizard && map_radius == 1000);

    if (!wizard_map)
    {
        if (map_radius > 50)
            map_radius = 50;
        else if (map_radius < 5)
            map_radius = 5;
    }

    // now gradually weaker with distance:
    const int pfar     = (map_radius * 7) / 10;
    const int very_far = (map_radius * 9) / 10;

    bool did_map = false;
    int  num_altars        = 0;
    int  num_shops_portals = 0;

    const FixedArray<char, GXM, GYM>& difficulty =
        _tile_difficulties(!deterministic);

    std::auto_ptr<FixedArray<bool, GXM, GYM> > detectable;

    if (!deterministic)
        detectable = _tile_detectability();

    for (radius_iterator ri(pos, map_radius, !circular, false); ri; ++ri)
    {
        if (!wizard_map)
        {
            int threshold = proportion;

            const int dist = grid_distance( you.pos(), *ri );

            if (dist > very_far)
                threshold = threshold / 3;
            else if (dist > pfar)
                threshold = threshold * 2 / 3;

            if (difficulty(*ri) > threshold)
                continue;
        }

        if (is_terrain_changed(*ri))
            clear_map_knowledge_grid(*ri);

        if (!wizard_map && (is_terrain_seen(*ri) || is_terrain_mapped(*ri)))
            continue;

        if (!wizard_map && !deterministic && !((*detectable)(*ri)))
            continue;

        const dungeon_feature_type feat = grd(*ri);

        bool open = true;

        if (feat_is_solid(feat) && !feat_is_closed_door(feat))
        {
            open = false;
            for (adjacent_iterator ai(*ri); ai; ++ai)
            {
                if (map_bounds(*ai) && (!feat_is_opaque(grd(*ai))
                                        || feat_is_closed_door(grd(*ai))))
                {
                    open = true;
                    break;
                }
            }
        }

        if (open)
        {
            if (wizard_map || !get_map_knowledge_obj(*ri))
                set_map_knowledge_obj(*ri, grd(*ri));

            if (wizard_map)
            {
                if (is_notable_terrain(feat))
                    seen_notable_thing(feat, *ri);

                set_terrain_seen(*ri);
#ifdef USE_TILE
                // Can't use set_map_knowledge_obj because that would
                // overwrite the gmap.
                env.tile_bk_bg(*ri) = tile_idx_unseen_terrain(ri->x, ri->y,
                                                              grd(*ri));
#endif
            }
            else
            {
                set_terrain_mapped(*ri);

                if (get_feature_dchar(feat) == DCHAR_ALTAR)
                    num_altars++;
                else if (get_feature_dchar(feat) == DCHAR_ARCH)
                    num_shops_portals++;
            }

            did_map = true;
        }
    }

    if (!suppress_msg)
    {
        mpr(did_map ? "You feel aware of your surroundings."
                    : "You feel momentarily disoriented.");

        std::vector<std::string> sensed;

        if (num_altars > 0)
            sensed.push_back(make_stringf("%d altar%s", num_altars,
                                          num_altars > 1 ? "s" : ""));

        if (num_shops_portals > 0)
        {
            const char* plur = num_shops_portals > 1 ? "s" : "";
            sensed.push_back(make_stringf("%d shop%s/portal%s",
                                          num_shops_portals, plur, plur));
        }

        if (!sensed.empty())
            mpr_comma_separated_list("You sensed ", sensed);
    }

    return (did_map);
}

// Is the given monster near (in LOS of) the player?
bool mons_near(const monsters *monster)
{
    if (crawl_state.arena || crawl_state.arena_suspended)
        return (true);
    return (you.see_cell(monster->pos()));
}

bool mon_enemies_around(const monsters *monster)
{
    // If the monster has a foe, return true.
    if (monster->foe != MHITNOT && monster->foe != MHITYOU)
        return (true);

    if (crawl_state.arena)
    {
        // If the arena-mode code in _handle_behaviour() hasn't set a foe then
        // we don't have one.
        return (false);
    }
    else if (monster->wont_attack())
    {
        // Additionally, if an ally is nearby and *you* have a foe,
        // consider it as the ally's enemy too.
        return (mons_near(monster) && there_are_monsters_nearby(true));
    }
    else
    {
        // For hostile monsters *you* are the main enemy.
        return (mons_near(monster));
    }
}

// Returns a string containing an ASCII representation of the map. If fullscreen
// is set to false, only the viewable area is returned. Leading and trailing
// spaces are trimmed from each line. Leading and trailing empty lines are also
// snipped.
std::string screenshot(bool fullscreen)
{
    UNUSED(fullscreen);

    // [ds] Screenshots need to be straight ASCII. We will now proceed to force
    // the char and feature tables back to ASCII.
    FixedVector<unsigned, NUM_DCHAR_TYPES> char_table_bk;
    char_table_bk = Options.char_table;

    init_char_table(CSET_ASCII);
    init_show_table();

    int firstnonspace = -1;
    int firstpopline  = -1;
    int lastpopline   = -1;

    std::vector<std::string> lines(crawl_view.viewsz.y);
    for (int count_y = 1; count_y <= crawl_view.viewsz.y; count_y++)
    {
        int lastnonspace = -1;

        for (int count_x = 1; count_x <= crawl_view.viewsz.x; count_x++)
        {
            // in grid coords
            const coord_def gc = view2grid(coord_def(count_x, count_y));

            int ch =
                  (!map_bounds(gc))             ? 0 :
                  (!crawl_view.in_grid_los(gc)) ? get_map_knowledge_char(gc.x, gc.y) :
                  (gc == you.pos())             ? you.symbol
                                                : get_screen_glyph(gc.x, gc.y);

            if (ch && !isprint(ch))
            {
                // [ds] Evil hack time again. Peek at grid, use that character.
                show_type object = show_type(grid_appearance(gc));
                unsigned glych;
                unsigned short glycol = 0;

                get_symbol( gc, object, &glych, &glycol );
                ch = glych;
            }

            // More mangling to accommodate C strings.
            if (!ch)
                ch = ' ';

            if (ch != ' ')
            {
                lastnonspace = count_x;
                lastpopline = count_y;

                if (firstnonspace == -1 || firstnonspace > count_x)
                    firstnonspace = count_x;

                if (firstpopline == -1)
                    firstpopline = count_y;
            }

            lines[count_y - 1] += ch;
        }

        if (lastnonspace < (int) lines[count_y - 1].length())
            lines[count_y - 1].erase(lastnonspace + 1);
    }

    // Restore char and feature tables.
    Options.char_table = char_table_bk;
    init_show_table();

    std::ostringstream ss;
    if (firstpopline != -1 && lastpopline != -1)
    {
        if (firstnonspace == -1)
            firstnonspace = 0;

        for (int i = firstpopline; i <= lastpopline; ++i)
        {
            const std::string &ref = lines[i - 1];
            if (firstnonspace < (int) ref.length())
                ss << ref.substr(firstnonspace);
            ss << EOL;
        }
    }

    return (ss.str());
}

static int _viewmap_flash_colour()
{
    if (you.attribute[ATTR_SHADOWS])
        return (DARKGREY);
    else if (you.berserk())
        return (RED);

    return (BLACK);
}

// Updates one square of the view area. Should only be called for square
// in LOS.
void view_update_at(const coord_def &pos)
{
    if (pos == you.pos())
        return;

    const coord_def vp = grid2view(pos);
    const coord_def ep = view2show(vp);
    env.show.update_at(pos, ep);

    show_type object = env.show(ep);

    if (!object)
        return;

    unsigned short  colour = object.colour;
    unsigned        ch = 0;

    get_symbol( pos, object, &ch, &colour );

    int flash_colour = you.flash_colour;
    if (flash_colour == BLACK)
        flash_colour = _viewmap_flash_colour();

#ifndef USE_TILE
    cgotoxy(vp.x, vp.y);
    put_colour_ch(flash_colour? real_colour(flash_colour) : colour, ch);

    // Force colour back to normal, else clrscr() will flood screen
    // with this colour on DOS.
    textattr(LIGHTGREY);
#endif
}

#ifndef USE_TILE
void flash_monster_colour(const monsters *mon, unsigned char fmc_colour,
                          int fmc_delay)
{
    if (you.can_see(mon))
    {
        unsigned char old_flash_colour = you.flash_colour;
        coord_def c(mon->pos());

        you.flash_colour = fmc_colour;
        view_update_at(c);

        update_screen();
        delay(fmc_delay);

        you.flash_colour = old_flash_colour;
        view_update_at(c);
        update_screen();
    }
}
#endif

bool view_update()
{
    if (you.num_turns > you.last_view_update)
    {
        viewwindow(false);
        return (true);
    }
    return (false);
}

static void _debug_pane_bounds()
{
#if DEBUG_PANE_BOUNDS
    // Doesn't work for HUD because print_stats() overwrites it.

    if (crawl_view.mlistsz.y > 0)
    {
        textcolor(WHITE);
        cgotoxy(1,1, GOTO_MLIST);
        cprintf("+   L");
        cgotoxy(crawl_view.mlistsz.x-4, crawl_view.mlistsz.y, GOTO_MLIST);
        cprintf("L   +");
    }

    cgotoxy(1,1, GOTO_STAT);
    cprintf("+  H");
    cgotoxy(crawl_view.hudsz.x-3, crawl_view.hudsz.y, GOTO_STAT);
    cprintf("H  +");

    cgotoxy(1,1, GOTO_MSG);
    cprintf("+ M");
    cgotoxy(crawl_view.msgsz.x-2, crawl_view.msgsz.y, GOTO_MSG);
    cprintf("M +");

    cgotoxy(crawl_view.viewp.x, crawl_view.viewp.y);
    cprintf("+V");
    cgotoxy(crawl_view.viewp.x+crawl_view.viewsz.x-2,
            crawl_view.viewp.y+crawl_view.viewsz.y-1);
    cprintf("V+");
    textcolor(LIGHTGREY);
#endif
}

// Do various updates when the player sees a cell. Returns whether
// exclusion LOS might have been affected.
static bool player_view_update_at(const coord_def &gc)
{
    const coord_def ep = grid2show(gc);
    maybe_remove_autoexclusion(gc);

    // Set excludes in a radius of 1 around harmful clouds genereated
    // by neither monsters nor the player.
    const int cloudidx = env.cgrid(gc);
    if (cloudidx != EMPTY_CLOUD)
    {
        cloud_struct &cl   = env.cloud[cloudidx];
        cloud_type   ctype = cl.type;

        if (!is_harmless_cloud(ctype)
            && cl.whose  == KC_OTHER
            && cl.killer == KILL_MISC)
        {
            for (adjacent_iterator ai(gc, false); ai; ++ai)
            {
                if (!cell_is_solid(*ai) && !is_exclude_root(*ai))
                    set_exclude(*ai, 0);
            }
        }
    }

    // Print tutorial messages for features in LOS.
    if (Options.tutorial_left)
        tutorial_observe_cell(gc);

    if (!player_in_mappable_area())
        return (false);

    bool need_excl_update = is_terrain_changed(gc) || !is_terrain_seen(gc);

    show_type  object = env.show(ep);
    unsigned short colour = object.colour;
    unsigned        ch;
    get_symbol(gc, object, &ch, &colour);

    set_terrain_seen(gc);
    set_map_knowledge_glyph(gc, object, colour);
    set_map_knowledge_detected_mons(gc, false);
    set_map_knowledge_detected_item(gc, false);

#ifdef USE_TILE
    // We remove any references to mcache when
    // writing to the background.
    if (Options.clean_map)
        env.tile_bk_fg(gc) = get_clean_map_idx(env.tile_fg(ep));
    else
        env.tile_bk_fg(gc) = env.tile_fg(ep);
    env.tile_bk_bg(gc) = env.tile_bg(ep);
#endif

    if (Options.clean_map && env.show.get_backup(ep))
    {
        get_symbol(gc, env.show.get_backup(ep), &ch, &colour);
        set_map_knowledge_glyph(gc, env.show.get_backup(ep), colour);
    }

    return (need_excl_update);
}

static void player_view_update()
{
    std::vector<coord_def> update_excludes;
    for (radius_iterator ri(you.pos(), LOS_RADIUS); ri; ++ri)
        if (player_view_update_at(*ri))
            update_excludes.push_back(*ri);
    update_exclusion_los(update_excludes);
}

static void draw_unseen(screen_buffer_t* buffy, const coord_def &gc)
{
#ifndef USE_TILE
    buffy[0] = ' ';
    buffy[1] = DARKGREY;
#else
    tileidx_unseen(buffy[0], buffy[1], ' ', gc);
#endif
}

static void draw_outside_los(screen_buffer_t* buffy, const coord_def &gc)
{
#ifndef USE_TILE
    // Outside the env.show area.
    buffy[0] = get_map_knowledge_char(gc);
    buffy[1] = DARKGREY;

    if (Options.colour_map)
        buffy[1] = colour_code_map(gc, Options.item_colour);
#else USE_TILE
    unsigned int bg = env.tile_bk_bg(gc);
    unsigned int fg = env.tile_bk_fg(gc);
    if (bg == 0 && fg == 0)
        tileidx_unseen(fg, bg, get_map_knowledge_char(gc), gc);

    buffy[0] = fg;
    buffy[1] = bg | tile_unseen_flag(gc);
#endif
}

static void draw_player(screen_buffer_t* buffy,
                        const coord_def& gc, const coord_def& ep)
{
    if (crawl_state.arena || crawl_state.arena_suspended)
        return;
#ifndef USE_TILE
    // Player overrides everything in cell.
    buffy[0] = you.symbol;
    buffy[1] = you.colour;
    if (you.swimming())
    {
        if (grd(gc) == DNGN_DEEP_WATER)
            buffy[1] = BLUE;
        else
            buffy[1] = CYAN;
    }
#else
    if (player_in_mappable_area())
    {
        env.tile_bk_bg(gc) = env.tile_bg(ep);
        env.tile_bk_fg(gc) = env.tile_fg(ep);
    }

    buffy[0] = env.tile_fg(ep) = tileidx_player(you.char_class);
    buffy[1] = env.tile_bg(ep);
#endif
}

static void draw_los(screen_buffer_t* buffy,
                     const coord_def& gc, const coord_def& ep)
{
#ifndef USE_TILE
    show_type  object = env.show(ep);
    unsigned short colour = object.colour;
    unsigned        ch;
    get_symbol(gc, object, &ch, &colour);
    buffy[0] = ch;
    buffy[1] = colour;
#else
    buffy[0] = env.tile_fg(ep);
    buffy[1] = env.tile_bg(ep);
#endif
}

static void draw_los_backup(screen_buffer_t* buffy,
                            const coord_def& gc, const coord_def& ep)
{
#ifndef USE_TILE
    buffy[0] = get_map_knowledge_char(gc);
    buffy[1] = DARKGREY;

    if (Options.colour_map)
        buffy[1] = colour_code_map(gc, Options.item_colour);
#else
    if (env.tile_bk_fg(gc) != 0
        || env.tile_bk_bg(gc) != 0)
    {
        buffy[0] = env.tile_bk_fg(gc);
        buffy[1] = env.tile_bk_bg(gc) | tile_unseen_flag(gc);
    }
    else
        tileidx_unseen(buffy[0], buffy[1], get_map_knowledge_char(gc), gc);
#endif
}

//---------------------------------------------------------------
//
// Draws the main window using the character set returned
// by get_symbol().
//
// This function should not interfere with the game condition,
// unless do_updates is set (ie. stealth checks for visible
// monsters).
//
//---------------------------------------------------------------
void viewwindow(bool do_updates)
{
    if (you.duration[DUR_TIME_STEP])
        return;
    flush_prev_message();

    screen_buffer_t *buffy(crawl_view.vbuf);

    calc_show_los();

#ifdef USE_TILE
    tiles.clear_text_tags(TAG_NAMED_MONSTER);
    tile_draw_floor();
    mcache.clear_nonref();
#endif

    env.show.init();

    monster_grid();
    if (do_updates && !crawl_state.arena)
        monster_grid_updates();

    player_view_update(); // XXX: also conditional on do_updates?

#ifdef USE_TILE
    tile_draw_rays(true);
    tiles.clear_overlays();
#endif

    cursor_control cs(false);

    int flash_colour = you.flash_colour;
    if (flash_colour == BLACK)
        flash_colour = _viewmap_flash_colour();

    const coord_def &tl = crawl_view.viewp;
    const coord_def br = tl + crawl_view.viewsz - coord_def(1,1);
    int bufcount = 0;
    for (rectangle_iterator ri(tl, br); ri; ++ri, bufcount += 2)
    {
        // in grid coords
        const coord_def gc = view2grid(*ri);
        const coord_def ep = view2show(grid2view(gc));

        if (!map_bounds(gc))
            draw_unseen(&buffy[bufcount], gc);
        else if (!crawl_view.in_grid_los(gc))
            draw_outside_los(&buffy[bufcount], gc);
        else if (gc == you.pos())
            draw_player(&buffy[bufcount], gc, ep);
        else if (observe_cell(gc))
            draw_los(&buffy[bufcount], gc, ep);
        else
            draw_los_backup(&buffy[bufcount], gc, ep);

        // Alter colour if flashing the characters vision.
        if (flash_colour)
        {
#ifndef USE_TILE
            buffy[bufcount + 1] = observe_cell(gc) ? real_colour(flash_colour)
                                                   : DARKGREY;
#endif
        }
        else
        {
            bool out_of_range = Options.target_range > 0
                 && (grid_distance(you.pos(), gc) > Options.target_range);
#ifndef USE_TILE
            if (!observe_cell(gc) || out_of_range)
                buffy[bufcount + 1] = DARKGREY;
#else
            if (out_of_range)
                buffy[bufcount + 1] |= TILE_FLAG_OOR;
#endif
        }
    }

    // Leaving it this way because short flashes can occur in long ones,
    // and this simply works without requiring a stack.
    you.flash_colour = BLACK;

    // TODO: move this before the loop
    if ((!you.running || Options.travel_delay > -1
        || you.running.is_explore() && Options.explore_delay > -1)
        && !you.asleep())
    {
#ifndef USE_TILE
        you.last_view_update = you.num_turns;
        puttext(crawl_view.viewp.x, crawl_view.viewp.y,
                crawl_view.viewp.x + crawl_view.viewsz.x - 1,
                crawl_view.viewp.y + crawl_view.viewsz.y - 1,
                buffy);

        update_monster_pane();
#else
        if (!is_resting())
        {
            tiles.set_need_redraw();
            tiles.load_dungeon(&buffy[0], crawl_view.vgrdc);
            tiles.update_inventory();
        }
#endif
    }

    _debug_pane_bounds();
}

////////////////////////////////////////////////////////////////////////////
// Term resize handling (generic).

void handle_terminal_resize(bool redraw)
{
    crawl_state.terminal_resized = false;

    if (crawl_state.terminal_resize_handler)
        (*crawl_state.terminal_resize_handler)();
    else
        crawl_view.init_geometry();

    if (redraw)
        redraw_screen();
}
