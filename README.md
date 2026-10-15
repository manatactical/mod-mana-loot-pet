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
| skinning / pickpocketing loot | never taken -- separate windows on the same corpse |
| a second kill while the pet is already fetching | left manually lootable, one fetch at a time |
| pet dies, despawns, is dismissed, or gets stuck | fetch abandoned and the pet resumes following |
| bags full | item is mailed (default) or left lootable, per `MailWhenBagsFull` |
| player under a play-time restriction | not served, matching what the core's loot handlers refuse |

Nothing is ever sent to the client except the ordinary "You loot..." money notification, so the
client's loot window is untouched and the corpse looks exactly as it does when looted by hand.

## How it hangs together

- `PlayerScript::OnPlayerCreatureKillCredit` is raised once per creature, after the corpse's loot has
  been generated and the lootable dynamic flag has been set -- the earliest point at which the corpse
  is genuinely lootable.
- `Unit::GetCompanionPet()` (the `SUMMON_SLOT_MINIPET` summon behind `UNIT_FIELD_CRITTER`) is the
  vanity pet. A hunter/warlock combat pet is deliberately *not* used, so a pet already fighting is
  never yanked away mid-combat.
- The companion idles on a follow generator; the fetch replaces it with a point generator on the
  active motion slot and puts the follow back afterwards, using the same distance and angle the
  summon path uses (`PET_FOLLOW_DIST`, `MINI_PET_FOLLOW_ANGLE`).
- The loot pass mirrors the core's own single-player path (`HandleAutostoreLootItemOpcode` and
  `HandleLootMoneyOpcode`): `Loot::FillNotNormalLootFor`, `Player::StoreLootItem`, `Loot::gold` and
  the money achievement. Reusing `Player::StoreLootItem` keeps its own guards -- master-loot
  protection, roll winners, blocked and not-allowed items -- so nothing is taken that the player
  could not have taken by hand.
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

- **The item hooks do not fire.** `Player::StoreLootItem` raises `sScriptMgr->OnPlayerLootItem`, and
  the money path raises `OnPlayerAfterCreatureLootMoney` / `OnLootMoney`; this module takes the loot
  directly, so other modules listening for a *hand* loot never hear about a companion loot. Wiring
  them in means reproducing hook arguments the core builds anyway, so it waits for a module that
  actually needs the signal.
- **Already-looted items on a partially looted corpse** still generate their equip error internally
  in `StoreLootItem` before the module can mail them. A corpse the player started by hand and then
  left with a full bag can therefore print one error line per such slot.
- **A reward-less kill is not fetched.** The gate is `UNIT_DYNFLAG_LOOTABLE`, so a corpse the core
  decided may not be looted (insufficient damage, no reward) is left alone -- which is the point.
- **A kill during a fetch is not queued.** The second corpse stays manually lootable rather than
  being remembered for the pet to visit next.

## Configuration

| key | default | meaning |
|---|---|---|
| `ManaLootPet.Enable` | 1 | master switch |
| `ManaLootPet.LootDistance` | 3.0 | yards the pet must close before the loot is taken |
| `ManaLootPet.ReachTimeoutMs` | 8000 | how long a fetch may take before it is abandoned |
| `ManaLootPet.LootMoney` | 1 | take the corpse's money |
| `ManaLootPet.LootItems` | 1 | take the corpse's items |
| `ManaLootPet.MailWhenBagsFull` | 1 | mail an item that no longer fits |
| `ManaLootPet.MaxItemsPerCorpse` | 20 | most slots taken from one corpse in one fetch |

The `.conf.dist` carries the full comments; the same defaults live in the code, so the module runs
with the key absent.

## Build

Standard AzerothCore module: drop the folder into `modules/` and build the worldserver. No extra
dependencies, no SQL.