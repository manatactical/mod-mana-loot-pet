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
| skinning / pickpocketing loot | never taken -- separate windows on the same corpse |
| a second kill while the pet is already fetching | queued behind the corpse it is walking to, looted one by one |
| pet dies, despawns, is dismissed, or gets stuck | the queue survives a resummon; the corpse it cannot reach is dropped and the next one is tried |
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
- The loot pass mirrors the core's own single-player path (`HandleAutostoreLootItemOpcode` and
  `HandleLootMoneyOpcode`): `Loot::FillNotNormalLootFor`, `Player::StoreLootItem`, `Loot::gold` and
  the money achievement. Reusing `Player::StoreLootItem` keeps its own guards -- master-loot
  protection, roll winners, blocked and not-allowed items -- so nothing is taken that the player
  could not have taken by hand; an item the core would refuse outright is left on the corpse, while
  one that only failed to fit is mailed. The money and item hooks the core raises
  (`OnPlayerBeforeLootMoney`, `OnPlayerAfterCreatureLootMoney`, `OnLootMoney`, and
  `OnPlayerAfterCreatureLoot` per stored item) are raised here too, so other modules see a companion
  loot the same way they see a hand loot.
- The pet only walks to a corpse it can actually help itself to: a corpse holding nothing but the
  loot this module deliberately leaves behind is passed over and the next queued corpse is walked
  instead, so a leftover never stalls the queue or parks the companion beside it. Nothing is taken
  from that corpse, so the player's hand-loot is unchanged -- but nothing is delivered from it
  either, so an item the player may now have room for waits for their own hand-loot.
- An item that cannot be stored is mailed through `Player::SendItemRetrievalMail`, the same call the
  core uses for a looted container's overflow. Only plain `loot->items` entries are handled that way;
  quest and conditional entries are left for the player, because their per-player slot bookkeeping is
  not worth undoing by hand.
- When the corpse empties it is cleaned up exactly as a hand-looted corpse is: the lootable flag is
  cleared and its value update forced, `Creature::AllLootRemovedFromCorpse` runs (skinnable flag,
  decay timer), and the loot is cleared.

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
- **Overflow mail diverges from the core.** A corpse's un-storable item is mailed (`SendItemRetrievalMail`),
  where the core mails only an item container's overflow. The mail is a plain stack, so a random
  suffix is lost, and a free-for-all slot is left on the corpse rather than consumed for every looter.
- **Already-looted items on a partially looted corpse** still generate their equip error internally
  in `StoreLootItem` before the module can mail them. A corpse the player started by hand and then
  left with a full bag can therefore print one error line per such slot.
- **A reward-less kill is not fetched.** The gate is `UNIT_DYNFLAG_LOOTABLE`, so a corpse the core
  decided may not be looted (insufficient damage, no reward) is left alone -- which is the point.
- **A corpse the companion passes over is left exactly as it is**: its lootable flag and decay are
  untouched, so the player's hand-loot is what the core expects. The follow-up error that a
  full-bag item would otherwise print when the pet stands beside a corpse it cannot finish is
  therefore avoided -- but so is taking it, unless a later kill brings the pet back.
- **The queue is not saved across a restart or a long disconnect**, and there is no cap on its
  length: a pull that kills fifty creatures queues fifty corpses, all walked in kill order.
- **A config reload does not move a fetch that is already in flight**, and the pet's decision to
  skip a corpse is taken with the pet stopped, so a kill landing mid-walk re-aims it at whatever
  dropped there rather than re-evaluating the corpse it was heading for.

## Configuration

| key | default | meaning |
|---|---|---|
| `ManaLootPet.Enable` | 1 | master switch |
| `ManaLootPet.LootDistance` | 3.0 | yards the pet must close before the loot is taken |
| `ManaLootPet.ReachTimeoutMs` | 8000 | how long the pet may walk to **one** corpse before that corpse is dropped (the rest of the queue is kept) |
| `ManaLootPet.LootMoney` | 1 | take the corpse's money |
| `ManaLootPet.LootItems` | 1 | take the corpse's items |
| `ManaLootPet.MailWhenBagsFull` | 1 | mail an item that no longer fits |
| `ManaLootPet.MaxItemsPerCorpse` | 20 | most slots taken **or mailed** from one corpse in one fetch |

The `.conf.dist` carries the full comments; the same defaults live in the code, so the module runs
with the key absent.

## Build

Standard AzerothCore module: drop the folder into `modules/` and build the worldserver. No extra
dependencies, no SQL.