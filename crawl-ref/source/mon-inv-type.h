#pragma once

// Adding slots breaks saves. YHBW.
enum mon_inv_type           // menv[].inv[]
{
    MSLOT_WEAPON,           // Primary weapon (melee)
    MSLOT_ALT_WEAPON,       // Alternate weapon, ranged or second melee weapon
                            // for monsters that can use two weapons.
    MSLOT_MISSILE,
    MSLOT_ALT_MISSILE,
    MSLOT_ARMOUR,
    MSLOT_SHIELD,
    MSLOT_WAND,
    MSLOT_JEWELLERY,
    MSLOT_MISCELLANY,

    // [ds] Last monster gear slot that the player can observe by examining
    // the monster; i.e. the last slot that goes into monster_info.
    MSLOT_LAST_VISIBLE_SLOT = MSLOT_MISCELLANY,

    MSLOT_POTION,
    MSLOT_SCROLL,
    MSLOT_GOLD,
    NUM_MONSTER_SLOTS
};