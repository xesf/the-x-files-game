# HDB object database

`XFILES.HDB` holds the whole game model: places, viewpoints, hotspots,
actions, triggers, conversations, variables and the names of media files.
It is a NeoAccess database. NeoAccess was a commercial object database
for the Macintosh and Windows, so all numbers are big-endian.

There are two versions of the file: one shared by every CD language and
one for the DVD release. Both hold 66,168 objects.

The engine finds objects through the database's own index, the same way
the original does: by object ID. The reader is `src/data/neo.c`.

## Header

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | Database version: 5 |
| 4 | 4 | Allocation mark (end of used space) |
| 8 | 4 | File offset of the class index root |
| 12 | 4 | Number of classes in use |
| 20 | 4 | File format: `0x501` |
| 24 | 2 | 4 |
| 28 | 4 | Node size: 256 |

The rest of the first page is not used by the reader.

## Index nodes

The class index, the per-class ID indexes and object lists all use the
same node layout:

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | Node flags. Bit 7 set means a leaf. |
| 1 | 5 | Not used by the reader |
| 6 | 2 | `count`: number of entries |
| 8 | … | Entries |

A branch node (bit 7 clear) holds `count` 4-byte file offsets of child
nodes. Leaf entries depend on the tree.

### Class index

Start at the offset in the header field at 8. Each leaf entry describes
one class:

| Offset | Size | Field |
|---|---|---|
| 0 | 10 | Class data, not used by the reader |
| 10 | 1 | `n`: number of indexes of this class |
| 11 | 16 | Not used by the reader |
| 27 | 16 × `n` | Index descriptions |

Each index description:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | Index type. 7 is the ID index. |
| 4 | 4 | File offset of the index root, 0 when the class has no objects |
| 8 | 4 | Class ID |
| 12 | 4 | Not used by the reader |

### ID index

Leaf entries are 8 bytes: the file offset of the object record, then the
object ID. Object IDs are unique across all classes, so an ID alone finds
an object and tells its class.

## Object records

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | Object flags |
| 2 | 4 | Object version, 1 in the shipped data |
| 6 | … | Class fields |

Records start on 8-byte boundaries. The class fields have no tags or
lengths: each class reads its fields in a fixed order, so the order of
the reads in the engine is the file format. See
[../engine/classes.md](../engine/classes.md) for the classes.

Field types:

| Type | Size | Content |
|---|---|---|
| Number | 1, 2 or 4 | Big-endian integer |
| Object ID | 4 | ID of another object |
| String or blob | 8 | File offset and length of the data. The data is stored elsewhere, often right after the record. Strings include their terminating zero. |
| List | 22 | An embedded list of object IDs (below) |

### Embedded lists

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | Number of IDs |
| 4 | 4 | File offset of the first list node, 0 when empty |
| 8 | 4 | Element type, 11 |
| 12 | 4 | Tag `null` |
| 16 | 4 | 1 |
| 20 | 1 | `0xFF` |
| 21 | 1 | 0 |

The list nodes use the index node layout. Leaf entries are 4-byte object
IDs, in list order. A list can hold objects of different classes.

In both HDB versions all 17,461 lists of the list classes hold exactly as
many IDs as their header says.

## Example

Object 24226 is a `VCActionList` (class `0x42`) at `0x1951A8`:

```
c2 31 00 00 00 01                  flags, version 1
00 00 00 05 00 19 51 d8            list: 5 IDs, first node at 0x1951D8
00 00 00 0b 6e 75 6c 6c 00 00 00 01 ff 00
```

The node at `0x1951D8` is a leaf with 5 IDs: 24227, 51985, 42470, 60543
and 67694, all `VCAction` objects.

## Tools

```sh
xftool hdb XFILES.HDB              # objects per class
xftool hdb-obj XFILES.HDB 24226 list   # one object and its list
xftool hdb-check XFILES.HDB        # read every list
python3 -I tools/re/hdb_index.py XFILES.HDB 42   # the same walk in Python
```

## Credits

The index layout follows the offline reader in the X-Files PC
enhancement patch. The class field orders were cross-checked with the
x-files-game decompilation project. See the README for both.
