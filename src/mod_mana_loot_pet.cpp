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
#include "Tokenize.h"
#include "WorldPacket.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <mutex>
#include <numbers>
#include <unordered_map>

namespace
{
    struct Config
    {
        bool   enabled      = true;
        float  lootDistance = 1.5f;  // how close the pet must get before the corpse is looted
        float  maxDistance  = 40.0f; // how close the player must be to a corpse before the pet is sent (0 = anywhere)
        uint32 reachMs      = 8000;  // a fetch older than this is abandoned (pet stuck or corpse gone)
        bool   money        = true;
        bool   items        = true;
        bool   mailOverflow = true;
        uint32 maxItems     = 20;    // most slots taken from one corpse in a single fetch
        uint32 petlessMs    = 60000; // how long a kill waits for a companion to be summoned
        // Creature entries allowed to fetch. Empty = any companion, which is the historical behaviour.
        std::vector<uint32> petIds;
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

        // True while the pet is actually walking to queue.front(). A corpse the player is too far
        // from is queued but not armed: the pet stays on its follow until the player closes in.
        bool walking = false;

        // True once this leg of the walk has outlived ReachTimeoutMs. The timeout is per corpse: the
        // caller drops that corpse and gives the next one a fresh startMs. The deadline is passed in
        // rather than read from g_config so the caller reads the config once, under its own lock.
        // `petlessMs` rather than reachMs: a queue with no companion at all needs a longer grace, so a
        // pet summoned a few seconds after the kill still fetches it (0 = never queue without a pet).
        [[nodiscard]] bool done(uint32 nowMs, uint32 reachMs, uint32 petlessMs) const
        {
            if (!pet)
                return !petlessMs || nowMs - startMs > petlessMs;

            // A corpse waiting for the player to close in has not been walked to yet.
            return walking && nowMs - startMs > reachMs;
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

    // Is this companion one the config lets fetch? An empty list means every companion may, so an
    // untouched .conf keeps the module behaving as it always did.
    bool companionAllowed(Creature* pet, Config const& cfg)
    {
        if (!pet)
            return false;
        if (cfg.petIds.empty())
            return true;
        return std::find(cfg.petIds.begin(), cfg.petIds.end(), pet->GetEntry()) != cfg.petIds.end();
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
        // A missing companion no longer aborts the fetch: the corpse waits in the queue for one to be
        // summoned, for QueueWithoutPetMs (0 disables that and restores the old pet-only gate). An
        // unlisted companion, though, never fetches, so there is nothing to queue for.
        Creature* const companion = GetCompanion(player);
        if (companion && !companionAllowed(companion, cfg))
            return false;
        if (!companion && !cfg.petlessMs)
            return false;

        // Nothing the pet can help itself to is still no reason to walk over there: a corpse holding
        // only loot that is being left for the player would strand it in the queue, stop the pet from
        // following, and eat a ReachTimeoutMs before it moved on. This is the gate that decides which
        // kills are fetched at all, so it has to count money as well as items.
        killed->loot.FillNotNormalLootFor(player);
        return hasFetchableItem(player, &killed->loot, killed->loot.sourceWorldObjectGUID, cfg);
    }

    // Is the player close enough for the pet to be sent over? A corpse across the zone would only
    // make the pet walk off and time out, so the fetch waits until the player closes the gap. Checked
    // against the player, not the pet: the pet follows the player, so its own distance says nothing
    // about whether the player is coming.
    bool inSendRange(Player* player, Creature* corpse, Config const& cfg)
    {
        if (!player || !corpse)
            return false;
        if (cfg.maxDistance <= 0.0f)
            return true;   // 0 disables the gate
        return player->IsWithinDistInMap(corpse, cfg.maxDistance);
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
            cfg.maxDistance  = sConfigMgr->GetOption<float>("ManaLootPet.MaxDistance", 40.0f);
            cfg.reachMs      = sConfigMgr->GetOption<uint32>("ManaLootPet.ReachTimeoutMs", 8000);
            cfg.money        = sConfigMgr->GetOption<bool>("ManaLootPet.LootMoney", true);
            cfg.items        = sConfigMgr->GetOption<bool>("ManaLootPet.LootItems", true);
            cfg.mailOverflow = sConfigMgr->GetOption<bool>("ManaLootPet.MailWhenBagsFull", true);
            cfg.maxItems     = sConfigMgr->GetOption<uint32>("ManaLootPet.MaxItemsPerCorpse", 20);
            cfg.petlessMs    = sConfigMgr->GetOption<uint32>("ManaLootPet.QueueWithoutPetMs", 60000);

            // "", "0" and "all" (or any unparsable token) leave the list empty, i.e. any companion.
            std::string const ids = sConfigMgr->GetOption<std::string>("ManaLootPet.PetIds", "");
            for (std::string_view token : Acore::Tokenize(ids, ',', false))
            {
                // Tolerate "1, 2" spacing in the .conf.
                size_t const first = token.find_first_not_of(" \t");
                if (first == std::string_view::npos)
                    continue;
                token.remove_prefix(first);
                token.remove_suffix(token.size() - token.find_last_not_of(" \t") - 1);

                uint32 entry = 0;
                auto const [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), entry);
                if (ec == std::errc() && ptr == token.data() + token.size() && entry)
                    cfg.petIds.push_back(entry);
            }

            // A pet that has to touch the corpse needs a real distance; one far below it would just
            // loot the moment the kill lands. The timeout must at least outlast a walk across range.
            if (cfg.lootDistance < 0.5f)
                cfg.lootDistance = 0.5f;
            // A negative MaxDistance is meaningless; clamp to 0, which is also the "no gate" value.
            if (cfg.maxDistance < 0.0f)
                cfg.maxDistance = 0.0f;
            if (cfg.reachMs < 1000)
                cfg.reachMs = 1000;
            if (!cfg.maxItems)
                cfg.maxItems = 1;
            // 0 means "never queue a kill without a companion"; cap the rest at a day so a typo
            // cannot pin a fetch for a week.
            if (cfg.petlessMs > 24u * 60 * 60 * 1000)
                cfg.petlessMs = 24u * 60 * 60 * 1000;

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

            std::lock_guard<std::recursive_mutex> lock(g_fetchMutex);

            auto inserted = g_fetch.try_emplace(player->GetGUID());
            if (inserted.second)
                g_fetchCount.fetch_add(1, std::memory_order_release);

            Fetch& fetch = inserted.first->second;
            fetch.queue.push_back(killed->GetGUID());

            // A resummon mid-queue: the old GUID is stale, this one is not. With no companion the
            // queue is left petless, and a summon picks it up on the next player tick. The grace clock
            // is re-anchored to this kill so a corpse appended to an already waiting queue gets its
            // own window instead of inheriting one that may already be spent.
            if (pet)
                fetch.pet = pet->GetGUID();
            else
                fetch.startMs = g_nowMs;

            // A queue of one means the pet had nowhere to be, so this new corpse is its next stop.
            // Anything else is a kill that landed while it was already away: the queue takes it and
            // the update below walks it next, which is what chains a pull. A pet whose walk has run
            // out of time is not "already away" any more -- it is re-aimed here instead of being left
            // on a corpse it may not even be near. A corpse the player is too far from is queued
            // without a send: the pet stays on its follow and OnPlayerUpdate arms the walk once the
            // player has closed the gap. Only the head of a queue of one is ever aimed at here, so a
            // kill landing behind a corpse already waiting cannot leapfrog it.
            if (fetch.queue.size() == 1 || fetch.done(g_nowMs, cfg.reachMs, cfg.petlessMs))
            {
                bool const wasWalking = fetch.walking;

                fetch.startMs = g_nowMs;
                fetch.walking = false;

                Creature* target = player->GetMap()->GetCreature(fetch.queue.front());
                if (pet && inSendRange(player, target, cfg))
                {
                    fetch.walking = true;
                    SendToCorpse(pet, target, cfg.lootDistance);
                }
                else if (wasWalking)
                    ResumeFollow(pet, player);   // a timed-out walk is being abandoned, not left running
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
            Creature* pet = fetch.pet ? ObjectAccessor::GetCreature(*player, fetch.pet) : nullptr;

            // Resolve the companion actually walking the queue. A petless queue is kept, not dropped:
            // the corpse was lootable while no companion (or an unlisted one, which never fetches) was
            // out, and it waits here until a summon adopts it. A summon that replaces the pet -- or a
            // swap for another listed companion -- adopts the fresh GUID the same way, because the
            // stale one resolves to nullptr above. Only a dead or unlisted companion drops the queue:
            // the corpses it never reached stay manually lootable.
            if (!pet || !pet->IsAlive() || !companionAllowed(pet, cfg))
            {
                Creature* replacement = GetCompanion(player);

                if (replacement && replacement->IsAlive() && companionAllowed(replacement, cfg))
                {
                    pet           = replacement;
                    fetch.pet     = pet->GetGUID();
                    fetch.startMs = g_nowMs;   // the new walk's clock, not the old pet's
                    fetch.walking = false;     // whatever generator the old pet had is gone with it
                }
                else if (pet)
                {
                    ResumeFollow(pet, player);
                    EraseFetch(itr);
                    return;
                }
                // else: still petless, and the branch below keeps the corpse queued.
            }

            // Drop what is no longer worth walking to: a despawned object.
            while (!fetch.queue.empty() &&
                   !player->GetMap()->GetCreature(fetch.queue.front()))
            {
                fetch.queue.erase(fetch.queue.begin());
                fetch.startMs = g_nowMs;

                // The walk in progress was aimed at the object that just went away, so the point
                // generator it left is meaningless: put the follow back before arming the next stop.
                if (fetch.walking)
                {
                    fetch.walking = false;
                    ResumeFollow(pet, player);
                }
            }

            if (fetch.queue.empty())
            {
                ResumeFollow(pet, player);
                EraseFetch(itr);
                return;
            }

            // Nothing further can be done without a companion: the corpse keeps waiting. A corpse that
            // emptied, or whose grace window ran out, is dropped -- the grace is what stops a player
            // with no pet holding a fetch (and its g_fetchCount early-out) forever.
            if (!pet)
            {
                Creature* head = player->GetMap()->GetCreature(fetch.queue.front());
                if (!head || head->loot.isLooted() || fetch.done(g_nowMs, cfg.reachMs, cfg.petlessMs))
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
            if (corpse->loot.isLooted() || fetch.done(g_nowMs, cfg.reachMs, cfg.petlessMs) ||
                !hasFetchableItem(player, &corpse->loot, corpse->loot.sourceWorldObjectGUID, cfg))
            {
                fetch.queue.erase(fetch.queue.begin());
                fetch.startMs = g_nowMs;
                fetch.walking = false;

                if (fetch.queue.empty())
                {
                    ResumeFollow(pet, player);
                    EraseFetch(itr);
                    return;
                }

                Creature* next = player->GetMap()->GetCreature(fetch.queue.front());
                fetch.walking = inSendRange(player, next, cfg);
                if (fetch.walking)
                    SendToCorpse(pet, next, cfg.lootDistance);
                else
                    ResumeFollow(pet, player);
                return;
            }

            // The player is not close enough for the pet to be sent over yet. The corpse stays at the
            // head of the queue and the pet stays on its follow -- the follow generator already walks
            // it back to the player, so there is nothing to re-issue each tick -- and closing the gap
            // is what arms the walk. Nothing happens here until the player is in range.
            if (!fetch.walking)
            {
                if (!inSendRange(player, corpse, cfg))
                    return;

                fetch.walking = true;
                fetch.startMs = g_nowMs;
                SendToCorpse(pet, corpse, cfg.lootDistance);
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
            itr->second.walking = false;

            if (itr->second.queue.empty())
            {
                ResumeFollow(pet, player);
                EraseFetch(itr);
                return;
            }

            // The next stop is armed only when the player is already within MaxDistance of it; a
            // corpse further away waits on the follow until the player closes in. The send itself is
            // unconditional once armed, because the loot above happens while the pet is still walking
            // -- skipping it on "already moving" would leave the point generator finalised with
            // nothing to re-aim it, and the rest of the queue would never be walked.
            Creature* next = player->GetMap()->GetCreature(itr->second.queue.front());
            itr->second.walking = inSendRange(player, next, cfg);

            if (itr->second.walking)
                SendToCorpse(pet, next, cfg.lootDistance);
            else
                ResumeFollow(pet, player);
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