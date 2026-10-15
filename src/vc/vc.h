/* VirtualCinema game model: typed views of the objects in XFILES.HDB
 * (see docs/engine/scene-graph.md). */
#ifndef XF_VC_H
#define XF_VC_H

#include "data/neo.h"

/* Class IDs from 0x498060 vc_register_classes. */
enum {
    VC_TITLE = 0x27,
    VC_NODE = 0x28,
    VC_LOCATON = 0x29,
    VC_VIEWPOINT = 0x2A,
    VC_VIEW = 0x2B,
    VC_NAME = 0x2F,
    VC_NAV = 0x33,
    VC_NAV_LIST = 0x34,
    VC_ASSET_REF = 0x35,
    VC_ASSET_REF_LIST = 0x36,
    VC_EXPLORABLE = 0x37,
    VC_EXPLORABLE_LIST = 0x38,
    VC_HOTSPOT = 0x39,
    VC_CHAR_VIEW = 0x2D,
    VC_CONVERSATION = 0x31,
    VC_ACTION = 0x41,
    VC_ACTION_ICON = 0x47,
    VC_EMOTION_ICON = 0x48,
    VC_INVENTORY = 0x4B,
    VC_IDEA_MAP = 0x4C,
    VC_IFACE_LAYOUT = 0x4E,
    VC_IFACE_ITEM = 0x4F,
    VC_ACTION_LIST = 0x42,
    VC_TRIGGER = 0x51,
    VC_TRIGGER_LIST = 0x52,
};

typedef struct {
    i16 left, top, right, bottom;
    u32 cursor; /* VCCursor id */
} vc_region;

typedef struct {
    u32 direction;  /* 1, 3, 5, 7, or a name id */
    u32 assets;     /* VCAssetRefList */
    u32 characters; /* VCCharViewList */
    u32 navs;       /* VCNav_List */
    u32 explorables;/* VCExplorableList */
    u32 triggers;   /* VCTriggerList */
} vc_view;

typedef struct {
    u32 target;  /* VCView */
    u32 hotspot; /* VCHotSpot */
    u32 extra;
} vc_nav;

typedef struct {
    vc_region region;
    u32 extra;
} vc_hotspot;

#define VC_MAX_DISC_ENTRIES 8
#define VC_PATH_MAX 128

typedef struct {
    int disc;
    char path[VC_PATH_MAX]; /* shipped file, '/' separators, e.g. "XV/19820.xmv" */
    u32 start, end;
} vc_media_entry;

typedef struct {
    char logical[VC_PATH_MAX];
    u32 name;     /* VCName id */
    u8 flags;     /* top byte of the packed field */
    u32 triggers; /* trigger list run when the media starts (8) and ends (9) */
    u8 kind;   /* 3 video, 9 navigation loop, 67 navigation still */
    u8 extra;
    int entry_count;
    vc_media_entry entries[VC_MAX_DISC_ENTRIES];
} vc_asset_ref;

int vc_class(const xf_neo_db *db, u32 id);
int vc_read_view(const xf_neo_db *db, u32 id, vc_view *v);
int vc_read_nav(const xf_neo_db *db, u32 id, vc_nav *n);
int vc_read_hotspot(const xf_neo_db *db, u32 id, vc_hotspot *h);
int vc_read_asset_ref(const xf_neo_db *db, u32 id, vc_asset_ref *a);
/* Copies the first string of a VCName into `out`. */
/* VCInterfaceItem: a region of an interface layout with its media and
 * triggers. `order` is the low byte of the last field: items with a
 * higher value are in front. */
typedef struct {
    vc_region region;
    u32 assets;   /* VCAssetRefList */
    u32 triggers; /* VCTriggerList */
    u16 flags;
} vc_iface_item;

/* VCCharView: a character standing in a view. */
typedef struct {
    u32 character;
    vc_region region;
    u32 conversations, idea_maps, assets, triggers;
} vc_char_view;

/* VCConversation: one question the player can ask a character. Fields
 * in record order (0x5F6720 VCConversation_read). */
typedef struct {
    u32 name;     /* VCName: the question as text */
    u32 icons;    /* VCAssetRefList: icon in the dialogue box */
    u32 answer;   /* VCAssetRefList: clip played after the question */
    u32 triggers; /* VCTriggerList */
    u32 question; /* VCAssetRefList: Willmore asking */
    u32 repeat;   /* VCAssetRefList: answer when asked again, or 0 */
    u8 flag;
} vc_conversation;

int vc_read_conversation(const xf_neo_db *db, u32 id, vc_conversation *c);
int vc_read_iface_item(const xf_neo_db *db, u32 id, vc_iface_item *it);
int vc_read_char_view(const xf_neo_db *db, u32 id, vc_char_view *c);
int vc_read_name(const xf_neo_db *db, u32 id, char *out, size_t size);
/* Reads the IDs of a list object (any class whose first field is a list).
 * Returns the number of IDs written, up to `max`, or -1. */
int vc_read_list(const xf_neo_db *db, u32 id, u32 *ids, int max);

/* Where a view sits in the hierarchy, and the trigger lists of each level. */
typedef struct {
    u32 view, viewpoint, location, node;
    u32 viewpoint_triggers, location_triggers, node_triggers;
} vc_place;

/* Builds the map from views to their viewpoint, location and node. */
typedef struct {
    vc_place *places; /* sorted by view */
    u32 count;
} vc_place_map;

int vc_place_map_build(const xf_neo_db *db, vc_place_map *m);
void vc_place_map_free(vc_place_map *m);
const vc_place *vc_place_of(const vc_place_map *m, u32 view);

/* Parses a packed media description (see the scene graph page). */
int vc_parse_media(const u8 *data, size_t size, vc_asset_ref *a);

#endif
