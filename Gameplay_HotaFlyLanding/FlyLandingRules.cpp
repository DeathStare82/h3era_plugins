#include "FlyLandingRules.h"

#include <cstdio>

// HotA-style landing rules for Fly / Water Walk on the adventure map.
//
// How vanilla decides (searchArray::testPossibleDirections, findpath.cpp in homm3-decomp):
//   for every neighbour `dest` of an expanded cell `source`
//     if (source.canStop || !destCell.isTrigger ||
//         (!traits[destCell.type].blocksLanding && destCell.type != TOWN))
//         pushPoint(source, dest, ..., isTrigger = destCell.isTrigger);
// so a hero hovering over rocks, trees or water (canStop == 0) may still enter every trigger whose
// blocksLanding flag is clear: mines, dwellings, Border Gates, banks, shrines and so on.
// Pickups, monsters, Quest Guards, heroes, events and towns are already refused from mid-air.
//
// Rule A (landing): refuse the remaining triggers too, unless the object is freely passable
//                   (a friendly garrison; optionally a Border Gate whose key the owner has).
// Rule B (guard):   a flying hero ignores the zone of control in pathfinding (seedPosition only
//                   restricts heroes that are not flying), and endMoveHero only makes the guard attack
//                   when the hero stops or moves onto a guarded cell at the end of the route. Objects
//                   visited from the adjacent cell (blocksLanding) go through handleStopOnTrigger,
//                   which checks no guard at all. Refuse entering such a trigger from a landed cell
//                   next to a wandering monster, so the hero must stop on that cell (and fight) first.
//                   The hero's own starting cell stays exempt, as it is for walking heroes.
//
// Both rules veto one pushPoint call: the one testPossibleDirections makes for the trigger cell itself.
// The second push vanilla makes for a flying hero (the airborne pass-over cell, isTrigger = 0) is kept,
// so heroes can still fly over object entrances. Cursor, route arrows and the AI all read the same
// search grid, so they follow the rules without further patches.

