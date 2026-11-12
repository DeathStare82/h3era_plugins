#pragma once
#include "framework.h"

namespace flylanding
{
// Addresses in the ERA executable (Heroes3 Complete/SoD 3.2 base, the one H3API and NH3API target).
// Function bodies were read in the homm3-decomp project (GOG Complete 4.0 build), whose findpath.obj
// sits 0x2A0 higher and search.obj 0x2D0 lower than ERA. NH3API anchors in the same objects
// (searchArray ctor/dtor 0x4B10D0/0x4B1140, getTravelTime 0x4B3C80, seedPosition 0x56B710) and
// H3API's getTerrainCost (0x4B1620) confirm both offsets. Startup checks re-verify them in the running exe.
namespace addr
{
constexpr UINT32 PUSH_POINT = 0x4B17D0;      // searchArray::pushPoint, __thiscall, 8 stack args (ret 20h)
constexpr UINT32 PUSH_POINT_SIZE = 0x88D;
constexpr UINT32 TEST_DIRECTIONS = 0x4B2060; // searchArray::testPossibleDirections, __thiscall, 9 stack args (ret 24h)
constexpr UINT32 TEST_DIRECTIONS_SIZE = 0xA94;
constexpr UINT32 SEED_POSITION = 0x56B710;   // searchArray::seedPosition
constexpr UINT32 SEED_POSITION_SIZE = 0x8EC;
constexpr UINT32 OBJECT_TRAITS_PTR = 0x660428; // pointer to the 232 x 16-byte adventure object trait rows
} // namespace addr

#pragma pack(push, 1)
// searchArray path cell, 30 bytes (homm3-decomp include/findpath.h, NH3API core/pathfinding.hpp).
struct PathCell
{
    UINT32 point;        // +00 type_point: x bits 0..9, y bits 16..25, z bits 26..29
    UINT32 flags;        // +04 bitfield, see Flag* below
    UINT32 lastPoint;    // +08
    UINT32 monster;      // +0C
    INT32 barrierValue;  // +10
    INT32 dangerValue;   // +14
    UINT16 cost;         // +18
    UINT16 adjustedCost; // +1A
    UINT16 moveLeft;     // +1C
};
#pragma pack(pop)
static_assert(sizeof(PathCell) == 0x1E, "pathCell must be 30 bytes");

// Bits 0..8 are identical in every known build.
constexpr UINT32 FLAG_VISITED = 1u << 0;
constexpr UINT32 FLAG_TRIGGER = 1u << 1;
constexpr UINT32 FLAG_IN_BOAT = 1u << 2;
constexpr UINT32 FLAG_FLYING = 1u << 4;
constexpr UINT32 FLAG_WATER_WALKING = 1u << 5;
constexpr UINT32 FLAG_DIMENSION_DOOR = 1u << 7;

// canStop ("the hero has landed on this cell") moved one bit up on PC: retail PC inserts a
// start-at-trigger flag at bit 9 (homm3-decomp), while the Dreamcast-derived layout (NH3API) keeps
// canStop at bit 9. The plugin detects which one the running exe uses before applying any rule.
constexpr int CAN_STOP_BIT_PC = 10;
constexpr int CAN_STOP_BIT_DREAMCAST = 9;

struct Settings
{
    BOOL enabled = TRUE;
    // Rule A: an object can be entered only from a cell where the hero has landed.
    BOOL landingRule = TRUE;
    // Rule A also covers cells over water (Water Walk / Fly over sea).
    BOOL landingRuleOnWater = TRUE;
    // Rule B: a flying hero who lands next to a wandering monster must fight it before visiting.
    BOOL guardRule = TRUE;
    // Apply both rules to AI searches too, not only to the human player's path.
    BOOL applyToAi = TRUE;
    // Monoliths, Subterranean Gates and Whirlpools may still be entered from mid-air.
    BOOL allowTeleportersFromAir = FALSE;
    // Treat a Border Gate whose key the owner already has as freely passable, like a friendly garrison.
    // Off by default: otherwise a keyed hero could still hover over the obstacles next to a gate sealed
    // by Quest Guards and pass through it from mid-air.
    BOOL openBorderGatesPassable = FALSE;
    // -1 = detect at runtime, 9 or 10 = force.
    int canStopBit = -1;

    void Load();
};

class FlyLandingRules : public IGamePatch
{
    FlyLandingRules();

    virtual void CreatePatches() override;

  public:
    static FlyLandingRules &Get();
    static Settings &Config();
};
} // namespace flylanding
