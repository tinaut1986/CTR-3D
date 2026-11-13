#ifndef DEBUG_ITEMS_H
#define DEBUG_ITEMS_H

/*
 * Debug item tools (CTR_INTERNAL, 3DS): give player 1 an item as if picked up
 * from a crate, or hit player 1 as if by a weapon. The Debug tab's ITEMS page
 * (platform/n3ds/n3ds_bottomui.c) and the dumpreq words `giveitem N` /
 * `hititem N` set one of these to an option index; the game thread applies it
 * on its next race frame and resets it to -1 (game/DebugItems.c). Plain ints:
 * the UI side is a libctru TU (short enums), the game side is the unity TU.
 * The option lists are in the same order on both sides.
 */

/* RANDOM (crate roulette), TURBO, BOMB, BOMB x3, MISSILE, MISSILE x3, TNT,
 * POTION, SHIELD, MASK, CLOCK, WARPBALL, INVISIBILITY, 10 WUMPA (juiced). */
#define DEBUG_GIVE_COUNT 14
/* BOMB, MISSILE, TNT, NITRO, GREEN POTION, RED POTION, CLOCK, WARPBALL,
 * SQUISH, BURN, SPIN. */
#define DEBUG_HIT_COUNT 11

extern int g_dbg_giveItem;
extern int g_dbg_hitItem;

#endif
