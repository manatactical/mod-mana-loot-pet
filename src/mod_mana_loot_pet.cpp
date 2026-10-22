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
#include <memory>
#include <mutex>
#include <numbers>
#include <unordered_map>

namespace
{
    // One definition of "is the mimic on by default", so the pre-config-load flag, the INI reader and
    // the struct default cannot drift apart.
    constexpr bool kMimicCombatDefault = true;

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
        bool   mimicCombat  = kMimicCombatDefault; // pretend to attack the target (no damage)
        // Creature entries allowed to fetch. Empty = any companion, which is the historical behaviour.
        std::vector<uint32> petIds;
    };

    // Published whole and read under its own (otherwise never taken) mutex: a map thread never sees
    // half of one config, and the read never contends with the fetch lock. Read at most once per
    // polled fetch, so the lock costs nothing measurable. A shared_ptr rather than a value type on
    // purpose: a kill credit can land before OnAfterConfigLoad publishes the real config, and the
    // default-constructed snapshot is what makes that path safe.
    std::shared_ptr<Config const> g_config = std::make_shared<Config const>();
    std::mutex                    g_configMutex;

    // One entry per player: the corpses the companion still owes a visit, in the order they dropped.
    // The pet's own GUID is kept because a companion summoned or dismissed mid-fetch makes the
    // creature pointer stale, and the corpse GUIDs are kept so a despawned object is simply not
    // found again. A queue rather than a single corpse is what lets a pull chain: the pet walks the
    // corpses one by one instead of the second kill staying on the ground.
    struct Fetch
    {
        std::vector<ObjectGuid> queue;
        ObjectGuid              pet;
        // when the walk to queue.front() was (re)started
        uint32                  startMs = 0;

        // When this fetch may next be looked at. The player-tick hook polls a fetch at 10 Hz rather
        // than at the map tick, which is the difference between a few checks a second and a hundred.
        uint32 nextCheckMs = 0;

        // True while the pet is actually walking to queue.front(). A corpse the player is too far
        // from is queued but not armed: the pet stays on its follow until the player closes in.
        bool walking = false;

        // True once this leg of the walk has outlived ReachTimeoutMs. The timeout is per corpse: the
        // caller drops that corpse and gives the next one a fresh startMs. The deadline is passed in
        // rather than read from the published config so a caller reads that config once per pass.
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

    // How often a live fetch is polled. The player tick runs at the map interval (10 ms by default),
    // an order of magnitude more often than a walking pet needs: at a 1.5 yard tolerance a walk takes
    // hundreds of milliseconds per yard, so ~100 ms of extra latency is invisible in game.
    constexpr uint32 kCheckIntervalMs = 100;

    // All fetches live behind one recursive mutex. Sharding them by player GUID was tried and
    // rejected: `LootCorpse` holds the lock while it fires script hooks, and a hook that credits a kill
    // for a *different* player would take that player's shard lock while this one is held -- a
    // lock-order inversion between two map threads, which a recursive mutex cannot rescue. One lock
    // keeps the invariant simple: no code path ever holds two of them.
    // shortcut: one mutex for all fetches, contention only matters past a few hundred concurrent
    // fetches; shard by map, not by GUID, if that is ever measured.
    std::unordered_map<ObjectGuid, Fetch> g_fetch;
    std::recursive_mutex                  g_fetchMutex;

    // Read without any lock as an early-out, so the common player tick -- no companion fetch anywhere
    // -- never takes the mutex.
    std::atomic<uint32> g_fetchCount{0};

    // The world tick keeps the clock. Atomic rather than mutex-guarded: a reader must never wait on
    // the world thread to learn what time it is. It is advisory, not a synchronisation point: a map
    // thread may read a value one world tick old, which only shortens or extends a deadline by that
    // tick. All arithmetic is unsigned subtraction, so a stale read can never wrap into a huge value.
    // A fresh process starts at 1 so that "never set" cannot read as a fetch that started at zero.
    std::atomic<uint32> g_nowMs{1};

    // Every erase of a fetch goes through here, so g_fetchCount cannot drift from g_fetch.
    void EraseFetch(std::unordered_map<ObjectGuid, Fetch>::iterator itr)
    {
        g_fetch.erase(itr);
        g_fetchCount.fetch_sub(1, std::memory_order_release);
    }

    // The cosmetic mimic's own state, keyed by player, behind its own mutex. A separate map and a
    // separate lock because the mimic must work for a player with no corpse queued at all, and because
    // it must never serialise against the loot fetch: a cosmetic feature has no business on the lock
    // the real one uses.
    struct Mimic
    {
        ObjectGuid target;
        uint32     swingMs   = 0;     // earliest tick the next swing animation may play
        bool       engaged   = false; // already stood beside the target once (hysteresis state)
        uint32     walkSentMs = 0;    // last approach issued, so a refused MovePoint is retried slowly
    };

    std::unordered_map<ObjectGuid, Mimic> g_mimic;
    std::recursive_mutex                  g_mimicMutex;

    // Read without the mutex on the hot path: with the feature off a player tick costs one relaxed load
    // and nothing else. Seeded from the struct default so the flag is right before the first reload.
    std::atomic<bool> g_mimicEnabled{kMimicCombatDefault};

    // How often the mimic is evaluated. It is a cosmetic, so a tenth of a second is plenty and keeps a
    // fully enabled server off this path 90% of the time.
    constexpr uint32 kMimicCheckMs = 100;

    // The config is read through a snapshot: a reload publishes a whole new Config and the reader
    // keeps its own reference, so the call it is in never sees half of an old and half of a new one.
    // A short mutex rather than an atomic shared_ptr: this is at most one read per polled fetch, and
    // a lock that is never contended in practice is cheaper than the portability of atomic<shared_ptr>.
    std::shared_ptr<Config const> ReadConfig()
    {
        std::lock_guard<std::mutex> lock(g_configMutex);
        return g_config;
    }

    void PublishConfig(Config cfg)
    {
        auto published = std::make_shared<Config const>(std::move(cfg));
        std::lock_guard<std::mutex> lock(g_configMutex);
        g_config = std::move(published);
    }

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

    // How close the pet stands to the target while imitating an attack, and how far it may drift
    // before walking back. The gap between the two is the hysteresis: with one threshold the pet
    // re-issues a walk the moment it drifts a hair over the line, which reads as constant fidgeting.
    constexpr float kMimicRange    = 2.5f;
    constexpr float kMimicRelease  = 4.0f;

    // How often the pet throws a swing animation. The emote is a pure SMSG_EMOTE; nothing in the
    // combat system is touched, so the rate only has to look right, not match a real attack speed.
    constexpr uint32 kMimicSwingMs = 1500;

    // Minimum gap between two approaches. Only reached when a walk finished outside range or was
    // refused outright: the normal case issues one MovePoint and then stands still.
    constexpr uint32 kMimicRetryMs = 1000;

    // Where the pet stands to "attack" the target: beside it, clear of the target's own body, and
    // already turned toward it. The spot is on the side the pet is *already* on, so it takes the
    // shortest walk and never circles the target to a fresh random angle. The orientation is handed to
    // MovePoint so the pet arrives facing the target instead of spinning on the spot after the spline.
    void SendToMimic(Creature* pet, Unit* target)
    {
        if (!pet || !target)
            return;

        // Angle from the target to the pet, expressed the way GetNearPosition wants it -- relative to
        // the target's own orientation (MovePosition adds GetOrientation()). Approaching from where the
        // pet already stands means the shortest walk, never a circle to a fresh random side.
        float const fromPet = target->GetRelativeAngle(pet->GetPositionX(), pet->GetPositionY());

        // Stand outside the target's model, not inside it: a raid boss has a combat reach of several
        // yards, so a fixed offset would bury the companion in it.
        float const stand = std::max(kMimicRange * 0.8f, target->GetCombatReach() + 1.0f);
        Position const spot = target->GetNearPosition(stand, fromPet);

        pet->GetMotionMaster()->MovePoint(0, spot.GetPositionX(), spot.GetPositionY(), spot.GetPositionZ(),
                                          FORCED_MOVEMENT_NONE, 0.0f,
                                          spot.GetAbsoluteAngle(target->GetPositionX(), target->GetPositionY()));
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
            g_nowMs.store(getMSTime(), std::memory_order_relaxed);
        }

        void OnAfterConfigLoad(bool /*reload*/) override
        {
            // Built whole, then published whole, so a map thread reading the config never sees half of
            // the old values and half of the new ones.
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
            cfg.mimicCombat  = sConfigMgr->GetOption<bool>("ManaLootPet.MimicCombat", kMimicCombatDefault);

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

            // Published whole, so a reader either sees the old config or the new one, never a mix.
            bool const wasEnabled = g_mimicEnabled.exchange(cfg.mimicCombat, std::memory_order_relaxed);
            PublishConfig(std::move(cfg));

            // A reload that turns the feature off must also unpark the pets already standing on a
            // target, or they would stay there until each player logs out.
            if (wasEnabled && !g_mimicEnabled.load(std::memory_order_relaxed))
                DropAllMimics();
        }

        // The kill credit is raised once per creature, after the corpse's loot is generated and the
        // lootable flag is set, so this is the earliest the corpse is really lootable. Every such
        // corpse is queued; the pet is only sent off when it is not already on its way somewhere.
        void OnPlayerCreatureKillCredit(Player* player, Creature* killed) override
        {
            // The config is read once for the whole decision: the fetch pass must not see a reload land
            // between its gate and its walk.
            std::shared_ptr<Config const> cfg = ReadConfig();

            if (!canFetch(player, killed, *cfg))
                return;

            Creature* pet = GetCompanion(player);

            std::lock_guard<std::recursive_mutex> lock(g_fetchMutex);

            auto inserted = g_fetch.try_emplace(player->GetGUID());
            if (inserted.second)
            {
                g_fetchCount.fetch_add(1, std::memory_order_release);
                inserted.first->second.nextCheckMs = g_nowMs.load(std::memory_order_relaxed);
            }

            Fetch& fetch = inserted.first->second;
            fetch.queue.push_back(killed->GetGUID());

            // A resummon mid-queue: the old GUID is stale, this one is not. With no companion the
            // queue is left petless, and a summon picks it up on the next player tick. The grace clock
            // is re-anchored to this kill so a corpse appended to an already waiting queue gets its
            // own window instead of inheriting one that may already be spent.
            if (pet)
                fetch.pet = pet->GetGUID();
            else
                fetch.startMs = g_nowMs.load(std::memory_order_relaxed);

            // A queue of one means the pet had nowhere to be, so this new corpse is its next stop.
            // Anything else is a kill that landed while it was already away: the queue takes it and
            // the update below walks it next, which is what chains a pull. A pet whose walk has run
            // out of time is not "already away" any more -- it is re-aimed here instead of being left
            // on a corpse it may not even be near. A corpse the player is too far from is queued
            // without a send: the pet stays on its follow and OnPlayerUpdate arms the walk once the
            // player has closed the gap. Only the head of a queue of one is ever aimed at here, so a
            // kill landing behind a corpse already waiting cannot leapfrog it.
            if (fetch.queue.size() == 1 || fetch.done(g_nowMs.load(std::memory_order_relaxed), cfg->reachMs, cfg->petlessMs))
            {
                bool const wasWalking = fetch.walking;

                fetch.startMs = g_nowMs.load(std::memory_order_relaxed);
                fetch.walking = false;
                // Re-aimed now, so this corpse is due on the next player tick rather than waiting out
                // a throttle window that was started by the corpse it just replaced.
                fetch.nextCheckMs = fetch.startMs;

                Creature* target = player->GetMap()->GetCreature(fetch.queue.front());
                if (pet && inSendRange(player, target, *cfg))
                {
                    fetch.walking = true;
                    SendToCorpse(pet, target, cfg->lootDistance);
                }
                else if (wasWalking)
                    ResumeFollow(pet, player);   // a timed-out walk is being abandoned, not left running
            }
        }

        // The cosmetic mimic. It never touches the combat system: the minipet is REACT_PASSIVE and
        // immune, and both the motion generator and HandleEmoteCommand only move and animate it, so no
        // damage, threat, aggro or combat state is ever involved.
        //
        // The 10 Hz gate is deliberately lock-free and stateless: each player is assigned one slot of
        // the current 100 ms window by its own GUID, so a fully enabled server takes this mutex ~10
        // times per second per player instead of once per map tick.
        void DriveMimic(Player* player, Config const& cfg, uint32 nowMs)
        {
            if (player->GetGUID().GetCounter() % (kMimicCheckMs / 10) !=
                (nowMs / 10) % (kMimicCheckMs / 10))
                return;

            std::lock_guard<std::recursive_mutex> lock(g_mimicMutex);

            auto      itr = g_mimic.find(player->GetGUID());
            Creature* pet = GetCompanion(player);

            // The same gates the rest of the module honours: no pet, a companion PetIds excludes, or a
            // disabled module all mean the pet behaves exactly as it did before this feature.
            if (!pet || !pet->IsAlive() || !companionAllowed(pet, cfg))
            {
                DropMimic(itr, player, nullptr);
                return;
            }

            Unit* target = player->GetSelectedUnit();
            if (target && (!target->IsAlive() || !target->IsInWorld() || target == pet ||
                           !player->IsWithinDistInMap(target, cfg.maxDistance > 0.0f ? cfg.maxDistance : 40.0f)))
                target = nullptr;

            // Nothing worth mimicking: restore the follow once, then forget the state so an idle player
            // costs nothing on later ticks. Never clobber a walk the loot fetch owns.
            if (!target)
            {
                DropMimic(itr, player, pet);
                return;
            }

            if (itr == g_mimic.end())
                itr = g_mimic.try_emplace(player->GetGUID()).first;

            Mimic& mimic = itr->second;

            if (mimic.target != target->GetGUID())
            {
                mimic.target  = target->GetGUID();
                mimic.engaged = false;   // a fresh target is approached from scratch
            }

            // A walking loot fetch owns the active motion slot; the mimic waits its turn.
            {
                std::lock_guard<std::recursive_mutex> lootLock(g_fetchMutex);
                auto fetchItr = g_fetch.find(player->GetGUID());
                if (fetchItr != g_fetch.end() && fetchItr->second.walking)
                    return;
            }

            // Two thresholds, not one: walk in at kMimicRelease, only call it "in position" at
            // kMimicRange. A single value makes the pet step in and out around the boundary forever.
            bool const inRange = pet->IsWithinDistInMap(target, mimic.engaged ? kMimicRelease : kMimicRange);

            // Walk over, but only once per approach: a MovePoint is re-issued only after the previous
            // spline stopped, and never faster than kMimicRetryMs, so a pet that cannot move at all
            // (UNIT_FLAG_DISABLE_MOVE, an unreachable spot) does not emit a packet every tick.
            if (!inRange)
            {
                mimic.engaged = false;

                if (!pet->HasUnitState(UNIT_STATE_ROAMING_MOVE) && nowMs - mimic.walkSentMs >= kMimicRetryMs)
                {
                    SendToMimic(pet, target);
                    mimic.walkSentMs = nowMs;
                }
                return;
            }

            mimic.engaged = true;

            // Turn to face the target before swinging, but only when it is genuinely looking away, so a
            // stationary fight sends no movement packets at all. GetRelativeAngle is normalised to
            // [0, 2pi), so facing either side of the wrap point has to be folded before comparing.
            float const off = pet->GetRelativeAngle(target->GetPositionX(), target->GetPositionY());
            if (std::min(off, 2.0f * std::numbers::pi_v<float> - off) > 0.25f)
                pet->SetFacingToObject(target);

            if (nowMs < mimic.swingMs)
                return;

            // Animation only: EMOTE_ONESHOT_ATTACK_UNARMED is a bare SMSG_EMOTE. A companion model
            // without an attack animation simply ignores it.
            pet->HandleEmoteCommand(EMOTE_ONESHOT_ATTACK_UNARMED);
            mimic.swingMs = nowMs + kMimicSwingMs;
        }

        // Drop the mimic state and put the pet back on its follow, unless the loot path owns the
        // motion slot or the pet is not there to be moved.
        void DropMimic(std::unordered_map<ObjectGuid, Mimic>::iterator itr, Player* player, Creature* pet)
        {
            if (itr == g_mimic.end())
                return;

            g_mimic.erase(itr);

            if (!pet)
                return;

            std::lock_guard<std::recursive_mutex> lootLock(g_fetchMutex);
            if (g_fetch.find(player->GetGUID()) == g_fetch.end())
                ResumeFollow(pet, player);
        }

        // A config reload that turns the feature off must also release the pets already parked on a
        // target, or they would stay there until the player logs out.
        void DropAllMimics()
        {
            std::lock_guard<std::recursive_mutex> lock(g_mimicMutex);

            for (auto itr = g_mimic.begin(); itr != g_mimic.end(); ++itr)
            {
                Player* player = ObjectAccessor::FindPlayer(itr->first);
                Creature* pet  = GetCompanion(player);
                if (!pet)
                    continue;

                std::lock_guard<std::recursive_mutex> lootLock(g_fetchMutex);
                if (g_fetch.find(itr->first) == g_fetch.end())
                    ResumeFollow(pet, player);
            }

            g_mimic.clear();
        }

        // Release one player's mimic state (logout, or a play-time restriction landing).
        void ClearMimic(Player* player)
        {
            std::lock_guard<std::recursive_mutex> lock(g_mimicMutex);
            auto itr = g_mimic.find(player->GetGUID());
            if (itr != g_mimic.end())
                DropMimic(itr, player, GetCompanion(player));
        }

        void OnPlayerUpdate(Player* player, uint32 /*p_time*/) override
        {
            if (!player)
                return;   // nothing to own a fetch; never dereferenced below

            uint32 const nowMs = g_nowMs.load(std::memory_order_relaxed);

            // The mimic runs before the fetch paths and outside their early-out: most players never
            // have a corpse queued, and the mimic is the whole point of the feature for them.
            if (g_mimicEnabled.load(std::memory_order_relaxed))
            {
                // Read once for the whole mimic decision. A play-time-restricted player is not
                // served by the loot path either, so the pet goes back on its follow.
                if (player->HasPlayerFlag(PLAYER_FLAGS_NO_PLAY_TIME) ||
                    player->HasPlayerFlag(PLAYER_FLAGS_PARTIAL_PLAY_TIME))
                    ClearMimic(player);
                else
                    DriveMimic(player, *ReadConfig(), nowMs);
            }

            // Cheap unlocked early-out: with no fetch in flight anywhere there is nothing left to
            // drive, so the common player tick never takes the mutex here.
            if (!g_fetchCount.load(std::memory_order_acquire))
                return;

            // One lock for the whole pass: find the fetch, then decide whether it is due. The throttle
            // comes first so a player whose fetch is not due -- restricted or not -- costs one hash
            // lookup and no config read, instead of serialising on the mutex at the map tick rate.
            std::lock_guard<std::recursive_mutex> lock(g_fetchMutex);

            auto itr = g_fetch.find(player->GetGUID());
            if (itr == g_fetch.end() || nowMs < itr->second.nextCheckMs)
                return;

            itr->second.nextCheckMs = nowMs + kCheckIntervalMs;

            // A player under play-time restriction must not receive loot, so a fetch that is still in
            // flight when the restriction lands is unwound here with a lock rather than a network
            // send: the companion is put back on its follow and the entry dropped.
            bool const restricted = player->HasPlayerFlag(PLAYER_FLAGS_NO_PLAY_TIME) ||
                                    player->HasPlayerFlag(PLAYER_FLAGS_PARTIAL_PLAY_TIME);
            if (restricted)
            {
                ResumeFollow(ObjectAccessor::GetCreature(*player, itr->second.pet), player);
                EraseFetch(itr);
                return;
            }

            // Read after the throttle, so a player whose fetch is not due yet does not even copy the
            // config snapshot.
            std::shared_ptr<Config const> cfg = ReadConfig();

            Fetch& fetch = itr->second;
            Creature* pet = fetch.pet ? ObjectAccessor::GetCreature(*player, fetch.pet) : nullptr;

            // Resolve the companion actually walking the queue. A petless queue is kept, not dropped:
            // the corpse was lootable while no companion (or an unlisted one, which never fetches) was
            // out, and it waits here until a summon adopts it. A summon that replaces the pet -- or a
            // swap for another listed companion -- adopts the fresh GUID the same way, because the
            // stale one resolves to nullptr above. Only a dead or unlisted companion drops the queue:
            // the corpses it never reached stay manually lootable.
            if (!pet || !pet->IsAlive() || !companionAllowed(pet, *cfg))
            {
                Creature* replacement = GetCompanion(player);

                if (replacement && replacement->IsAlive() && companionAllowed(replacement, *cfg))
                {
                    pet           = replacement;
                    fetch.pet     = pet->GetGUID();
                    fetch.startMs = nowMs;     // the new walk's clock, not the old pet's
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
                fetch.startMs = nowMs;

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
                if (!head || head->loot.isLooted() || fetch.done(nowMs, cfg->reachMs, cfg->petlessMs))
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
            if (corpse->loot.isLooted() || fetch.done(nowMs, cfg->reachMs, cfg->petlessMs) ||
                !hasFetchableItem(player, &corpse->loot, corpse->loot.sourceWorldObjectGUID, *cfg))
            {
                fetch.queue.erase(fetch.queue.begin());
                fetch.startMs = nowMs;
                fetch.walking = false;

                if (fetch.queue.empty())
                {
                    ResumeFollow(pet, player);
                    EraseFetch(itr);
                    return;
                }

                Creature* next = player->GetMap()->GetCreature(fetch.queue.front());
                fetch.walking = inSendRange(player, next, *cfg);
                if (fetch.walking)
                    SendToCorpse(pet, next, cfg->lootDistance);
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
                if (!inSendRange(player, corpse, *cfg))
                    return;

                fetch.walking = true;
                fetch.startMs = nowMs;
                SendToCorpse(pet, corpse, cfg->lootDistance);
                return;
            }

            if (!pet->IsWithinDistInMap(corpse, cfg->lootDistance))
            {
                // Stopped and still out of range: the walk was interrupted (evade, resummon), so it is
                // re-aimed here instead of waiting out a whole ReachTimeoutMs in place. A moving pet
                // is left to finish its own walk.
                if (!pet->HasUnitState(UNIT_STATE_ROAMING_MOVE))
                    SendToCorpse(pet, corpse, cfg->lootDistance);

                return;
            }

            // LootCorpse fires script hooks, and a hook that leaves loot behind can re-enter
            // OnPlayerCreatureKillCredit -- which mutates g_fetch. So the entry is re-looked-up from
            // its key afterwards, because a reference cannot be rebound and an erase/insert of the
            // same key would leave the old one dangling. Holding the mutex throughout keeps the queue
            // from being rewritten underneath this walk; std::recursive_mutex keeps the re-entry safe.
            ObjectGuid const playerGuid = player->GetGUID();
            LootCorpse(player, corpse, *cfg);

            itr = g_fetch.find(playerGuid);
            if (itr == g_fetch.end())
                return;   // a re-entrant kill, a logout or an expired play time finished the fetch

            pet = ObjectAccessor::GetCreature(*player, itr->second.pet);

            if (!corpse->loot.isLooted())
                return;   // something was left for the player; a later kill re-aims at this corpse

            if (itr->second.queue.empty() || itr->second.queue.front() != corpse->GetGUID())
                return;   // the re-entry queued something ahead of this corpse

            itr->second.queue.erase(itr->second.queue.begin());
            itr->second.startMs = nowMs;
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
            itr->second.walking = inSendRange(player, next, *cfg);

            if (itr->second.walking)
                SendToCorpse(pet, next, cfg->lootDistance);
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

            g_mimic.erase(player->GetGUID());   // a logging-out player owns no motion slot to restore
        }
    };
}

void AddSC_ManaLootPet()
{
    new ManaLootPetScript();
}
