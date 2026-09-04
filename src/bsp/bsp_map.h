#ifndef MEGALDOOM_BSP_MAP_H
#define MEGALDOOM_BSP_MAP_H

#include <genesis.h>
#include "generated_map_limits.h"

#define BSP_KEY_NONE 0x00u
#define BSP_KEY_BLUE 0x01u
#define BSP_KEY_YELLOW 0x02u
#define BSP_KEY_RED 0x04u
#define BSP_KEY_ALL (BSP_KEY_BLUE | BSP_KEY_YELLOW | BSP_KEY_RED)

#define BSP_DOOR_GROUP_NONE 0xFFu
#define BSP_MAX_DOORS 64

// Hand-authored test level for the BSP engine. Coordinates are in world
// fixed-point units (FX_ONE = 256 units per "cell"). This is now the single
// source of truth for geometry, collision, doors and the exit switch — the old
// grid world_map is gone.

typedef struct {
    s16 x;
    s16 y;
} BspVertex;

typedef enum {
    BSP_SEG_WALL = 0,
    BSP_SEG_DOOR = 1,
    BSP_SEG_EXIT = 2,
    BSP_SEG_SWITCH = 3,
    BSP_SEG_TRIGGER = 4,
    // A wall with a see-through band: Doom's windows. Still fully solid for
    // collision and line of sight -- only rendering differs. See
    // window_recess() in tools/doom_map.py for how one is recognised.
    BSP_SEG_WINDOW = 5,
    // A one-sided parapet bounding an F_SKY1 sector SHORTER than
    // RAY_WORLD_WALL_HEIGHT. This engine has no per-wall height, so these
    // would otherwise render as a full-height opaque wall and leave almost no
    // sky visible from inside the sector they bound. A taller sky sector --
    // an open-air courtyard -- is deliberately NOT one of these: the slab is
    // already no taller than its real walls, and blanking the buildings
    // standing in it left their windows hanging in mid-air. Fully solid for
    // collision and line of sight, same as a plain WALL -- only rendering
    // differs (see bsp_draw_seg). See sky_wall_sector() in tools/doom_map.py
    // for how one is recognised.
    BSP_SEG_SKY_WALL = 6
} BspSegType;

#define BSP_SEG_FLAG_DIRECT_USE 0x01u
#define BSP_SEG_FLAG_PLAIN_DOOR 0x02u

// One-sided wall segment. (nx, ny) is the front-facing normal pointing into the
// room interior (-1/0/1 for the axis-aligned test walls); the seg is drawn and
// collides only when it is closed. A DOOR seg toggles open/closed; when open it
// is skipped for both rendering and collision (a passable gap).
typedef struct {
    u16 v1;
    u16 v2;
    s16 nx;
    s16 ny;
    s16 tex_u_offset;
    u8 tex_v_offset;
    u8 texture_id; // exact generated shared-map texture ID
    u8 type;       // BspSegType
    // For a BSP_SEG_WINDOW these two bytes instead carry the see-through band
    // as Q8 fractions of the drawn slab, measured from its top
    // (bsp_seg_window_band_top/_bottom below). A window is never a door and
    // never locked, so neither field has a meaning to lose -- and keeping
    // BspSeg at exactly 16 bytes keeps its address calculation a shift instead
    // of the MULU.W an 18-byte record would force on the renderer's hottest
    // array. Widen this struct only with that cost in mind.
    u8 door_group; // shared state for every face of one physical door
    u8 required_key; // BSP_KEY_* bit, or BSP_KEY_NONE
    u8 flags;      // BSP_SEG_FLAG_* interaction and visual metadata
} BspSeg;

_Static_assert(sizeof(BspSeg) == 16,
               "BspSeg must stay 16 bytes for shift-only indexing");

typedef struct {
    u16 first_seg;
    u16 seg_count;
} BspSubsector;

typedef struct {
    s16 min_x;
    s16 min_y;
    s16 max_x;
    s16 max_y;
} BspBox;

// Rounded up to 32 bytes on purpose. The natural layout is 28, and every node
// visit then indexes bsp_nodes with a MULU.W #28 (~70 cycles) plus a shift/add
// chain for the same index; at 32 the whole address calculation collapses to
// one LSL.L #5. The 4 bytes/node cost ROM (bsp_nodes is const), not the 64KB
// work RAM: 236 E1M1 nodes => 944 bytes of a 1.4MB cartridge. Expressed as an
// alignment rather than a pad member so the generated map files keep their
// positional initializers without tripping -Wmissing-field-initializers.
typedef struct {
    s16 px;
    s16 py;
    s16 dx;
    s16 dy;
    BspBox front_box;
    BspBox back_box;
    u16 front; // child encoding (see BSP_CHILD_* below)
    u16 back;
} __attribute__((aligned(32))) BspNode;

