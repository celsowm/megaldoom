#ifndef MEGALDOOM_BSP_MAP_H
#define MEGALDOOM_BSP_MAP_H

#include <genesis.h>

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
    BSP_SEG_TRIGGER = 4
} BspSegType;

#define BSP_SEG_FLAG_DIRECT_USE 0x01u

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
    u8 texture_id; // exact generated E1M1 texture ID
    u8 type;       // BspSegType
    u8 door_group; // shared state for every face of one physical door
    u8 required_key; // BSP_KEY_* bit, or BSP_KEY_NONE
    u8 flags;      // BSP_SEG_FLAG_* interaction metadata
} BspSeg;

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

typedef struct {
    s16 px;
    s16 py;
    s16 dx;
    s16 dy;
    BspBox front_box;
    BspBox back_box;
    u16 front; // child encoding (see BSP_CHILD_* below)
    u16 back;
} BspNode;

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

// Upper bound on solid segs across any map, for the door-state array.
#define BSP_MAX_SEGS 2048

// Upper bound for the renderer's per-frame transformed-vertex cache. E1M1 has
// 467 vertices; keeping a little headroom retains the hand-map/test contract
// while making the RAM cost explicit and guarded.
#define BSP_MAX_VERTICES 512

// DEBUG_PERF's visible-subsector oracle keeps one bit for every leaf touched
// by the front-to-back cast. E1M1 has 237 leaves; retain a little map-growth
// headroom just as the vertex/node limits above do.
#define BSP_MAX_SUBSECTORS 256

extern const BspVertex bsp_vertices[];
extern const BspSeg bsp_segs[];
extern const BspSubsector bsp_subsectors[];
extern const u16 bsp_subsector_sector[];
extern const BspNode bsp_nodes[];
extern const u16 bsp_root_node;
extern const u16 bsp_seg_count;
extern const u16 bsp_vertex_count;
extern const u16 bsp_subsector_count;
extern const u16 bsp_node_count;
extern const u16 bsp_door_count;
extern const BspThing bsp_things[];
extern const u16 bsp_thing_count;

// Compact parallel ROM table: precomputed horizontal wall length
// (|bx-ax| + |by-ay|) per seg, so the renderer avoids two vertex lookups and
// two abs calls per seg visit. Camera-independent, so it belongs in ROM.
extern const u16 bsp_seg_wall_len[];

// Offline-generated 256-world-unit broad-phase grid. Each cell owns a slice
// [offsets[cell], offsets[cell+1]) of segment indices. Exact collision and LOS
// tests still run against those candidates, preserving map semantics.
#define BSP_GRID_CELL_SHIFT 8
#define BSP_GRID_CELL_SIZE (1 << BSP_GRID_CELL_SHIFT)
extern const s16 bsp_grid_min_x;
extern const s16 bsp_grid_min_y;
extern const u16 bsp_grid_width;
extern const u16 bsp_grid_height;
extern const u16 bsp_grid_cell_offsets[];
extern const u16 bsp_grid_seg_indices[];

// Exact generated vertex bounds. The renderer uses these once per frame to
// prove that every camera-relative coordinate fits a signed word before taking
// the native 68000 MULS.W path; cameras outside that envelope retain the exact
// 32-bit fallback.
extern const s16 bsp_map_min_x;
extern const s16 bsp_map_min_y;
extern const s16 bsp_map_max_x;
extern const s16 bsp_map_max_y;

// Upper bound on BSP nodes across any map, sizing the near/far order cache
// bit array in bsp_render.c. E1M1 uses 236; the hand map uses 1.
#define BSP_MAX_NODES 640

// Player 1 start, supplied by the active map's data file.
extern const s32 bsp_player_start_x;
extern const s32 bsp_player_start_y;
extern const u16 bsp_player_start_angle;

// Reset per-level mutable state (door open/closed). phase_index is kept for
// parity with the level flow; geometry is currently shared across phases.
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
