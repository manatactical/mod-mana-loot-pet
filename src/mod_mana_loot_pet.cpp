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
 * else) is left alone, so a group's loot rules and rolls are never bypassed. Corpses are queued
 * per player and walked one at a time, so a pull that kills several creatures is chained.
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

#include <algorithm>
#include <mutex>
#include <numbers>
#include <unordered_map>

namespace
{
    struct Config
    {
        bool   enabled      = true;
        float  lootDistance = 1.5f;  // how close the pet must get before the corpse is looted
        uint32 reachMs      = 8000;  // a fetch older than this is abandoned (pet stuck or corpse gone)
        bool   money        = true;
        bool   items        = true;
        bool   mailOverflow = true;
        uint32 maxItems     = 20;    // most slots taken from one corpse in a single fetch
    };

    Config g_config;

    // One entry per player: the corpses the companion still owes a visit, in the order they dropped.
    // The pet's own GUID is kept because a companion summoned or dismissed mid-fetch makes the
    // creature pointer stale, and the corpse GUIDs are kept so a despawned object is simply not
    // found again. A queue rather than a single corpse is what lets a pull chain: the pet walks the
    // corpses one by one instead of the second kill staying on the ground.
    struct Fetch
    {
        std::vector<ObjectGuid> queue;
        ObjectGuid              pet;
        uint32                  startMs = 0;   // when the walk to queue.front() was (re)started

        // True once this leg of the walk has outlived ReachTimeoutMs. The timeout is per corpse: the
        // caller drops that corpse and gives the next one a fresh startMs.
        [[nodiscard]] bool done(uint32 nowMs) const
        {
            return !pet || nowMs - startMs > g_config.reachMs;
        }
    };

    // Every read or write of g_fetch, and of the clock the walks are timed against, holds
    // g_fetchMutex: the three hooks below can run on different map update threads (MapUpdate.Threads
    // above 1), one of them re-enters through a script hook, and one sends a packet.
    std::unordered_map<ObjectGuid, Fetch> g_fetch;
    std::recursive_mutex                  g_fetchMutex;

    // Refresh the clock once per world tick so N players cost one call rather than one each. A fresh
    // process starts at 1 so that "never set" cannot read as a fetch that started at zero.
    uint32 g_nowMs = 1;

    // The companion the player currently owns, or nullptr. A pet summoned or dismissed mid-fetch
    // makes the GUID stale, which the update below simply drops.
    Creature* GetCompanion(Player* player)
    {
        return player ? player->GetCompanionPet() : nullptr;
    }

    // An item is worth mailing only when the only thing standing in its way was room in the bags
    // (or a unique the player already owns). The core's other refusals -- blocked, won by another
    // roller, not allowed for this player -- must leave the item on the corpse for its rightful
    // owner, and they all come down to this test: AllowedForPlayer covers the allowedGUIDs set, the
    // per-player conditions, and hides a follow_loot_rules entry from LootItemInSlot entirely.
    bool readable(Player* player, LootItem const* item, ObjectGuid source)
    {
        return item->AllowedForPlayer(player, source) &&
               (item->rollWinnerGUID.IsEmpty() || item->rollWinnerGUID == player->GetGUID());
    }

    // True while the companion still has something of its own to take: the corpse's money, or a plain
    // item slot that is neither looted, conditional, nor outside what this player is allowed to read.
    // Money cannot be forgotten here -- most kills drop nothing but coin, and a corpse that is only
    // money would then be skipped, which reads as "the pet fetches one kill in three". A corpse whose
    // only leftovers are the quest tail or a conditional drop is done as far as the pet is concerned:
    // those are the player's to hand-loot. `is_blocked`, `is_underthreshold`, the eliminator's
    // `is_counted` and a loot id's value are deliberately not checked -- the first two are only ever
    // set for group loot, and the last two exist to give a player a reward the core then decides not
    // to (an eliminator, or an EvMap boss with no loot id). An item's own conditions are not read
    // either: a plain item with a condition the player passes is a normal drop by this point, and
    // skipping it is cheaper than re-evaluating what the core already evaluated.
    bool hasFetchableItem(Player* player, Loot* loot, ObjectGuid source)
    {
        if (g_config.money && loot->gold)
            return true;

        if (!g_config.items)
            return false;

        for (LootItem const& item : loot->items)
            if (!item.is_looted && item.conditions.empty() && readable(player, &item, source))
                return true;
        return false;
    }

