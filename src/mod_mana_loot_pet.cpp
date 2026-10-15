/*
 * mod-mana-loot-pet: the player's vanity companion walks to a fresh corpse and loots it.
 *
 * A creature killed solo by a player is looted by that player's non-combat companion
 * (UNIT_FIELD_CRITTER / SUMMON_SLOT_MINIPET). The kill credit starts a fetch: the pet is sent
 * to a point beside the corpse, and the loot only moves once it is actually there, so the
 * behaviour is visible rather than instantaneous. Money and items go straight to the player;
 * an item that no longer fits is mailed, exactly as a looted container's overflow is.
 *
 * Only a sole recipient is served: a creature whose loot recipient is a group (or someone
 * else) is left alone, so a group's loot rules and rolls are never bypassed. One fetch per
 * player at a time -- a second kill while the pet is away stays manually lootable.
 *
 * shortcut: leader-only fetches and quest-item-only fetches, upgrade if crowded play wants them.
 */

#include "Config.h"
#include "Creature.h"
#include "Item.h"
#include "LootMgr.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "PetDefines.h"
#include "Player.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "Timer.h"
#include "WorldPacket.h"

#include <numbers>
#include <unordered_map>

namespace
{
    struct Config
    {
        bool   enabled      = true;
        float  lootDistance = 3.0f;  // how close the pet must get before the corpse is looted
        uint32 reachMs      = 8000;  // a fetch older than this is abandoned (pet stuck or corpse gone)
        bool   money        = true;
        bool   items        = true;
        bool   mailOverflow = true;
        uint32 maxItems     = 20;    // most slots taken from one corpse in a single fetch
    };

    Config g_config;

    // One entry per player: the fetch the companion is on, if any. The pet's own GUID is kept
    // because a companion summoned or dismissed mid-fetch makes the creature pointer stale, and the
    // corpse GUID is kept so a despawned object is simply not found again.
    struct Fetch
    {
        ObjectGuid corpse;
        ObjectGuid pet;
        uint32     startMs = 0;
    };

    std::unordered_map<ObjectGuid, Fetch> g_fetch;

    // Refresh the clock once per world tick so N players cost one call rather than one each.
    uint32 g_nowMs = 0;

    // The companion the player currently owns, or nullptr. A pet summoned or dismissed mid-fetch
    // makes the GUID stale, which the update below simply drops.
    Creature* GetCompanion(Player* player)
    {
        return player ? player->GetCompanionPet() : nullptr;
    }

    // A fresh corpse is worth fetching only if this player is its sole recipient: a group
    // recipient, or another player, keeps its own loot rules and rolls.
    bool CanFetch(Player* player, Creature* killed)
    {
        if (!g_config.enabled || !player || !killed || !killed->IsInWorld())
            return false;
        if (killed->GetLootRecipientGUID() != player->GetGUID() || killed->GetLootRecipientGroup())
            return false;
        if (killed->loot.isLooted())
            return false;
        // The corpse must be lootable by hand right now: the dynamic flag is exactly what the
        // client's loot cursor keys off, and the core clears it for a kill that may not be looted
        // (no reward, insufficient damage).
        if (!killed->HasDynamicFlag(UNIT_DYNFLAG_LOOTABLE))
            return false;
            return false;
        // Never take skinning or pickpocketing loot: those are separate windows on the same corpse.
        if (killed->loot.loot_type != LOOT_NONE && killed->loot.loot_type != LOOT_CORPSE)
            return false;
        // A player under play-time restriction must not receive loot; the core's own handlers
        // refuse it, so an automatic path must not quietly work around that.
        if (player->HasPlayerFlag(PLAYER_FLAGS_NO_PLAY_TIME) ||
            player->HasPlayerFlag(PLAYER_FLAGS_PARTIAL_PLAY_TIME))
            return false;
        return GetCompanion(player) != nullptr;
    }

    // The companion idles on a follow generator; sending it to a point replaces that generator in
    // the active slot, so the follow just has to be put back the way the summon path built it.
    void ResumeFollow(Creature* pet, Player* player)
    {
        if (pet && player)
            pet->GetMotionMaster()->MoveFollow(player, PET_FOLLOW_DIST, MINI_PET_FOLLOW_ANGLE,
                                               MOTION_SLOT_ACTIVE);
    }

    // Walk the corpse's loot into the player. Mirrors the core's own single-player loot path
    // (HandleAutostoreLootItemOpcode / HandleLootMoneyOpcode), minus the loot window: no window is
    // ever sent, so the client never has to be told the corpse was looted.
    void LootCorpse(Player* player, Creature* corpse)
    {
        Loot* loot = &corpse->loot;

        // Per-player FFA / quest / conditional caches, normally built by Player::SendLoot. Idempotent.
        loot->FillNotNormalLootFor(player);

        if (g_config.money && loot->gold)
        {
            uint32 const gold = loot->gold;
            loot->gold = 0;

            player->ModifyMoney(int32(gold));
            player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_LOOT_MONEY, gold);

            WorldPacket data(SMSG_LOOT_MONEY_NOTIFY, 4 + 1);
            data << uint32(gold);
            data << uint8(1);   // "You loot..."
            player->SendDirectMessage(&data);
        }