// A raw Doom THING converted to engine coordinates. Runtime billboard spawning
// owns the curated type mapping; keeping raw type/flags here lets the generated
// map remain a faithful, compact source of level entities.
typedef struct {
    s16 x;
    s16 y;
    u16 type;
    u16 angle;
    u16 flags;
} BspThing;

// Every generated level is immutable cartridge data. Runtime systems select
// one descriptor and keep their mutable door/entity/stat state in work RAM.
typedef struct {
    const BspVertex *vertices;
    const BspSeg *segs;
    const u16 *seg_wall_len;
    const BspSubsector *subsectors;
    const u16 *subsector_sector;
    const BspNode *nodes;
    const BspThing *things;
    const u16 *grid_cell_offsets;
    const u16 *grid_seg_indices;
    const u8 *secret_sector_bits;
    // Per-sector bit: this sector's Doom ceiling flat is F_SKY1. Baked from the
    // flat NAME only -- no sector height ever enters the ROM. The renderer uses
    // it to swap the flat ceiling table for the baked sky horizon.
    const u8 *sky_sector_bits;
    u16 root_node;
    u16 seg_count;
    u16 vertex_count;
    u16 subsector_count;
    u16 node_count;
    u16 door_count;
    u16 thing_count;
    u16 sector_count;
    u16 secret_count;
    s16 grid_min_x;
    s16 grid_min_y;
    u16 grid_width;
    u16 grid_height;
    s16 map_min_x;
    s16 map_min_y;
    s16 map_max_x;
    s16 map_max_y;
    s32 player_start_x;
    s32 player_start_y;
    u16 player_start_angle;
} BspMapData;

// Child encoding (Doom convention): high bit set => subsector leaf, low bits =
// index into the corresponding array.
#define BSP_CHILD_LEAF_BIT 0x8000u
#define BSP_CHILD_INDEX(c) ((u16)((c) & 0x7FFFu))
#define BSP_CHILD_IS_SUBSECTOR(c) (((c) & BSP_CHILD_LEAF_BIT) != 0)

// Result of a "use" (B button) action in front of the player, consumed by the HUD.
typedef enum {
    DOOR_ACTION_NONE = 0,
    DOOR_ACTION_TOGGLED = 1,
    DOOR_ACTION_LOCKED = 2,
    DOOR_ACTION_UNLOCKED = 3,
    DOOR_ACTION_EXIT = 4
} DoorActionResult;

typedef struct {
    DoorActionResult action;
    u8 required_key;
} BspUseResult;

// Define BSP_USE_HAND_MAP to compile the hand-authored two-room test map
// (src/bsp_map_test.c) instead of the imported E1M1 geometry
// (src/generated_e1m1_map.c). Default: E1M1.
// #define BSP_USE_HAND_MAP

// Cache bounds are generated from the selected cartridge maps rather than
// retaining E1M1-era headroom in scarce 64 KB work RAM.
#define BSP_MAX_SEGS MEGALDOOM_MAP_MAX_SEGS
#define BSP_MAX_VERTICES MEGALDOOM_MAP_MAX_VERTICES
#define BSP_MAX_SUBSECTORS MEGALDOOM_MAP_MAX_SUBSECTORS
#define BSP_MAX_NODES MEGALDOOM_MAP_MAX_NODES

extern const BspMapData g_e1m1_map;
extern const BspMapData g_e1m2_map;
extern const BspMapData *g_bsp_map;

bool bsp_select_map(u16 level_index);
const BspMapData *bsp_current_map(void);

// Compatibility aliases keep the renderer/collision hot paths readable while
// making every access resolve through the selected ROM descriptor.
#define bsp_vertices (g_bsp_map->vertices)
#define bsp_segs (g_bsp_map->segs)
#define bsp_seg_wall_len (g_bsp_map->seg_wall_len)
#define bsp_subsectors (g_bsp_map->subsectors)
#define bsp_subsector_sector (g_bsp_map->subsector_sector)
#define bsp_nodes (g_bsp_map->nodes)
#define bsp_things (g_bsp_map->things)
#define bsp_root_node (g_bsp_map->root_node)
#define bsp_seg_count (g_bsp_map->seg_count)
#define bsp_vertex_count (g_bsp_map->vertex_count)
#define bsp_subsector_count (g_bsp_map->subsector_count)
#define bsp_node_count (g_bsp_map->node_count)
#define bsp_door_count (g_bsp_map->door_count)
#define bsp_thing_count (g_bsp_map->thing_count)

