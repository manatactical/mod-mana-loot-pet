# mod-mana-loot-pet

An AzerothCore module that sends a player's **vanity companion** to a fresh corpse to loot it.

Kill a creature solo while a non-combat pet is out: the pet is sent to a point beside the body and
the loot only moves once it has actually got there, so the fetch is visible instead of instant.
Money and items go straight to the player; an item that no longer fits is mailed by the Postmaster,
the same way a looted container's overflow already is.

## What it will and will not touch

| case | behaviour |
|---|---|
| corpse whose sole loot recipient is the killer | fetched by that player's companion |
| corpse tapped for a group (any group loot method) | left alone -- group rules and rolls are the group's |
| corpse tapped for another player | left alone |
| quest items, conditional drops, rolled items | left on the corpse for the player to hand-loot |
| a free-for-all item that no longer fits | left on the corpse, never mailed (mailing it would consume it for every looter) |
| an item this player was not allowed to read | left on the corpse, never mailed |
| a corpse whose only leftovers are the above | skipped, left exactly as it is, and it does not hold up the queue |
| a kill that dropped only money | fetched -- money counts as loot the pet can take |
| skinning / pickpocketing loot | never taken -- separate windows on the same corpse |
| a second kill while the pet is already fetching | queued behind the corpse it is walking to, looted one by one |
| corpse further than `MaxDistance` from the player | queued, not fetched; the pet stays on follow and is sent once the player closes in |
| pet dies, despawns, is dismissed, or gets stuck | the queue survives a resummon; the corpse it cannot reach is dropped and the next one is tried |
| a kill that lands with no companion out | queued for `QueueWithoutPetMs`; a companion summoned in that window walks it, otherwise the corpse stays for a hand-loot |
| `MimicCombat=1`, player has a target | pet stands beside it and plays an attack animation. Cosmetic only: no damage, no threat, no combat state, and it gives way to any loot fetch |
| bags full | item is mailed (default) or left lootable, per `MailWhenBagsFull` |
| player under a play-time restriction | not served, matching what the core's loot handlers refuse |

Nothing is ever sent to the client except the ordinary "You loot..." money notification, so the
client's loot window is untouched and the corpse looks exactly as it does when looted by hand.

## How it hangs together

- `PlayerScript::OnPlayerCreatureKillCredit` is raised once per creature, after the corpse's loot has
  been generated and the lootable dynamic flag has been set -- the earliest point at which the corpse
  is genuinely lootable. Every such corpse is appended to that player's queue, so a pull that kills
  several creatures is walked one corpse at a time rather than only the first.
- `Unit::GetCompanionPet()` (the `SUMMON_SLOT_MINIPET` summon behind `UNIT_FIELD_CRITTER`) is the
  vanity pet. A hunter/warlock combat pet is deliberately *not* used, so a pet already fighting is
  never yanked away mid-combat.
- The companion idles on a follow generator; the fetch replaces it with a point generator on the
  active motion slot and puts the follow back afterwards, using the same distance and angle the
  summon path uses (`PET_FOLLOW_DIST`, `MINI_PET_FOLLOW_ANGLE`). Arrival is not a handshake: the
  point generator clears `UNIT_STATE_ROAMING_MOVE` when it finishes, so "stopped and in range" is the
  pet standing on the corpse. A stop that is still out of range was interrupted (evade, resummon)
  and is re-aimed on the next tick.
- The per-player cost is bounded: `PlayerScript::OnPlayerUpdate` early-outs on a lock-free atomic when
  no fetch is live anywhere, and a live fetch is polled every 100 ms rather than at the map tick
  (`kCheckIntervalMs`), so a walking pet costs ~10 checks a second instead of ~100. The config is a
  snapshot swapped whole on reload, so reading it never takes the fetch lock; that lock is held only
  while a fetch is actually being advanced.
- The loot pass mirrors the core's own single-player path (`HandleAutostoreLootItemOpcode` and
  `HandleLootMoneyOpcode`): `Loot::FillNotNormalLootFor`, `Player::StoreLootItem`, `Loot::gold` and
  the money achievement. Reusing `Player::StoreLootItem` keeps its own guards -- master-loot
  protection, roll winners, blocked and not-allowed items -- so nothing is taken that the player
  could not have taken by hand; an item the core would refuse outright is left on the corpse, while
  one that only failed to fit is mailed. The money and item hooks the core raises
  (`OnPlayerBeforeLootMoney`, `OnPlayerAfterCreatureLootMoney`, `OnLootMoney`) are raised here too, and
  `OnPlayerAfterCreatureLoot` is raised once for the whole corpse -- as the core raises it, once per
  loot action, not once per item -- so other modules see a companion loot the same way they see a hand
  loot.
- Whether a slot can be taken is asked with `CanStoreNewItem` before it is taken rather than letting
  `StoreLootItem` refuse: the core's refusal sends the player a red inventory/unique error, which is
  wrong here because the refusal is not the player's action and an un-storable slot is mailed instead.
  A slot the pet was configured to leave behind, or one belonging to another looter, is left exactly
  as it was -- nothing is even asked about it -- so a corpse needing a later hand-loot is never drained
  by a retry loop.
- The pet only walks to a corpse it can actually help itself to: a corpse holding nothing but the
  loot this module deliberately leaves behind is passed over and the next queued corpse is walked
  instead, so a leftover never stalls the queue or parks the companion beside it. Money counts as
  loot the pet can take, so the common money-only kill is still fetched. Nothing is taken from a
  corpse that is skipped, so the player's hand-loot is unchanged -- but nothing is delivered from it
  either, so an item the player may now have room for waits for their own hand-loot.