        if (g_config.items)
        {
            uint32 const maxSlot = loot->GetMaxSlotInLootFor(player);
            uint32       taken   = 0;

            for (uint32 slot = 0; slot < maxSlot && taken < g_config.maxItems; ++slot)
            {
                LootItem* item = loot->LootItemInSlot(slot, player);
                if (!item || item->is_looted)
                    continue;

                InventoryResult msg = EQUIP_ERR_OK;
                player->StoreLootItem(uint8(slot), loot, msg);
                if (msg == EQUIP_ERR_OK)
                {
                    ++taken;
                    continue;
                }

                // Not storable (bags full, unique already owned). Mail it, the way a looted
                // container's overflow is mailed, so it is not destroyed and the corpse can clear.
                // The separate quest_items list is left alone: its per-player slot bookkeeping is
                // not worth undoing by hand -- the player hand-loots those.
                if (g_config.mailOverflow && slot < loot->items.size())
                {
                    player->SendItemRetrievalMail(item->itemid, item->count);
                    item->is_looted = true;
                    loot->NotifyItemRemoved(item->itemIndex);
                    --loot->unlootedCount;
                }
            }
        }

        // A corpse with nothing left behaves exactly like one looted by hand: lootable flagged off,
        // skinnable set, decay timer shortened.
        if (loot->isLooted())
        {
            corpse->RemoveDynamicFlag(UNIT_DYNFLAG_LOOTABLE);
            corpse->ForceValuesUpdateAtIndex(UNIT_DYNAMIC_FLAGS);
            corpse->AllLootRemovedFromCorpse();
            loot->clear();
        }
    }

    // The world tick keeps the clock; the kill-credit hook starts a fetch; the player's own update
    // drives it. The pet can only ever fetch for the player who killed the creature, so no player's
    // update touches another player's fetch.
    class ManaLootPetScript final : public WorldScript, public PlayerScript
    {
    public:
        ManaLootPetScript()
            : WorldScript("ManaLootPetWorld", { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_UPDATE }),
              PlayerScript("ManaLootPetPlayer", { PLAYERHOOK_ON_CREATURE_KILL_CREDIT, PLAYERHOOK_ON_UPDATE,
                                                  PLAYERHOOK_ON_LOGOUT }) {}

        void OnUpdate(uint32 /*diff*/) override
        {
            g_nowMs = getMSTime();
        }

        void OnAfterConfigLoad(bool /*reload*/) override
        {
            g_config.enabled      = sConfigMgr->GetOption<bool>("ManaLootPet.Enable", true);
            g_config.lootDistance = sConfigMgr->GetOption<float>("ManaLootPet.LootDistance", 3.0f);
            g_config.reachMs      = sConfigMgr->GetOption<uint32>("ManaLootPet.ReachTimeoutMs", 8000);
            g_config.money        = sConfigMgr->GetOption<bool>("ManaLootPet.LootMoney", true);
            g_config.items        = sConfigMgr->GetOption<bool>("ManaLootPet.LootItems", true);
            g_config.mailOverflow = sConfigMgr->GetOption<bool>("ManaLootPet.MailWhenBagsFull", true);
            g_config.maxItems     = sConfigMgr->GetOption<uint32>("ManaLootPet.MaxItemsPerCorpse", 20);

            // A pet that has to touch the corpse needs a real distance; one far below it would just
            // loot the moment the kill lands. The timeout must at least outlast a walk across range.
            if (g_config.lootDistance < 0.5f)
                g_config.lootDistance = 0.5f;
            if (g_config.reachMs < 1000)
                g_config.reachMs = 1000;
            if (!g_config.maxItems)
                g_config.maxItems = 1;
        }

        // The kill credit is raised once per creature, after the corpse's loot is generated and the
        // lootable flag is set, so this is the earliest the corpse is really lootable.
        void OnPlayerCreatureKillCredit(Player* player, Creature* killed) override
        {
            if (!CanFetch(player, killed))
                return;
            if (g_fetch.count(player->GetGUID()))
                return;   // one corpse at a time: a kill during a fetch stays manually lootable

            Creature* pet = GetCompanion(player);
            Position  dest = killed->GetNearPosition(frand(0.5f, g_config.lootDistance * 0.6f),
                                                    frand(0.0f, 2.0f * std::numbers::pi_v<float>));
            pet->GetMotionMaster()->MovePoint(0, dest);

            Fetch& fetch = g_fetch[player->GetGUID()];
            fetch.corpse  = killed->GetGUID();
            fetch.pet     = pet->GetGUID();
            fetch.startMs = g_nowMs;
        }

        void OnPlayerUpdate(Player* player, uint32 /*p_time*/) override
        {
            if (!g_config.enabled || !player)
                return;

            auto itr = g_fetch.find(player->GetGUID());
            if (itr == g_fetch.end())
                return;

            Fetch const& fetch = itr->second;
            Creature*    pet   = ObjectAccessor::GetCreature(*player, fetch.pet);
            Creature*    corpse = player->GetMap()->GetCreature(fetch.corpse);

            // The pet vanished or died, the corpse despawned or was looted by hand, or the walk is
            // taking too long (stuck, unreachable). Whatever the reason, the fetch is over.
            if (!pet || !pet->IsAlive() || !corpse || corpse->loot.isLooted() ||
                g_nowMs - fetch.startMs > g_config.reachMs)
            {
                ResumeFollow(pet, player);
                g_fetch.erase(itr);
                return;
            }

            if (!pet->IsWithinDistInMap(corpse, g_config.lootDistance))
                return;

            LootCorpse(player, corpse);
            ResumeFollow(pet, player);
            g_fetch.erase(itr);
        }

        void OnPlayerLogout(Player* player) override
        {
            if (player)
                g_fetch.erase(player->GetGUID());
        }
    };
}

void AddSC_ManaLootPet()
{
    new ManaLootPetScript();
}