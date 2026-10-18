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
#include "DatabaseEnv.h"
#include "Item.h"
#include "Log.h"
#include "LootMgr.h"
#include "Mail.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "PetDefines.h"
#include "Player.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "Timer.h"
#include "WorldPacket.h"

#include <algorithm>
#include <atomic>
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

    Config g_config;   // rebuilt whole, under g_fetchMutex, by a config reload

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
        // caller drops that corpse and gives the next one a fresh startMs. The deadline is passed in
        // rather than read from g_config so the caller reads the config once, under its own lock.
        [[nodiscard]] bool done(uint32 nowMs, uint32 reachMs) const
        {
            return !pet || nowMs - startMs > reachMs;
        }
    };

    // Every read or write of g_fetch, and of the clock the walks are timed against, holds
    // g_fetchMutex: the three hooks below can run on different map update threads (MapUpdate.Threads
    // above 1), one of them re-enters through a script hook, and one sends a packet.
    std::unordered_map<ObjectGuid, Fetch> g_fetch;
    std::recursive_mutex                  g_fetchMutex;

    // How many players have a fetch in flight. Read without the lock as an early-out, so the common
    // player tick -- no companion fetch anywhere -- never serialises the map threads on g_fetchMutex.
    std::atomic<uint32> g_fetchCount{0};

    // A reload rebuilds the config on the world thread while the map threads read it, so a read goes
    // through the same recursive mutex that guards the fetches: never a torn value, and safe to call
    // from inside a hook that already holds it.
    Config ReadConfig()
    {
        std::lock_guard<std::recursive_mutex> lock(g_fetchMutex);
        return g_config;
    }

    // Every erase of a fetch goes through here, so g_fetchCount cannot drift from g_fetch.
    void EraseFetch(std::unordered_map<ObjectGuid, Fetch>::iterator itr)
    {
        g_fetch.erase(itr);
        g_fetchCount.fetch_sub(1, std::memory_order_release);
    }

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
        return player && item && item->AllowedForPlayer(player, source) &&
               (item->rollWinnerGUID.IsEmpty() || item->rollWinnerGUID == player->GetGUID());
    }

    // Something on the corpse the pet is allowed to take: money, or one plain item slot that is
    // readable. The two options are passed in rather than read from the global, so a caller that
    // already decided which kinds of loot are in play does not read the config twice.
    bool hasFetchableItem(Player* player, Loot* loot, ObjectGuid source, Config const& cfg)
    {
        if (cfg.money && loot->gold)
            return true;

        if (!cfg.items)
            return false;

        for (LootItem const& item : loot->items)
            if (!item.is_looted && item.conditions.empty() && readable(player, &item, source))
                return true;
        return false;
    }

    // The correctness of the corpse teardown rests on `Loot::unlootedCount`, not on the options: the
    // per-player fill below is what puts the quest tail, the free-for-all slots and the conditional
    // drops into it, so `loot->isLooted()` answers "would the core call this corpse empty for this
    // player" -- the same question its own `HandleLootRelease` asks before it clears the loot.

    // A fresh corpse is worth fetching only if this player is its sole recipient: a group
    // recipient, or another player, keeps its own loot rules and rolls. `cfg` is the caller's snapshot.
    bool canFetch(Player* player, Creature* killed, Config const& cfg)
    {
        if (!cfg.enabled || !player || !killed || !killed->IsInWorld())
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
        return hasFetchableItem(player, &killed->loot, killed->loot.sourceWorldObjectGUID, cfg);
    }

    // Point the pet at a corpse and let it walk. frand asserts max >= min, and lootDistance is only
    // clamped to >= 0.5, so the spawn radius is the lower bound whenever a small LootDistance
    // (down to the documented 0.5) would otherwise put the upper bound below it.
    void SendToCorpse(Creature* pet, Creature* corpse, float lootDistance)
    {
        if (!pet || !corpse)
            return;

        float const spread = std::max(0.5f, lootDistance * 0.6f);
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

    // The Postmaster's own letter text, as the core's `Player::SendItemRetrievalMail` uses it.
    char const* const kMailSubject = "Recovered Item";
    char const* const kMailBody    = "We recovered a lost item in the twisting nether and noted that it was yours.$B$BPlease find said object enclosed.";

    // One un-storable slot, as the mail needs it back: a mail stores an item instance, so unlike
    // (entry, count) it also carries the loot's random roll. `count` is the rolled stack size.
    struct Overflow
    {
        uint32 itemid;
        uint32 count;
        int32  randomPropertyId;   // > 0 a random-property id, < 0 a random-suffix id, 0 neither
    };

    // Mail the slots the pet could not store, as few mails as the mailbox allows. This is the body
    // of the core's own `Player::SendItemRetrievalMail` batch, over item instances rather than
    // (entry, count) pairs: that overload re-creates each stack and so drops a looted item's random
    // roll, while `Item::CreateItem` -- the call the store path itself uses -- takes it. A slot that
    // cannot be recovered at all (item template gone, DB error) is logged and dropped: the corpse has
    // already given it up, and the failure is not something the player did.
    void MailOverflow(Player* player, std::vector<Overflow> const& overflow)
    {
        if (overflow.empty())
            return;

        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
        MailDraft                   draft(kMailSubject, kMailBody);
        MailSender const            sender(MAIL_CREATURE, 34337 /* The Postmaster */);
        MailReceiver const          receiver(player, player->GetGUID().GetCounter());
        uint32                      perMail = 0;

        for (Overflow const& entry : overflow)
        {
            // A stack above the item's own maximum would be silently clamped by CreateItem, the same
            // loss the pair overload guards against with its own split.
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry.itemid);
            if (!proto)
            {
                LOG_ERROR("modules", "mod-mana-loot-pet: cannot mail unknown item {}", entry.itemid);
                continue;
            }

            // CreateItem already applies the loot's random roll (a suffix factor of its own is seeded
            // from the entry, as the core does on a load, so there is no seed to carry over).
            int32 const suffix = entry.randomPropertyId;

            for (uint32 left = entry.count; left;)
            {
                uint32 const count = std::min(left, proto->GetMaxStackSize());
                Item* mailItem     = Item::CreateItem(entry.itemid, count, player, false, suffix);
                if (!mailItem)
                {
                    LOG_ERROR("modules", "mod-mana-loot-pet: cannot create item {} for mail", entry.itemid);
                    break;
                }

                mailItem->SaveToDB(trans);   // saved first so a failed send cannot lose it

                draft.AddItem(mailItem);
                left -= count;

                if (++perMail >= MAX_MAIL_ITEMS)
                {
                    draft.SendMailTo(trans, receiver, sender);
                    draft   = MailDraft(kMailSubject, kMailBody);
                    perMail = 0;
                }
            }
        }

        if (perMail)
            draft.SendMailTo(trans, receiver, sender);

        CharacterDatabase.CommitTransaction(trans);
    }

    // Walk the corpse's loot into the player. Mirrors the core's own single-player loot path
    // (HandleAutostoreLootItemOpcode / HandleLootMoneyOpcode), minus the loot window: no window is
    // ever sent, so the client never has to be told the corpse was looted. `cfg` is the caller's
    // snapshot, so nothing here can disagree with the config the fetch pass read.
    void LootCorpse(Player* player, Creature* corpse, Config const& cfg)
    {
        Loot* loot = &corpse->loot;

        // Whether the pet actually took anything, which is what the "a creature was looted" report
        // below keys off: a corpse it was told to leave alone must not report a loot.
        bool tookAnything = false;

        // Per-player FFA / quest / conditional caches, normally built by Player::SendLoot. Idempotent.
        loot->FillNotNormalLootFor(player);

        if (cfg.money && loot->gold)
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
                tookAnything = true;
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

        if (cfg.items)
        {
            // Only the plain item list is walked: LootItemInSlot puts slot >= items.size() into the
            // quest_items tail, and a conditional drop keeps its QuestItem bookkeeping in a per-player
            // map -- both are left for the player to hand-loot, so a slot with conditions is skipped
            // the same way the quest tail is.
            uint32 const maxSlot = uint32(loot->items.size());
            uint32       taken   = 0;

            // Slots the pet cannot store, mailed in one batch after the walk: mailing each slot on
            // the spot would build one draft -- one mail -- per item.
            std::vector<Overflow> overflow;

            for (uint32 slot = 0; slot < maxSlot && taken < cfg.maxItems; ++slot)
            {
                LootItem* item = loot->LootItemInSlot(slot, player);
                // Not this player's to take (another looter's, or hidden by `follow_loot_rules`):
                // `StoreLootItem` would answer that with `SendLootRelease`, and the slot must stay on
                // the corpse, so it is never even asked about.
                if (!item || item->is_looted || !item->conditions.empty() ||
                    !readable(player, item, loot->sourceWorldObjectGUID))
                    continue;

                // Ask the storage check directly rather than letting `StoreLootItem` refuse: it
                // answers with a red inventory/unique error on the client, which is wrong here
                // because the refusal is not the player's action and the item is about to be mailed.
                // "Can there be room for it" also covers a unique the player already owns, and it is
                // silent; the loot is not touched by asking.
                ItemPosCountVec    dest;
                InventoryResult const storable = player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest,
                                                                        item->itemid, item->count);
                if (storable == EQUIP_ERR_OK)
                {
                    uint32 const before = loot->unlootedCount;
                    InventoryResult msg = EQUIP_ERR_OK;
                    player->StoreLootItem(uint8(slot), loot, msg);

                    if (loot->unlootedCount != before)
                    {
                        tookAnything = true;
                        ++taken;
                    }
                    // A refusal the check above did not predict took nothing and reported itself;
                    // the slot stays on the corpse for the next pass.
                    continue;
                }

                if (!cfg.mailOverflow || item->freeforall)
                    continue;   // left on the corpse deliberately

                // Not storable (bags full, unique already owned). Mail it so it is not destroyed and
                // the corpse can clear -- a deliberate divergence from the core, which mails only an
                // item container's overflow. A free-for-all slot is left alone because marking it
                // looted would consume it for every looter. Queued, not mailed here, so the whole
                // corpse travels in as few mails as the mailbox allows; a queued slot still costs one
                // slot of the same per-corpse budget, so a corpse with more items than the bags hold
                // stops at MaxItemsPerCorpse. The loot's random roll travels with it, because the mail
                // is built from an item instance rather than (entry, count).
                overflow.push_back({ item->itemid, item->count, item->randomPropertyId });
                item->is_looted = true;
                loot->NotifyItemRemoved(item->itemIndex);
                --loot->unlootedCount;
                tookAnything = true;
                ++taken;
            }

            // One batch for the whole corpse: MAX_MAIL_ITEMS (12) items per mail, so only a bigger
            // overflow spills into a second. A slot whose mail could not be built at all (item template
            // gone, DB error) is logged inside and not retried: the corpse has already given it up.
            // shortcut: that loss is a server log line with no client notice, upgrade if players report
            // missing overflow mail with no explanation.
            MailOverflow(player, overflow);
        }

        // One "a creature was looted" report for the whole corpse, which is what the core's own
        // handler raises -- once per loot action -- and not once per item taken. The money report is
        // its own hook above, exactly as in `HandleLootMoneyOpcode`.
        if (tookAnything)
            sScriptMgr->OnPlayerAfterCreatureLoot(player);

        // A corpse with nothing left for this player behaves exactly like one looted by hand: lootable
        // flagged off, skinnable set, decay timer shortened. `Loot::unlootedCount` is the whole test --
        // `FillNotNormalLootFor` above just filled this player's quest / free-for-all / conditional
        // slots into it, so it is non-zero whenever something is still there to hand-loot, whatever the
        // module's own options say about what the pet may take. That is what stops the old bug: a corpse
        // holding only loot the options excluded used to be torn down with that loot still on it.
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
            std::lock_guard<std::recursive_mutex> lock(g_fetchMutex);
            g_nowMs = getMSTime();
        }

        void OnAfterConfigLoad(bool /*reload*/) override
        {
            // Built whole, then moved in under the mutex, so a map thread reading the config never
            // sees half of the old values and half of the new ones.
            Config cfg;
            cfg.enabled      = sConfigMgr->GetOption<bool>("ManaLootPet.Enable", true);
            cfg.lootDistance = sConfigMgr->GetOption<float>("ManaLootPet.LootDistance", 1.5f);
            cfg.reachMs      = sConfigMgr->GetOption<uint32>("ManaLootPet.ReachTimeoutMs", 8000);
            cfg.money        = sConfigMgr->GetOption<bool>("ManaLootPet.LootMoney", true);
            cfg.items        = sConfigMgr->GetOption<bool>("ManaLootPet.LootItems", true);
            cfg.mailOverflow = sConfigMgr->GetOption<bool>("ManaLootPet.MailWhenBagsFull", true);
            cfg.maxItems     = sConfigMgr->GetOption<uint32>("ManaLootPet.MaxItemsPerCorpse", 20);

            // A pet that has to touch the corpse needs a real distance; one far below it would just
            // loot the moment the kill lands. The timeout must at least outlast a walk across range.
            if (cfg.lootDistance < 0.5f)
                cfg.lootDistance = 0.5f;
            if (cfg.reachMs < 1000)
                cfg.reachMs = 1000;
            if (!cfg.maxItems)
                cfg.maxItems = 1;

            std::lock_guard<std::recursive_mutex> lock(g_fetchMutex);
            g_config = cfg;
        }

        // The kill credit is raised once per creature, after the corpse's loot is generated and the
        // lootable flag is set, so this is the earliest the corpse is really lootable. Every such
        // corpse is queued; the pet is only sent off when it is not already on its way somewhere.
        void OnPlayerCreatureKillCredit(Player* player, Creature* killed) override
        {
            // The config is read once for the whole decision, before the lock: the fetch pass must not
            // see a reload land between its gate and its walk.
            Config const cfg = ReadConfig();

            if (!canFetch(player, killed, cfg))
                return;

            Creature* pet = GetCompanion(player);
            if (!pet)
                return;   // canFetch said there was one; a summon can still land between its two calls

            std::lock_guard<std::recursive_mutex> lock(g_fetchMutex);

            auto inserted = g_fetch.try_emplace(player->GetGUID());
            if (inserted.second)
                g_fetchCount.fetch_add(1, std::memory_order_release);

            Fetch& fetch = inserted.first->second;
            fetch.pet    = pet->GetGUID();   // a resummon mid-queue: the old GUID is stale, this one is not
            fetch.queue.push_back(killed->GetGUID());

            // A queue of one means the pet had nowhere to be, so this new corpse is its next stop.
            // Anything else is a kill that landed while it was already away: the queue takes it and
            // the update below walks it next, which is what chains a pull. A pet whose walk has run
            // out of time is not "already away" any more -- it is re-aimed here instead of being left
            // on a corpse it may not even be near.
            if (fetch.queue.size() == 1 || fetch.done(g_nowMs, cfg.reachMs))
            {
                fetch.startMs = g_nowMs;
                SendToCorpse(pet, killed, cfg.lootDistance);
            }
        }

        void OnPlayerUpdate(Player* player, uint32 /*p_time*/) override
        {
            if (!player)
                return;   // nothing to own a fetch; never dereferenced below

            // Cheap unlocked early-out: with no fetch in flight anywhere there is nothing to drive,
            // which is what keeps every player tick from serialising on g_fetchMutex.
            if (!g_fetchCount.load(std::memory_order_acquire))
                return;

            Config const cfg = ReadConfig();

            // A player under play-time restriction must not receive loot, so a fetch that is still in
            // flight when the restriction lands is unwound here with a lock rather than a network
            // send: the companion is put back on its follow and the entry dropped.
            bool const restricted = player->HasPlayerFlag(PLAYER_FLAGS_NO_PLAY_TIME) ||
                                    player->HasPlayerFlag(PLAYER_FLAGS_PARTIAL_PLAY_TIME);
            if (restricted)
            {
                std::lock_guard<std::recursive_mutex> lock(g_fetchMutex);

                auto stuck = g_fetch.find(player->GetGUID());
                if (stuck != g_fetch.end())
                {
                    ResumeFollow(ObjectAccessor::GetCreature(*player, stuck->second.pet), player);
                    EraseFetch(stuck);
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
                EraseFetch(itr);
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
                EraseFetch(itr);
                return;
            }

            Creature* corpse = player->GetMap()->GetCreature(fetch.queue.front());

            // A corpse is skipped when it is already empty, when it holds nothing the pet may take,
            // or when this leg of the walk has run out of time (stuck, rooted, unreachable). The
            // timeout drops only this corpse and gives the next one a fresh ReachTimeoutMs, so one
            // bad body costs its own deadline instead of the whole pull; only a pet that never gets
            // anywhere drains the queue. The corpse itself is left exactly as it is, lootable flag
            // and decay untouched.
            if (corpse->loot.isLooted() || fetch.done(g_nowMs, cfg.reachMs) ||
                !hasFetchableItem(player, &corpse->loot, corpse->loot.sourceWorldObjectGUID, cfg))
            {
                fetch.queue.erase(fetch.queue.begin());
                fetch.startMs = g_nowMs;

                if (fetch.queue.empty())
                {
                    ResumeFollow(pet, player);
                    EraseFetch(itr);
                    return;
                }

                SendToCorpse(pet, player->GetMap()->GetCreature(fetch.queue.front()), cfg.lootDistance);
                return;
            }

            if (!pet->IsWithinDistInMap(corpse, cfg.lootDistance))
            {
                // Stopped and still out of range: the walk was interrupted (evade, resummon), so it is
                // re-aimed here instead of waiting out a whole ReachTimeoutMs in place. A moving pet
                // is left to finish its own walk.
                if (!pet->HasUnitState(UNIT_STATE_ROAMING_MOVE))
                    SendToCorpse(pet, corpse, cfg.lootDistance);

                return;
            }

            // LootCorpse fires script hooks, and a hook that leaves loot behind can re-enter
            // OnPlayerCreatureKillCredit -- which mutates g_fetch. So the entry is re-looked-up from
            // its key afterwards, because a reference cannot be rebound and an erase/insert of the
            // same key would leave the old one dangling. Holding the mutex throughout keeps the queue
            // from being rewritten underneath this walk; std::recursive_mutex keeps the re-entry safe.
            ObjectGuid const playerGuid = player->GetGUID();
            LootCorpse(player, corpse, cfg);

            itr = g_fetch.find(playerGuid);
            if (itr == g_fetch.end())
                return;   // a re-entrant kill, a logout or an expired play time finished the fetch

            pet = ObjectAccessor::GetCreature(*player, itr->second.pet);

            if (!corpse->loot.isLooted())
                return;   // something was left for the player; a later kill re-aims at this corpse

            if (itr->second.queue.empty() || itr->second.queue.front() != corpse->GetGUID())
                return;   // the re-entry queued something ahead of this corpse

            itr->second.queue.erase(itr->second.queue.begin());
            itr->second.startMs = g_nowMs;

            if (itr->second.queue.empty())
            {
                ResumeFollow(pet, player);
                EraseFetch(itr);
                return;
            }

            // The pet is sent to the next corpse unconditionally. It has to be: the loot above happens
            // the moment it is in range, which is usually while it is still walking, so the walk it is
            // on is still heading for the corpse just looted. Skipping the send on "already moving"
            // would leave it standing there with the rest of the queue never walked to -- the point
            // generator finalises, UNIT_STATE_ROAMING_MOVE clears, and nothing re-aims it ever again.
            SendToCorpse(pet, player->GetMap()->GetCreature(itr->second.queue.front()), cfg.lootDistance);
        }

        void OnPlayerLogout(Player* player) override
        {
            if (!player)
                return;

            std::lock_guard<std::recursive_mutex> lock(g_fetchMutex);
            auto itr = g_fetch.find(player->GetGUID());
            if (itr != g_fetch.end())
                EraseFetch(itr);
        }
    };
}

void AddSC_ManaLootPet()
{
    new ManaLootPetScript();
}