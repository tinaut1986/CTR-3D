// Debug item tools: give player 1 an item, or hit player 1 as if by a weapon.
// Driven by g_dbg_giveItem / g_dbg_hitItem (include/platform/debug_items.h),
// applied from MainFrame_GameLogic's driver loop on player 1's own turn, so
// only during a race and never while paused. Internal 3DS builds only.
//
// Each option repeats what the game itself does at that moment (crate pickup:
// RB_Crate.c, roulette end: VehPhysGeneral_SetHeldItem; hits: RB_GenericMine.c,
// RB_Burst.c, RB_Warpball.c, the clock in VehPickupItem_ShootNow), minus the
// weapon's own projectile, explosion and sound: the state change only.

#if defined(CTR_NATIVE) && defined(CTR_INTERNAL) && defined(__3DS__)

#include <platform/debug_items.h>

static const u8 s_debugGiveItems[DEBUG_GIVE_COUNT] = {
    HELD_ITEM_ROULETTE, HELD_ITEM_TURBO, HELD_ITEM_BOMB_1X, HELD_ITEM_BOMB_3X,  HELD_ITEM_MISSILE_1X,   HELD_ITEM_MISSILE_3X, HELD_ITEM_TNT,
    HELD_ITEM_POTION,   HELD_ITEM_SHIELD, HELD_ITEM_MASK,  HELD_ITEM_CLOCK,    HELD_ITEM_WARPBALL, HELD_ITEM_INVISIBILITY, HELD_ITEM_NONE /* 10 wumpa */,
};

static void DebugItems_Give(struct GameTracker *gGT, struct Driver *d, int opt)
{
	const u8 item = s_debugGiveItems[opt];

	if (opt == (DEBUG_GIVE_COUNT - 1))
	{
		// Ten wumpa: the next items come juiced.
		d->numWumpas = DRIVER_WUMPA_JUICED_COUNT;
		return;
	}

	if (item == HELD_ITEM_ROULETTE)
	{
		// As a crate does (RB_Crate.c): the roulette spins and picks.
		d->heldItemID = HELD_ITEM_ROULETTE;
		d->itemRollTimer = FPS_DOUBLE(90);
		if ((gGT->gameMode1 & ROLLING_ITEM) == 0)
		{
			OtherFX_Play(0x5d, 0);
			gGT->gameMode1 |= ROLLING_ITEM;
		}
		d->PickupTimeboxHUD.cooldown = FPS_DOUBLE(5);
		d->noItemTimer = 0;
		return;
	}

	// As the roulette's end does (VehPhysGeneral_SetHeldItem): the item, its
	// count for the three-stacks, the one-warpball flag, the "ding".
	d->heldItemID = item;
	d->numHeldItems = ((item == HELD_ITEM_BOMB_3X) || (item == HELD_ITEM_MISSILE_3X)) ? HELD_ITEM_STACK_COUNT : 0;
	d->itemRollTimer = 0;
	d->noItemTimer = 0;
	if (item == HELD_ITEM_WARPBALL)
	{
		gGT->gameMode1 |= WARPBALL_HELD;
	}
	OtherFX_Play(VEH_PHYS_PROC_ITEM_ROLL_NORMAL_FX, 0);
}

// Potion hits flash the victim's view white (RB_GenericMine.c).
static void DebugItems_PotionFlash(struct GameTracker *gGT, struct Driver *d)
{
	gGT->pushBuffer[d->driverID].fadeFromBlack_currentValue = 0x1fff;
	gGT->pushBuffer[d->driverID].fadeFromBlack_desiredResult = 0x1000;
	gGT->pushBuffer[d->driverID].fade_step = 0xff78;
}

static void DebugItems_Hit(struct GameTracker *gGT, struct Driver *d, int opt)
{
	switch (opt)
	{
	case 0: // bomb
		RB_Hazard_HurtDriver(d, VEH_PICK_DAMAGE_BLAST, 0, VEH_PICK_REASON_BOMB);
		d->damageColorTimer = 0x1e;
		break;
	case 1: // missile
		RB_Hazard_HurtDriver(d, VEH_PICK_DAMAGE_BLAST, 0, VEH_PICK_REASON_MISSILE);
		d->damageColorTimer = 0x1e;
		break;
	case 2: // TNT exploding
		RB_Hazard_HurtDriver(d, VEH_PICK_DAMAGE_BLAST, 0, VEH_PICK_REASON_MOTIONLESS_POTION);
		d->damageColorTimer = 0x1e;
		break;
	case 3: // nitro
		RB_Hazard_HurtDriver(d, VEH_PICK_DAMAGE_BLAST, 0, VEH_PICK_REASON_MOTIONLESS_POTION);
		d->damageColorTimer = -0x1e;
		break;
	case 4: // green potion
		RB_Hazard_HurtDriver(d, VEH_PICK_DAMAGE_SPIN, 0, VEH_PICK_REASON_MOTIONLESS_POTION);
		DebugItems_PotionFlash(gGT, d);
		d->damageColorTimer = -0x1e;
		break;
	case 5: // red potion: the rain cloud too
		if (RB_Hazard_HurtDriver(d, VEH_PICK_DAMAGE_SPIN, 0, VEH_PICK_REASON_MOTIONLESS_POTION) != 0)
		{
			RB_RainCloud_Init(d);
		}
		DebugItems_PotionFlash(gGT, d);
		d->damageColorTimer = 0x1e;
		break;
	case 6: // clock (VehPickupItem_ShootNow): flash, spin, the slow-down
		d->clockFlash = CLOCK_FLASH_FRAMES;
		if (RB_Hazard_HurtDriver(d, CLOCK_HURT_REASON, 0, 0) != 0)
		{
			d->clockReceive = CLOCK_HURT_DURATION_NORMAL;
		}
		break;
	case 7: // warpball
		RB_Hazard_HurtDriver(d, VEH_PICK_DAMAGE_BLAST, 0, 0);
		break;
	case 8: // squished
		RB_Hazard_HurtDriver(d, VEH_PICK_DAMAGE_SQUISH, 0, 0);
		break;
	case 9: // burned
		RB_Hazard_HurtDriver(d, VEH_PICK_DAMAGE_BURN, 0, 0);
		break;
	default: // plain spin-out
		RB_Hazard_HurtDriver(d, VEH_PICK_DAMAGE_SPIN, 0, 0);
		break;
	}
}

void DebugItems_Apply(struct GameTracker *gGT, struct Driver *d)
{
	const int give = g_dbg_giveItem;
	const int hit = g_dbg_hitItem;

	// Outside a race the player bucket can still hold driver objects (the
	// menus' karts, a race being torn down): a hit there leaves a spin state
	// running on an object that is not a live racer and corrupted memory on
	// the next menu load. Drop the request instead of keeping it pending.
	if ((gGT->gameMode1 & (MAIN_MENU | GAME_CUTSCENE | LOADING | END_OF_RACE)) != 0)
	{
		if ((give >= 0) || (hit >= 0))
		{
			g_dbg_giveItem = -1;
			g_dbg_hitItem = -1;
		}
		return;
	}
	if (d != gGT->drivers[0])
	{
		return;
	}
	if (give >= 0)
	{
		g_dbg_giveItem = -1;
		if (give < DEBUG_GIVE_COUNT)
		{
			DebugItems_Give(gGT, d, give);
		}
	}
	if (hit >= 0)
	{
		g_dbg_hitItem = -1;
		if (hit < DEBUG_HIT_COUNT)
		{
			DebugItems_Hit(gGT, d, hit);
		}
	}
}

#endif