// Offline-generated 256-world-unit broad-phase grid. Each cell owns a slice
// [offsets[cell], offsets[cell+1]) of segment indices. Exact collision and LOS
// tests still run against those candidates, preserving map semantics.
#define BSP_GRID_CELL_SHIFT 8
#define BSP_GRID_CELL_SIZE (1 << BSP_GRID_CELL_SHIFT)
#define bsp_grid_min_x (g_bsp_map->grid_min_x)
#define bsp_grid_min_y (g_bsp_map->grid_min_y)
#define bsp_grid_width (g_bsp_map->grid_width)
#define bsp_grid_height (g_bsp_map->grid_height)
#define bsp_grid_cell_offsets (g_bsp_map->grid_cell_offsets)
#define bsp_grid_seg_indices (g_bsp_map->grid_seg_indices)

// Exact generated vertex bounds. The renderer uses these once per frame to
// prove that every camera-relative coordinate fits a signed word before taking
// the native 68000 MULS.W path; cameras outside that envelope retain the exact
// 32-bit fallback.
#define bsp_map_min_x (g_bsp_map->map_min_x)
#define bsp_map_min_y (g_bsp_map->map_min_y)
#define bsp_map_max_x (g_bsp_map->map_max_x)
#define bsp_map_max_y (g_bsp_map->map_max_y)

// Player 1 start, supplied by the active map's data file.
#define bsp_player_start_x (g_bsp_map->player_start_x)
#define bsp_player_start_y (g_bsp_map->player_start_y)
#define bsp_player_start_angle (g_bsp_map->player_start_angle)

bool bsp_sector_is_secret(u16 sector_index);
bool bsp_sector_is_sky(u16 sector_index);

// Window band accessors, Q8 of the drawn slab from its top. Meaningful only
// for BSP_SEG_WINDOW; see the field-overload note on BspSeg.
static inline u8 bsp_seg_window_band_top(const BspSeg *seg) {
    return seg->door_group;
}
static inline u8 bsp_seg_window_band_bottom(const BspSeg *seg) {
    return seg->required_key;
}

// Select the level and reset its mutable state (doors start closed).
void bsp_map_reset(u16 phase_index);

// Runtime door state. Non-door segs are never "open".
bool bsp_seg_is_open(u16 seg_index);

// Door lift in Q8 units: 0 is fully closed and 256 is fully raised. Non-door
// segs return 0. All faces in one physical door group share the same value.
u16 bsp_seg_door_lift(u16 seg_index);

// Advance every moving door by elapsed_vblanks. A complete trip takes 16
// vblanks. Returns TRUE while any lift changed so the caller can rebuild the
// world view even when the player is stationary.
bool bsp_update_doors(u16 elapsed_vblanks);

// Monotonic revision for cached visibility. Changes whenever door state or the
// active map resets, so billboard LOS results never survive world-geometry changes.
u16 bsp_get_visibility_revision(void);

// Locate the authored Doom subsector containing a world-space point.
u16 bsp_find_subsector(s32 x, s32 y);

// Locate the leaf as above and report whether a radius around the point stays
// on the same side of every BSP partition on its root-to-leaf path. The proof
// uses a conservative L1 bound on each partition normal, so FALSE merely
// declines an optimization; TRUE means the whole footprint owns that leaf.
u16 bsp_find_subsector_with_margin(s32 x, s32 y, s32 radius, bool *contained);

// Collision: is a circle of the given radius at world (x, y) touching a solid
// (closed) wall segment? Used by player and enemy movement.
bool bsp_circle_blocked(s32 x, s32 y, s32 radius);

// Line-of-sight: does the segment (x0,y0)-(x1,y1) cross any solid wall?
bool bsp_segment_hits_wall(s32 x0, s32 y0, s32 x1, s32 y1);

// Explosion sight: only a proper wall crossing blocks the blast. Endpoint-only
// contact is ignored so a barrel placed against a wall cannot occlude itself.
bool bsp_segment_crosses_wall(s32 x0, s32 y0, s32 x1, s32 y1);

#if DEBUG_PERF
typedef enum {
    BSP_QUERY_PLAYER = 0,
    BSP_QUERY_ENEMY = 1
} BspDebugQueryOwner;
void bsp_debug_set_query_owner(BspDebugQueryOwner owner);
u32 bsp_get_debug_player_collision_subticks(void);
u32 bsp_get_debug_enemy_collision_subticks(void);
u32 bsp_get_debug_los_subticks(void);
u16 bsp_get_debug_collision_candidates(void);
u16 bsp_get_debug_los_candidates(void);
void bsp_debug_reset_query_stats(void);
#endif

// Interact with whatever door / exit switch is directly in front of the player.
// Keys are persistent bits: use never consumes owned_keys.
BspUseResult bsp_use_in_front(s32 x, s32 y, u16 angle, u8 owned_keys);

#endif
