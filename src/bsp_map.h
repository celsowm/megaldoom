#ifndef MEGALDOOM_BSP_MAP_H
#define MEGALDOOM_BSP_MAP_H

#include <genesis.h>

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
    BSP_SEG_LOCKED_DOOR = 2,
    BSP_SEG_EXIT = 3
} BspSegType;

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
} BspSeg;

typedef struct {
    u16 first_seg;
    u16 seg_count;
    u16 sector_id;
} BspSubsector;

typedef struct {
    u8 ceiling_color;
    u8 floor_color;
} BspSectorVisual;

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
    DOOR_ACTION_EXIT = 4,
    DOOR_ACTION_EXIT_LOCKED = 5
} DoorActionResult;

// Define BSP_USE_HAND_MAP to compile the hand-authored two-room test map
// (src/bsp_map_test.c) instead of the imported E1M1 geometry
// (src/generated_e1m1_map.c). Default: E1M1.
// #define BSP_USE_HAND_MAP

// Upper bound on solid segs across any map, for the door-state array.
#define BSP_MAX_SEGS 1024

extern const BspVertex bsp_vertices[];
extern const BspSeg bsp_segs[];
extern const BspSubsector bsp_subsectors[];
extern const BspSectorVisual bsp_sector_visuals[];
extern const BspNode bsp_nodes[];
extern const u16 bsp_root_node;
extern const u16 bsp_seg_count;
extern const u16 bsp_node_count;
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

// Upper bound on BSP nodes across any map, sizing the near/far order cache
// bit array in bsp_render.c. E1M1 uses 236; the hand map uses 1.
#define BSP_MAX_NODES 256

// Player 1 start, supplied by the active map's data file.
extern const s32 bsp_player_start_x;
extern const s32 bsp_player_start_y;
extern const u16 bsp_player_start_angle;

// Reset per-level mutable state (door open/closed). phase_index is kept for
// parity with the level flow; geometry is currently shared across phases.
void bsp_map_reset(u16 phase_index);

// Runtime door state. Non-door segs are never "open".
bool bsp_seg_is_open(u16 seg_index);

// Monotonic revision for cached visibility. Changes whenever door state or the
// active map resets, so billboard LOS results never survive world-geometry changes.
u16 bsp_get_visibility_revision(void);

// Collision: is a circle of the given radius at world (x, y) touching a solid
// (closed) wall segment? Used by player and enemy movement.
bool bsp_circle_blocked(s32 x, s32 y, s32 radius);

// Line-of-sight: does the segment (x0,y0)-(x1,y1) cross any solid wall?
bool bsp_segment_hits_wall(s32 x0, s32 y0, s32 x1, s32 y1);

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
DoorActionResult bsp_use_in_front(s32 x, s32 y, u16 angle, bool has_key, bool *consumed_key);

#endif