- An item that cannot be stored is mailed by the Postmaster draft this module builds itself, in the
  core's own letter text. The whole corpse is passed in one batch, so its un-storable slots share a
  mail (`MAX_MAIL_ITEMS`, 12, per mail) instead of arriving one mail each, and the mail is built from
  an item instance -- the loot's random roll travels with it. Only plain `loot->items` entries are
  handled that way; quest and conditional entries are left for the player, because their per-player
  slot bookkeeping is not worth undoing by hand.
- When the corpse is settled it is cleaned up exactly as a hand-looted corpse is: the lootable flag is
  cleared and its value update forced, `Creature::AllLootRemovedFromCorpse` runs (skinnable flag,
  decay timer), and the loot is cleared. The gate is `Loot::isLooted()`, deliberately not
  `hasFetchableItem`: the latter asks what the *pet* may take, so a corpse holding only loot the
  module's own `LootMoney`/`LootItems` options excluded would have been torn down with that loot still
  on it. `isLooted()` is the core's own emptiness test -- `Loot::FillNotNormalLootFor` counts this
  player's quest, free-for-all and conditional slots into `unlootedCount`, so it stays false while
  anything is left to hand-loot.

## Limits

- Only a **sole** recipient is served. A grouped player's kills are not fetched at all, which keeps
  the module out of loot rolls entirely; a groups-aware version would have to reproduce the core's
  permission and roll decisions, which this deliberately does not.
- The companion loots, but it does not *carry*: the loot is the player's the moment it is taken.
- Skinning loot is not fetched. A skinnable corpse still needs the player's own skinning cast.

## Known gaps

These are deliberate for now, not defects:

- **The money hooks do fire, the item hooks do not.** The money path raises
  `OnPlayerBeforeLootMoney` / `OnPlayerAfterCreatureLootMoney` / `OnLootMoney` with the amounts the
  core's own handler would pass. `Player::StoreLootItem` raises `sScriptMgr->OnPlayerLootItem`, but
  this module takes items directly and never has the `Item*` the hook wants, so other modules
  listening for a *hand* loot still do not hear about a companion item loot.
- **Overflow mail diverges from the core.** A corpse's un-storable item is mailed from a draft this
  module builds (`MailOverflow`), where the core mails only an item container's overflow and does so
  through `SendItemRetrievalMail`. The draft is needed because that call takes `(entry, count)` and
  re-creates the stack, dropping the loot's random roll; `Item::CreateItem(entry, count, player,
  false, randomPropertyId)` -- the call the store path uses -- keeps it. A random *suffix's* amount is
  still re-rolled when the mail is read, because the item's suffix seed is not what gets persisted.
  A free-for-all slot is left on the corpse rather than consumed for every looter.
- **Already-looted items on a partially looted corpse** no longer print an error for every slot: the
  storable check runs before `StoreLootItem`, so a slot the pet cannot take is never handed to it to
  refuse. A corpse the player started by hand and left with a full bag is left for that player.
- **An un-storable slot whose mail cannot be built** (item template missing, DB error) is logged and
  dropped, and the corpse settles as if the pet had taken it: the slot was already marked looted, so
  there is nothing left to retry. `shortcut: the loss is server-log only, no client notice; upgrade if
  players report missing overflow mail with no explanation.`
- **A reward-less kill is not fetched.** The gate is `UNIT_DYNFLAG_LOOTABLE`, so a corpse the core
  decided may not be looted (insufficient damage, no reward) is left alone -- which is the point.
- **A corpse is settled on loot, not on the options.** `LootMoney=0` or `LootItems=0` stops the pet
  taking that kind of loot; it never tears the corpse down. The same holds for what the pet always
  leaves behind (quest tail, free-for-all, conditional drops): those keep the corpse lootable.
- **The queue is not saved across a restart or a long disconnect**, and there is no cap on its
  length: a pull that kills fifty creatures queues fifty corpses, all walked in kill order.
- **A config reload does not move a fetch that is already in flight**, and the pet's decision to
  skip a corpse is taken with the pet stopped, so a kill landing mid-walk re-aims it at whatever
  dropped there rather than re-evaluating the corpse it was heading for.

## Configuration

| key | default | meaning |
|---|---|---|
| `ManaLootPet.Enable` | 1 | master switch |
| `ManaLootPet.LootDistance` | 1.5 | yards the pet must close before the loot is taken (also caps how far away it aims) |
| `ManaLootPet.MaxDistance` | 40 | yards the player must be within before the pet is sent; a kill further away queues and waits for the player to close in (0 = no limit) |
| `ManaLootPet.ReachTimeoutMs` | 8000 | how long the pet may walk to **one** corpse before that corpse is dropped (the rest of the queue is kept) |
| `ManaLootPet.LootMoney` | 1 | take the corpse's money |
| `ManaLootPet.LootItems` | 1 | take the corpse's items |
| `ManaLootPet.MailWhenBagsFull` | 1 | mail an item that no longer fits |
| `ManaLootPet.MaxItemsPerCorpse` | 20 | most slots taken **or mailed** from one corpse in one fetch |
| `ManaLootPet.PetIds` | (empty) | comma-separated creature entries allowed to fetch. Empty = any companion. A narrowed list also aborts a fetch already in flight on reload. |
| `ManaLootPet.QueueWithoutPetMs` | 60000 | how long a kill waits for a companion to be summoned (0 = only fetch when a pet is already out) |
| `ManaLootPet.MimicCombat` | 0 | cosmetic: pet follows the player's target and plays an unarmed swing animation. No damage, threat or combat state. |

The `.conf.dist` carries the full comments; the same defaults live in the code, so the module runs
with the key absent.

## Build

Standard AzerothCore module: drop the folder into `modules/` and build the worldserver. No extra
dependencies, no SQL.