namespace flylanding
{
namespace
{
constexpr LPCSTR LOG_SOURCE = "HotaFlyLanding";
constexpr int NORMAL_SEARCH = 0; // type_search_type::const_normal_search, the human player's search

constexpr INT8 TERRAIN_WATER = 8;
constexpr INT8 TERRAIN_ROCK = 9;
constexpr UINT8 ACCESS_BLOCKED = 0x01;  // H3MapItem::access, NewmapCell::isBlocked
constexpr UINT8 ACCESS_ENTRANCE = 0x10; // H3MapItem::access, NewmapCell::isTrigger
constexpr UINT8 MIRROR_PASSABLE = 0x40; // H3MapItem::mirror bit 6, read by hero::canLand

Settings g_settings;
int g_canStopBit = -1; // -1 until the pathCell layout is known; rules stay off until then

// Valid only while testPossibleDirections runs; restored on exit so nesting stays safe.
struct DirectionsContext
{
    const PathCell *source = nullptr;
    const H3Hero *hero = nullptr;
    int searchType = NORMAL_SEARCH;
};
DirectionsContext g_context;

void Log(LPCSTR operation, LPCSTR text)
{
    if (Era::WriteLog)
        Era::WriteLog(LOG_SOURCE, operation, text);
}

inline int PosX(UINT32 p)
{
    return int(p & 0x3FF);
}
inline int PosY(UINT32 p)
{
    return int((p >> 16) & 0x3FF);
}
inline int PosZ(UINT32 p)
{
    return int((p >> 26) & 0xF);
}
inline bool SamePos(UINT32 a, UINT32 b)
{
    return ((a ^ b) & 0x3FFF03FFu) == 0;
}

H3MapItem *MapItemAt(int x, int y, int z)
{
    H3Main *game = H3Main::Get();
    const int size = game->mainSetup.mapSize;
    const int levels = game->mainSetup.hasUnderground ? 2 : 1;
    if (x < 0 || y < 0 || z < 0 || x >= size || y >= size || z >= levels)
        return nullptr;
    return &game->mainSetup.mapitems[(z * size + y) * size + x];
}

H3MapItem *MapItemAt(UINT32 p)
{
    return MapItemAt(PosX(p), PosY(p), PosZ(p));
}

// The game indexes this table with the cell type without a bound check, so do the same.
const H3GlobalObjectSettings &Traits(int type)
{
    return H3GlobalObjectSettings::Get()[type];
}

inline bool CanStop(const PathCell &cell)
{
    return ((cell.flags >> g_canStopBit) & 1) != 0;
}

bool SameTeam(int a, int b)
{
    if (a < 0 || b < 0 || a >= 8 || b >= 8)
        return false;
    if (a == b)
        return true;
    const H3Main *game = H3Main::Get();
    return game->mapInfo.playerTeam[a] == game->mapInfo.playerTeam[b];
}

bool IsTeleporter(int type)
{
    return type == eObject::MONOLITH_ONE_WAY_ENTRANCE || type == eObject::MONOLITH_TWO_WAY ||
           type == eObject::SUBTERRANEAN_GATE || type == eObject::WHIRLPOOL;
}

// HotA keeps "freely passable" objects out of the rule, e.g. a friendly garrison. A Border Gate the owner
// holds the key for is passable too (enter_trigger lets the search continue through both), but it is
// only exempted when open_border_gates_passable is set.
bool IsFreelyPassable(H3MapItem &target, int owner)
{
    switch (target.objectType)
    {
    case eObject::GARRISON:
    case eObject::GARRISON2: {
        const H3Garrison *garrison = target.garrison.Get();
        return garrison && SameTeam(garrison->owner, owner);
    }
    case eObject::BORDER_GATE: {
        const int color = target.objectSubtype;
        if (!g_settings.openBorderGatesPassable || color < 0 || color >= 8 || owner < 0 || owner >= 8)
            return false;
        return ((H3Main::Get()->keymasterVisited[color].bits >> owner) & 1) != 0;
    }
    default:
        return false;
    }
}

// Mirrors advManager::findAdjacentMonster (0x41A3C0 in ERA): a wandering monster on one of the eight
// neighbours, on the same kind of terrain (land/water), other than `excluded`. A cell that is itself an
// entrance not enterable from the north only looks at its own row and the row below.
bool IsGuardedLanding(UINT32 center, UINT32 excluded)
{
    const H3MapItem *centerItem = MapItemAt(center);
    if (!centerItem)
        return false;

    const int size = H3Main::Get()->mainSetup.mapSize;
    const int cx = PosX(center), cy = PosY(center), cz = PosZ(center);
    const bool centerIsWater = centerItem->land == TERRAIN_WATER;

    const int left = cx > 0 ? cx - 1 : 0;
    const int right = cx + 2 < size ? cx + 2 : size;
    int top = cy > 0 ? cy - 1 : 0;
    const int bottom = cy + 2 < size ? cy + 2 : size;
    if ((centerItem->access & ACCESS_ENTRANCE) && centerItem->objectType >= 0 &&
        !Traits(centerItem->objectType).exitTop)
        top = cy;

    for (int x = left; x < right; ++x)
    {
        for (int y = top; y < bottom; ++y)
        {
            const H3MapItem *item = MapItemAt(x, y, cz);
            if (!item || item->objectType != eObject::MONSTER || !(item->access & ACCESS_ENTRANCE))
                continue;
            if ((item->land == TERRAIN_WATER) != centerIsWater)
                continue;
            if (x == PosX(excluded) && y == PosY(excluded) && cz == PosZ(excluded))
                continue;
            return true;
        }
    }
    return false;
}

// Same test as hero::canLand, a little stricter (no entrance at all). Used only for layout detection.
bool IsPlainLand(UINT32 p)
{
    const H3MapItem *item = MapItemAt(p);
    return item && item->land != TERRAIN_WATER && item->land != TERRAIN_ROCK && !(item->access & ACCESS_BLOCKED) &&
           !(item->access & ACCESS_ENTRANCE) && (item->mirror & MIRROR_PASSABLE);
}

// seedPosition pushes the hero's starting cell onto itself before the search begins. In that cell the
// hero is standing still: without Fly/Water Walk, or on plain land, canStop is 1 in every build.
//   PC layout:        bit 9 = startAtTrigger (always cleared there), bit 10 = canStop
//   Dreamcast layout: bit 9 = canStop
void DetectCanStopBit(const PathCell &start)
{
    if (g_canStopBit >= 0)
        return;
    if ((start.flags & (FLAG_FLYING | FLAG_WATER_WALKING)) && !IsPlainLand(start.point))
        return;

    const bool bit9 = ((start.flags >> 9) & 1) != 0;
    const bool bit10 = ((start.flags >> 10) & 1) != 0;
    if (bit9)
        g_canStopBit = CAN_STOP_BIT_DREAMCAST;
    else if (bit10)
        g_canStopBit = CAN_STOP_BIT_PC;
    else
        return;

    char text[96];
    std::snprintf(text, sizeof(text), "pathCell.canStop detected at bit %d (start flags 0x%08X)", g_canStopBit,
                  start.flags);
    Log("Layout", text);
}

bool ShouldVeto(const PathCell &source, const PathCell &dest)
{
    if (g_canStopBit < 0)
        return false; // layout not known yet: behave like vanilla
    if (!g_settings.applyToAi && g_context.searchType != NORMAL_SEARCH)
        return false;
    if (source.flags & FLAG_IN_BOAT)
        return false;

    H3MapItem *target = MapItemAt(dest.point);
    if (!target)
        return false;

    const H3Hero *hero = g_context.hero;
    const int owner = hero ? hero->owner : -1;
    if (IsFreelyPassable(*target, owner))
        return false;

    if (!CanStop(source))
    {
        // Rule A: the hero is above an obstacle, water or unexplored cell.
        if (!g_settings.landingRule)
            return false;
        if (g_settings.allowTeleportersFromAir && IsTeleporter(target->objectType))
            return false;
        if (!g_settings.landingRuleOnWater)
        {
            const H3MapItem *from = MapItemAt(source.point);
            if (from && from->land == TERRAIN_WATER)
                return false;
        }
        return true;
    }

    // Rule B: the hero has landed next to a guard he flew past and would visit without moving: objects
    // with blocksLanding (pickups, monsters, heroes, Quest Guards...) are visited from the adjacent cell
    // through handleStopOnTrigger, which never checks guards. Objects the hero steps onto (mines, towns,
    // gates, garrisons) need nothing: endMoveHero checks the guard on their entrance cell.
    if (!g_settings.guardRule || !(source.flags & FLAG_FLYING))
        return false;
    if (target->objectType < 0 || !Traits(target->objectType).cannotEnter)
        return false;
    if (hero && SamePos(source.point, H3Position::Pack(hero->x, hero->y, hero->z)))
        return false;
    return IsGuardedLanding(source.point, dest.point);
}

void __stdcall PushPointHook(HiHook *hook, void *search, const PathCell *oldCell, PathCell *point, int direction,
                             int moveCost, int limit, INT32 barrierValue, UINT32 monster, int isTrigger)
{
    if (oldCell == point)
        DetectCanStopBit(*oldCell);
    else if (isTrigger && oldCell == g_context.source && ShouldVeto(*oldCell, *point))
        return;

    THISCALL_9(void, hook->GetDefaultFunc(), search, oldCell, point, direction, moveCost, limit, barrierValue,
               monster, isTrigger);
}

void __stdcall TestDirectionsHook(HiHook *hook, void *search, const H3Hero *hero, const PathCell *source,
                                  INT32 turnMobility, INT32 maxMobility, INT32 adjacentMonster,
                                  UINT32 monsterLocation, INT32 pathfinding, INT32 searchType, INT32 nativeTerrain)
{
    const DirectionsContext saved = g_context;
    g_context.source = source;
    g_context.hero = hero;
    g_context.searchType = searchType;

    THISCALL_10(void, hook->GetDefaultFunc(), search, hero, source, turnMobility, maxMobility, adjacentMonster,
                monsterLocation, pathfinding, searchType, nativeTerrain);

    g_context = saved;
}

int CountCallsTo(UINT32 start, UINT32 size, UINT32 target)
{
    const BYTE *code = reinterpret_cast<const BYTE *>(start);
    int count = 0;
    for (UINT32 i = 0; i + 5 <= size; ++i)
    {
        if (code[i] != 0xE8)
            continue;
        const INT32 rel = *reinterpret_cast<const INT32 *>(code + i + 1);
        if (start + i + 5 + UINT32(rel) == target)
            ++count;
    }
    return count;
}

int CountDwords(UINT32 start, UINT32 size, UINT32 value)
{
    const BYTE *code = reinterpret_cast<const BYTE *>(start);
    int count = 0;
    for (UINT32 i = 0; i + 4 <= size; ++i)
        if (*reinterpret_cast<const UINT32 *>(code + i) == value)
            ++count;
    return count;
}
} // namespace

void Settings::Load()
{
    const auto readBool = [](LPCSTR key, BOOL &target) {
        bool found = false;
        const int value = EraJS::readInt(key, found);
        if (found)
            target = value != 0;
    };
    readBool("hota_fly_landing.enabled", enabled);
    readBool("hota_fly_landing.landing_rule", landingRule);
    readBool("hota_fly_landing.landing_rule_on_water", landingRuleOnWater);
    readBool("hota_fly_landing.guard_rule", guardRule);
    readBool("hota_fly_landing.apply_to_ai", applyToAi);
    readBool("hota_fly_landing.allow_teleporters_from_air", allowTeleportersFromAir);
    readBool("hota_fly_landing.open_border_gates_passable", openBorderGatesPassable);

    bool found = false;
    const int bit = EraJS::readInt("hota_fly_landing.can_stop_bit", found);
    if (found && (bit == CAN_STOP_BIT_DREAMCAST || bit == CAN_STOP_BIT_PC))
        canStopBit = bit;
}

FlyLandingRules::FlyLandingRules() : IGamePatch("EraPlugin.HotaFlyLanding.Rules")
{
    CreatePatches();
}

FlyLandingRules &FlyLandingRules::Get()
{
    static auto *instance = new FlyLandingRules;
    return *instance;
}

Settings &FlyLandingRules::Config()
{
    return g_settings;
}

void FlyLandingRules::CreatePatches()
{
    if (m_isInited)
        return;
    m_isInited = true;

    // Make sure these are the pathfinder functions in the running exe before hooking them.
    // Expected in ERA: 3 / 1 / 1 / 3 (homm3-decomp counts for the same functions).
    const int directionPushes = CountCallsTo(addr::TEST_DIRECTIONS, addr::TEST_DIRECTIONS_SIZE, addr::PUSH_POINT);
    const int seedDirections = CountCallsTo(addr::SEED_POSITION, addr::SEED_POSITION_SIZE, addr::TEST_DIRECTIONS);
    const int seedPushes = CountCallsTo(addr::SEED_POSITION, addr::SEED_POSITION_SIZE, addr::PUSH_POINT);
    const int traitReads = CountDwords(addr::TEST_DIRECTIONS, addr::TEST_DIRECTIONS_SIZE, addr::OBJECT_TRAITS_PTR);

    char text[160];
    std::snprintf(text, sizeof(text),
                  "testPossibleDirections->pushPoint calls %d, seedPosition->testPossibleDirections %d, "
                  "seedPosition->pushPoint %d, trait table reads %d",
                  directionPushes, seedDirections, seedPushes, traitReads);
    Log("Init", text);

    if (directionPushes < 1 || seedDirections < 1 || seedPushes < 1 || traitReads < 1)
    {
        Log("Init", "Pathfinder addresses do not match this executable; rules are not installed");
        return;
    }

    if (g_settings.canStopBit >= 0)
        g_canStopBit = g_settings.canStopBit;

    if (!WriteHiHook(addr::PUSH_POINT, THISCALL_, reinterpret_cast<void *>(PushPointHook)) ||
        !WriteHiHook(addr::TEST_DIRECTIONS, THISCALL_, reinterpret_cast<void *>(TestDirectionsHook)))
    {
        _pi->UndoAll();
        Log("Init", "Failed to install hooks");
        return;
    }
    m_isEnabled = true;
}
} // namespace flylanding
