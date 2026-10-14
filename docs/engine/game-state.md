# Game state

`XFILES.GAM` holds the initial game state. It is a NeoAccess database
like `XFILES.HDB` (see [../formats/hdb.md](../formats/hdb.md)), with its
own ID space: an ID in an action refers to the GAM or the HDB depending
on the field.

The English DVD GAM holds 4,045 objects:

| Class | Count | Role |
|---|---|---|
| `VCVariable` (`0x53`) | 919 | Named game variables |
| `VCEnabled` (`0x46`) | 3,115 | Whether an HDB object is active |
| `VCString` (`0x50`) | 11 | String values of string variables |

Saved games (`*.x`) probably store the same objects with their current
values; this is not yet checked.

## VCVariable

| Field | Content |
|---|---|
| string | Name, for example `bDoesClerkKnowNames` |
| u32 | Initial value |
| u32 | ID of the HDB object the variable belongs to, or 0 |
| u8 | Class ID of that owner |
| u8 | Type flags |

Type flags:

| Bits | Meaning |
|---|---|
| 0–1 | Type: 0 character, 1 integer, 2 boolean, 3 string (the value is the ID of a `VCString`) |
| 7 | Set on preferences, inventory items and constants such as `bTRUE` |

Examples:

| ID | Name | Value | Type flags |
|---|---|---|---|
| 316 | `cSubNode` (of node 301) | 65 (`A`) | 0 |
| 346 | `iCurrInventory` | 0 | 1 |
| 858 | `iUberXTrack` | 100 | 1 |
| 859 | `iUberParanoia` | 100 | 1 |
| 860 | `iUberLoss` | 100 | 1 |
| 605 | `bTRUE` | 1 | 130 |
| 347 | `Inventory_iBadge` | 1 | 129 |
| 983 | `Pref_cInventory (H=hide,V=visible)` | 72 (`H`) | 128 |
| 2622 | `IFL_scPhoneNumber` | 2621 | 3 |

The three "uber" variables track the player's choices through the game.

## VCEnabled

| Field | Content |
|---|---|
| u32 | ID of the HDB object |
| u8 | Initial state: 1 enabled, 0 disabled |
| u8 | Class ID of the HDB object |

The objects that can be turned on and off are media references (1,317),
interface items (845), conversations (332), moves (209), explorables
(159), characters (107) and icons.

## References from actions

In `XFILES.HDB`, action operands of kind 0 are GAM variable IDs: 9,306 of
9,313 resolve to a `VCVariable`. The targets of enable actions (type 3)
are GAM `VCEnabled` IDs: 5,261 of 5,265 resolve. See
[actions.md](actions.md).

## Tools

```sh
xftool hdb XFILES.GAM                                # objects per class
python3 -I tools/re/hdb_tree.py XFILES.GAM 316 0     # one variable
python3 -I tools/re/check_refs.py XFILES.HDB XFILES.GAM   # resolve action references
```