    // A fresh corpse is worth fetching only if this player is its sole recipient: a group
    // recipient, or another player, keeps its own loot rules and rolls.
    bool canFetch(Player* player, Creature* killed)
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
        // Never take skinning or pickpocketing loot: those are separate windows on the same corpse.
        if (killed->loot.loot_type != LOOT_NONE && killed->loot.loot_type != LOOT_CORPSE)
            return false;
        // A player under play-time restriction must not receive loot; the core's own handlers
        // refuse it, so an automatic path must not quietly work around that.
        if (player->HasPlayerFlag(PLAYER_FLAGS_NO_PLAY_TIME) ||
            player->HasPlayerFlag(PLAYER_FLAGS_PARTIAL_PLAY_TIME))
            return false;
        if (!GetCompanion(player))
            return false;

        // Nothing the pet can help itself to is still no reason to walk over there: a corpse holding
        // only loot that is being left for the player would strand it in the queue, stop the pet from
        // following, and eat a ReachTimeoutMs before it moved on. This is the gate that decides which
        // kills are fetched at all, so it has to count money as well as items.
        killed->loot.FillNotNormalLootFor(player);
        return hasFetchableItem(player, &killed->loot, killed->loot.sourceWorldObjectGUID);
    }

    // Point the pet at a corpse and let it walk. frand asserts max >= min, and lootDistance is only
    // clamped to >= 0.5, so the spawn radius is the lower bound whenever a small LootDistance
    // (down to the documented 0.5) would otherwise put the upper bound below it.
    void SendToCorpse(Creature* pet, Creature* corpse)
    {
        if (!pet || !corpse)
            return;

        float const spread = std::max(0.5f, g_config.lootDistance * 0.6f);
        pet->GetMotionMaster()->MovePoint(0, corpse->GetNearPosition(
            frand(0.5f, spread), frand(0.0f, 2.0f * std::numbers::pi_v<float>)));
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
            // Mirrors HandleLootMoneyOpcode's copy: the report hooks take the amount that dropped.
            // There is no partial-play-time halving to do -- canFetch refuses a restricted player, and
            // OnPlayerUpdate unwinds a fetch the moment the flag goes up.
            sScriptMgr->OnPlayerBeforeLootMoney(player, loot);
            loot->NotifyMoneyRemoved();

            uint32 const dropped = loot->gold;
            uint32 const awarded = dropped;   // canFetch never serves a restricted player, so no halving

            sScriptMgr->OnPlayerAfterCreatureLootMoney(player);

            if (awarded)
            {
                player->ModifyMoney(int32(awarded));
                player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_LOOT_MONEY, awarded);

                WorldPacket data(SMSG_LOOT_MONEY_NOTIFY, 4 + 1);
                data << uint32(awarded);
                data << uint8(1);   // "You loot..."
                player->SendDirectMessage(&data);
            }

            sScriptMgr->OnLootMoney(player, dropped);
            loot->gold = 0;
        }

        if (g_config.items)
        {
            // Only the plain item list is walked: LootItemInSlot puts slot >= items.size() into the
            // quest_items tail, and a conditional drop keeps its QuestItem bookkeeping in a per-player
            // map -- both are left for the player to hand-loot, so a slot with conditions is skipped
            // the same way the quest tail is.
            uint32 const maxSlot = uint32(loot->items.size());
            uint32       taken   = 0;

            for (uint32 slot = 0; slot < maxSlot && taken < g_config.maxItems; ++slot)
            {
                LootItem* item = loot->LootItemInSlot(slot, player);
                if (!item || item->is_looted || !item->conditions.empty())
                    continue;

                // StoreLootItem can refuse for reasons other than a full bag -- blocked, won by
                // another roller, not allowed for this player -- and a refusal leaves the corpse
                // untouched, so an unchanged unlootedCount is what says "nothing was taken".
                uint32 const    before = loot->unlootedCount;
                InventoryResult msg    = EQUIP_ERR_OK;
                player->StoreLootItem(uint8(slot), loot, msg);

                if (loot->unlootedCount != before)
                    sScriptMgr->OnPlayerAfterCreatureLoot(player);   // the core raises this per store
                else if (g_config.mailOverflow && !item->freeforall &&
                         readable(player, item, loot->sourceWorldObjectGUID))
                {
                    // Not storable (bags full, unique already owned). Mail it so it is not destroyed
                    // and the corpse can clear -- a deliberate divergence from the core, which mails
                    // only an item container's overflow: the mail is a plain stack, so a random suffix
                    // is lost. A free-for-all slot is left alone because marking it looted would
                    // consume it for every looter. A mail costs one slot of the same per-corpse budget,
                    // so a corpse with more items than the bags hold still stops at MaxItemsPerCorpse.
                    player->SendItemRetrievalMail(item->itemid, item->count);
                    item->is_looted = true;
                    loot->NotifyItemRemoved(item->itemIndex);
                    --loot->unlootedCount;
                }
                else
                    continue;   // refused outright, or deliberately left: nothing left the corpse

                ++taken;
            }
        }

        // A corpse with nothing left behaves exactly like one looted by hand: lootable flagged off,
        // skinnable set, decay timer shortened. A corpse still holding something this player may read
        // (the quest tail, a conditional drop, a slot the pet was told not to take) is left lootable
        // instead: the core's own handler would refuse the follow-up, and clearing the flag would
        // leave a corpse that never becomes looted. `hasFetchableItem` is the same predicate, so a
        // corpse the pet emptied of everything it was allowed to take settles down to the flag
        // exactly when that predicate goes false.
        if (loot->isLooted() || !hasFetchableItem(player, loot, loot->sourceWorldObjectGUID))
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
            std::lock_guard<std::recursive_mutex> lock(g_fetchMutex);
            g_nowMs = getMSTime();
        }

        void OnAfterConfigLoad(bool /*reload*/) override
        {
            g_config.enabled      = sConfigMgr->GetOption<bool>("ManaLootPet.Enable", true);
            g_config.lootDistance = sConfigMgr->GetOption<float>("ManaLootPet.LootDistance", 1.5f);
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
        // lootable flag is set, so this is the earliest the corpse is really lootable. Every such
        // corpse is queued; the pet is only sent off when it is not already on its way somewhere.
        void OnPlayerCreatureKillCredit(Player* player, Creature* killed) override
        {
            if (!canFetch(player, killed))
                return;

            Creature* pet = GetCompanion(player);
            if (!pet)
                return;   // canFetch said there was one; a summon can still land between its two calls

            std::lock_guard<std::recursive_mutex> lock(g_fetchMutex);

            Fetch& fetch = g_fetch[player->GetGUID()];
            fetch.pet    = pet->GetGUID();   // a resummon mid-queue: the old GUID is stale, this one is not
            fetch.queue.push_back(killed->GetGUID());

            // A queue of one means the pet had nowhere to be, so this new corpse is its next stop.
            // Anything else is a kill that landed while it was already away: the queue takes it and
            // the update below walks it next, which is what chains a pull. A pet whose walk has run
            // out of time is not "already away" any more -- it is re-aimed here instead of being left
            // on a corpse it may not even be near.
            if (fetch.queue.size() == 1 || fetch.done(g_nowMs))
            {
                fetch.startMs = g_nowMs;
                SendToCorpse(pet, killed);
            }
        }

        void OnPlayerUpdate(Player* player, uint32 /*p_time*/) override
        {
            // A player under play-time restriction must not receive loot, so a fetch that is still in
            // flight when the restriction lands is unwound here with a lock rather than a network
            // send: the companion is put back on its follow and the entry dropped.
            bool const restricted = player && (player->HasPlayerFlag(PLAYER_FLAGS_NO_PLAY_TIME) ||
                                               player->HasPlayerFlag(PLAYER_FLAGS_PARTIAL_PLAY_TIME));
            if (!player || restricted)
            {
                std::lock_guard<std::recursive_mutex> lock(g_fetchMutex);

                auto stuck = g_fetch.find(player->GetGUID());
                if (stuck != g_fetch.end())
                {
                    ResumeFollow(ObjectAccessor::GetCreature(*player, stuck->second.pet), player);
                    g_fetch.erase(stuck);
                }

                return;
            }

            std::lock_guard<std::recursive_mutex> lock(g_fetchMutex);

            auto itr = g_fetch.find(player->GetGUID());
            if (itr == g_fetch.end())
                return;

            Fetch& fetch = itr->second;
            Creature* pet = ObjectAccessor::GetCreature(*player, fetch.pet);

            // A companion that vanished or died takes the queue with it: the corpses it never
            // reached stay manually lootable. A walk running late is NOT handled here -- one slow leg
            // must not discard the rest of the pull, so it drops that corpse below and moves on.
            if (!pet || !pet->IsAlive())
            {
                ResumeFollow(pet, player);
                g_fetch.erase(itr);
                return;
            }

            // Drop what is no longer worth walking to: a despawned object.
            while (!fetch.queue.empty() &&
                   !player->GetMap()->GetCreature(fetch.queue.front()))
            {
                fetch.queue.erase(fetch.queue.begin());
                fetch.startMs = g_nowMs;
            }

            if (fetch.queue.empty())
            {
                ResumeFollow(pet, player);
                g_fetch.erase(itr);
                return;
            }

            Creature* corpse = player->GetMap()->GetCreature(fetch.queue.front());

            // A corpse is skipped when it is already empty, when it holds nothing the pet may take,
            // or when this leg of the walk has run out of time (stuck, rooted, unreachable). The
            // timeout drops only this corpse and gives the next one a fresh ReachTimeoutMs, so one
            // bad body costs its own deadline instead of the whole pull; only a pet that never gets
            // anywhere drains the queue. The corpse itself is left exactly as it is, lootable flag
            // and decay untouched.
            if (corpse->loot.isLooted() || fetch.done(g_nowMs) ||
                !hasFetchableItem(player, &corpse->loot, corpse->loot.sourceWorldObjectGUID))
            {
                fetch.queue.erase(fetch.queue.begin());
                fetch.startMs = g_nowMs;

                if (fetch.queue.empty())
                {
                    ResumeFollow(pet, player);
                    g_fetch.erase(itr);
                    return;
                }

                SendToCorpse(pet, player->GetMap()->GetCreature(fetch.queue.front()));
                return;
            }

            if (!pet->IsWithinDistInMap(corpse, g_config.lootDistance))
            {
                // Stopped and still out of range: the walk was interrupted (evade, resummon), so it is
                // re-aimed here instead of waiting out a whole ReachTimeoutMs in place. A moving pet
                // is left to finish its own walk.
                if (!pet->HasUnitState(UNIT_STATE_ROAMING_MOVE))
                    SendToCorpse(pet, corpse);

                return;
            }

            // LootCorpse fires script hooks, and a hook that leaves loot behind can re-enter
            // OnPlayerCreatureKillCredit -- which mutates g_fetch. So the entry is re-looked-up from
            // its key afterwards instead of reusing a reference across the call, and the loot state is
            // settled before the queue moves on. Holding the mutex throughout keeps the queue from
            // being rewritten underneath this walk; std::recursive_mutex keeps the re-entry safe.
            ObjectGuid const playerGuid = player->GetGUID();
            LootCorpse(player, corpse);

            itr = g_fetch.find(playerGuid);
            if (itr == g_fetch.end())
                return;   // a re-entrant kill, a logout or an expired play time finished the fetch

            fetch = itr->second;
            pet  = ObjectAccessor::GetCreature(*player, fetch.pet);

            if (!corpse->loot.isLooted())
                return;   // something was left for the player; a later kill re-aims at this corpse

            if (fetch.queue.empty() || fetch.queue.front() != corpse->GetGUID())
                return;   // the re-entry queued something ahead of this corpse

            fetch.queue.erase(fetch.queue.begin());
            fetch.startMs = g_nowMs;

            if (fetch.queue.empty())
            {
                ResumeFollow(pet, player);
                g_fetch.erase(itr);
                return;
            }

            // The pet is sent to the next corpse unconditionally. It has to be: the loot above happens
            // the moment it is in range, which is usually while it is still walking, so the walk it is
            // on is still heading for the corpse just looted. Skipping the send on "already moving"
            // would leave it standing there with the rest of the queue never walked to -- the point
            // generator finalises, UNIT_STATE_ROAMING_MOVE clears, and nothing re-aims it ever again.
            SendToCorpse(pet, player->GetMap()->GetCreature(fetch.queue.front()));
        }

        void OnPlayerLogout(Player* player) override
        {
            if (!player)
                return;

            std::lock_guard<std::recursive_mutex> lock(g_fetchMutex);
            g_fetch.erase(player->GetGUID());
        }
    };
}

void AddSC_ManaLootPet()
{
    new ManaLootPetScript();
}