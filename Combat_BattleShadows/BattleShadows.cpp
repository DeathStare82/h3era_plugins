#include "framework.h"
#include "BattleShadows.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace battle_shadows
{
namespace
{
constexpr INT32 COLUMNS = 17;
constexpr INT32 BATTLE_HEXES = 187;
constexpr INT32 MAX_RADIUS = 4;
constexpr INT32 FREE_HEX_SLOTS = 10; // x1..x10 of BattleShadow_Hexes
constexpr INT32 PLUGIN_VERSION = 5;  // reported by BattleShadow_Debug
constexpr const char *MOUSE_HINT_EVENT = "OnBattleMouseHint"; // !?MM0
// ERA fires "<trigger name>_Quit" after all handlers of a trigger when a function with that name exists.
constexpr const char *MOUSE_HINT_QUIT_EVENT = "OnBattleMouseHint_Quit";

// Native calls already used in this repository with the same arguments:
// Combat_SwitchingCreatureAction (0x4746B0, 0x43ED00, 0x493FC0, 0x4934B0) and
// ERA_SystemOptionsExtension (0x4934B0 named BattleMgr::DrawGrid).
constexpr UINT COMBAT_DLG_PROC = 0x4746B0;    // thiscall(combat, H3Msg *) -> INT: every combat screen message
constexpr UINT SET_STACK_CONTOUR = 0x43ED00;  // thiscall(stack, BOOL8 on) -> BOOL8 changed
constexpr UINT REDRAW_BATTLEFIELD = 0x493FC0; // thiscall(combat, flip, setRedraws, useRedraws, wait, background, doWait)
// BattleMgr::DrawGrid rebuilds the background layer (H3CombatManager::drawBuffer: ground, grid, shadows) that the
// battlefield redraw reuses; obstacles and stacks are drawn over it later. While doNotDrawShade is set it leaves
// the layer untouched, so this plugin paints its hexes itself and saves the pixels under them first, to put them
// back before the next paint.
constexpr UINT DRAW_GRID = 0x4934B0;            // thiscall(combat, BOOL, BOOL)
constexpr UINT CURSOR_SHADOW_OPTION = 0x698810; // game option "cursor shadow" (diagnostics only)
// CCellShd.pcx is the hex mask. Its pixels use the Heroes 3 special palette slots, which mean "darken" rather than
// a color (copying its raw colors gives solid blue hexes). Amount to darken, out of 256, per slot; VCMI uses the
// same table for this picture: 0 transparent, 1..2 shadow border, 3..4 shadow body.
constexpr UINT8 SHADOW_ALPHA[] = {0, 32, 64, 128, 128};
constexpr UINT8 SHADOW_ALPHA_OTHER = 128;
constexpr INT32 HEX_WIDTH = 0x2D;
constexpr INT32 HEX_HEIGHT = 0x34;

struct SavedArea
{
    INT32 x;
    INT32 y;
    INT32 width;
    INT32 height;
    std::vector<UINT8> bytes;
};

struct ShadowState
{
    // Shapes ERM asked for during the current mouse hint (Radius/Hexes); dropped before every !?MM0.
    std::vector<INT32> hintHexes;
    std::vector<INT32> hintContourHexes;
    INT32 hintAnchor = -1; // cursor hex those shapes belong to
    bool inHint = false;   // between OnBattleMouseHint and its _Quit
    // Follow mode: a radius around the hex under the mouse, updated by the plugin on every mouse move.
    INT32 followRadius = -1;
    bool followContour = false;
    // What is on the screen now.
    std::vector<INT32> paintedHexes;              // sorted
    std::vector<H3CombatCreature *> contourStacks; // stacks that should be outlined (sorted)
    std::vector<H3CombatCreature *> litStacks;     // contours switched on by this plugin
    std::vector<SavedArea> savedAreas;             // layer pixels under painted hexes, in paint order
    H3LoadedPcx16 *savedLayer = nullptr;
    INT32 savedBytesPerPixel = 0;
    // Screen updates.
    INT32 procedureDepth = 0;    // inside the combat screen message handler
    bool presentPending = false; // the layer changed but the battlefield was not redrawn yet
    INT32 repaints = 0;          // diagnostics
};
ShadowState state;

H3CombatManager *ActiveCombat()
{
    H3CombatManager *combat = H3CombatManager::Get();
    return combat && combat->dlg ? combat : nullptr;
}

bool CanDraw(H3CombatManager *combat)
{
    return combat && combat->dlg && !combat->IsHiddenBattle();
}

INT32 CursorHex(H3CombatManager *combat)
{
    INT32 x = 0;
    INT32 y = 0;
    H3POINT::GetCursorPosition(x, y);
    return combat->SquareAtCoordinates(x - combat->dlg->GetX(), y - combat->dlg->GetY());
}

// Columns 0 and 16 are the hero edges of the field. Skipping them also makes omitted ERM arguments (0) harmless.
bool IsShadable(INT32 hex)
{
    if (hex < 0 || hex >= BATTLE_HEXES)
        return false;
    const INT32 column = hex % COLUMNS;
    return column > 0 && column < COLUMNS - 1;
}

// Hex distance on the 17 x 11 field. Which rows are shifted right is read from the drawn hex positions.
INT32 HexDistance(H3CombatManager *combat, INT32 from, INT32 to)
{
    const bool evenRowsShiftedRight = combat->squares[1].left > combat->squares[COLUMNS + 1].left;
    const auto axialQ = [evenRowsShiftedRight](INT32 hex) {
        const INT32 column = hex % COLUMNS;
        const INT32 row = hex / COLUMNS;
        const INT32 odd = row & 1;
        return evenRowsShiftedRight ? column - (row + odd) / 2 : column - (row - odd) / 2;
    };
    const INT32 dq = axialQ(to) - axialQ(from);
    const INT32 dr = to / COLUMNS - from / COLUMNS;
    return (std::abs(dq) + std::abs(dr) + std::abs(dq + dr)) / 2;
}

void AppendArea(H3CombatManager *combat, INT32 center, INT32 radius, std::vector<INT32> &out)
{
    if (center < 0 || center >= BATTLE_HEXES || radius < 0 || radius > MAX_RADIUS)
        return;
    for (INT32 hex = 0; hex < BATTLE_HEXES; ++hex)
        if (IsShadable(hex) && HexDistance(combat, center, hex) <= radius)
            out.push_back(hex);
}

template <typename T> void SortUnique(std::vector<T> &items)
{
    std::sort(items.begin(), items.end());
    items.erase(std::unique(items.begin(), items.end()), items.end());
}

template <typename T> bool Contains(const std::vector<T> &items, const T &item)
{
    return std::find(items.begin(), items.end(), item) != items.end();
}

H3Vector<INT32> &NativeHexes()
{
    return *H3BattleShadowHexes::Get();
}

INT32 BytesPerPixel(const H3LoadedPcx16 *layer)
{
    return layer->scanlineSize >= layer->width * 4 ? 4 : 2; // HD 32-bit mode or the game's 16-bit mode
}

// Puts back the layer pixels saved under our last paint (newest first, because neighbouring hexes overlap).
void RestoreSavedAreas(H3CombatManager *combat)
{
    H3LoadedPcx16 *layer = combat ? combat->drawBuffer : nullptr;
    if (layer && layer == state.savedLayer && layer->buffer && BytesPerPixel(layer) == state.savedBytesPerPixel)
    {
        const INT32 bpp = state.savedBytesPerPixel;
        for (auto area = state.savedAreas.rbegin(); area != state.savedAreas.rend(); ++area)
        {
            if (area->x + area->width > layer->width || area->y + area->height > layer->height)
                continue;
            const INT32 rowBytes = area->width * bpp;
            for (INT32 row = 0; row < area->height; ++row)
                std::memcpy(layer->GetRow(area->y + row) + area->x * bpp, area->bytes.data() + row * rowBytes, rowBytes);
        }
    }
    state.savedAreas.clear();
    state.savedLayer = nullptr;
}

UINT32 DarkenChannel(UINT32 color, UINT32 mask, UINT32 keep)
{
    return (((color & mask) * keep) >> 8) & mask;
}

// Darkens one hex of the layer through the CCellShd mask, saving the pixels underneath first.
void PaintHex(H3LoadedPcx16 *layer, const H3LoadedPcx *mask, INT32 left, INT32 top)
{
    const INT32 maskWidth = std::min(mask->width, HEX_WIDTH);
    const INT32 maskHeight = std::min(mask->height, HEX_HEIGHT);
    const INT32 x0 = std::max(left, 0);
    const INT32 y0 = std::max(top, 0);
    const INT32 x1 = std::min(left + maskWidth, layer->width);
    const INT32 y1 = std::min(top + maskHeight, layer->height);
    if (x0 >= x1 || y0 >= y1)
        return;

    const INT32 bpp = BytesPerPixel(layer);
    SavedArea saved{x0, y0, x1 - x0, y1 - y0, {}};
    const INT32 rowBytes = saved.width * bpp;
    saved.bytes.resize(size_t(rowBytes) * saved.height);
    for (INT32 y = y0; y < y1; ++y)
        std::memcpy(saved.bytes.data() + (y - y0) * rowBytes, layer->GetRow(y) + x0 * bpp, rowBytes);
    state.savedAreas.push_back(std::move(saved));

    UINT32 red = H3RedMask::Get();
    UINT32 green = H3GreenMask::Get();
    UINT32 blue = H3BlueMask::Get();
    if (!red || !green || !blue)
    {
        red = 0xF800; // RGB565
        green = 0x07E0;
        blue = 0x001F;
    }

    for (INT32 y = y0; y < y1; ++y)
    {
        const UINT8 *maskRow = mask->GetRow(y - top);
        PUINT8 layerRow = layer->GetRow(y);
        for (INT32 x = x0; x < x1; ++x)
        {
            const UINT8 slot = maskRow[x - left];
            const UINT32 alpha = size_t(slot) < sizeof(SHADOW_ALPHA) ? SHADOW_ALPHA[slot] : SHADOW_ALPHA_OTHER;
            if (!alpha)
                continue;
            const UINT32 keep = 256 - alpha;
            PUINT8 pixel = layerRow + x * bpp;
            if (bpp == 4)
            {
                for (INT32 channel = 0; channel < 3; ++channel) // B, G, R; alpha byte stays
                    pixel[channel] = UINT8((pixel[channel] * keep) >> 8);
            }
            else
            {
                UINT16 color = 0;
                std::memcpy(&color, pixel, sizeof(color));
                color = UINT16(DarkenChannel(color, red, keep) | DarkenChannel(color, green, keep) |
                               DarkenChannel(color, blue, keep));
                std::memcpy(pixel, &color, sizeof(color));
            }
        }
    }
}

// Paints paintedHexes on the layer. Hexes the game shaded itself are skipped, so a hex is never darkened twice.
void PaintLayer(H3CombatManager *combat)
{
    H3LoadedPcx16 *layer = combat->drawBuffer;
    const H3LoadedPcx *mask = combat->CCellShdPcx;
    if (state.paintedHexes.empty() || !layer || !layer->buffer || !mask || !mask->buffer)
        return;
    state.savedLayer = layer;
    state.savedBytesPerPixel = BytesPerPixel(layer);
    H3Vector<INT32> &native = NativeHexes();
    const bool gameDrewItsList = !combat->doNotDrawShade;
    for (const INT32 hex : state.paintedHexes)
    {
        if (gameDrewItsList && std::find(native.begin(), native.end(), hex) != native.end())
            continue;
        const H3CombatSquare &square = combat->squares[hex];
        PaintHex(layer, mask, square.left, square.top);
    }
}

BOOL8 SetContour(H3CombatCreature *stack, BOOL8 on)
{
    return THISCALL_2(BOOL8, SET_STACK_CONTOUR, stack, on);
}

void UpdateContours(const std::vector<H3CombatCreature *> &wanted)
{
    std::vector<H3CombatCreature *> stillLit;
    for (H3CombatCreature *stack : state.litStacks)
    {
        if (Contains(wanted, stack))
            stillLit.push_back(stack);
        else
            SetContour(stack, FALSE);
    }
    for (H3CombatCreature *stack : wanted)
        if (!Contains(stillLit, stack) && SetContour(stack, TRUE)) // FALSE: the game already outlines it
            stillLit.push_back(stack);
    state.litStacks = stillLit;
    state.contourStacks = wanted;
}

void Present(H3CombatManager *combat)
{
    state.presentPending = false;
    if (CanDraw(combat))
        THISCALL_7(void, REDRAW_BATTLEFIELD, combat, TRUE, TRUE, FALSE, 0, TRUE, FALSE);
}

// Brings the screen in line with the requested shapes. Does nothing when they did not change, so it can run on
// every message. Inside the combat message handler the battlefield is redrawn once, after the handler.
void Commit(H3CombatManager *combat, bool allowPresent = true)
{
    if (!CanDraw(combat))
        return;
    std::vector<INT32> hexes = state.hintHexes;
    std::vector<INT32> contourHexes = state.hintContourHexes;
    if (state.followRadius >= 0)
    {
        std::vector<INT32> follow;
        AppendArea(combat, CursorHex(combat), state.followRadius, follow);
        hexes.insert(hexes.end(), follow.begin(), follow.end());
        if (state.followContour)
            contourHexes.insert(contourHexes.end(), follow.begin(), follow.end());
    }
    SortUnique(hexes);
    std::vector<H3CombatCreature *> stacks;
    for (const INT32 hex : contourHexes)
    {
        H3CombatCreature *stack = combat->squares[hex].GetCreature();
        if (stack && stack->numberAlive > 0)
            stacks.push_back(stack);
    }
    SortUnique(stacks);

    const bool hexesChanged = hexes != state.paintedHexes;
    const bool contoursChanged = stacks != state.contourStacks;
    if (!hexesChanged && !contoursChanged)
        return;
    if (hexesChanged)
    {
        RestoreSavedAreas(combat);
        state.paintedHexes = hexes;
        PaintLayer(combat);
    }
    if (contoursChanged)
        UpdateContours(stacks);
    ++state.repaints;
    state.presentPending = true;
    if (allowPresent && state.procedureDepth == 0)
        Present(combat);
}

// Commits now unless an !?MM0 is still running; its _Quit or the end of the message handler commits then.
void CommitSoon()
{
    if (!state.inHint)
        Commit(ActiveCombat());
}

void ClearHint()
{
    state.hintHexes.clear();
    state.hintContourHexes.clear();
    state.hintAnchor = -1;
}

// Drops everything without touching the game (battle over, game left or loaded).
void Forget()
{
    ClearHint();
    state.inHint = false;
    state.followRadius = -1;
    state.followContour = false;
    state.paintedHexes.clear();
    state.contourStacks.clear();
    state.litStacks.clear();
    state.savedAreas.clear();
    state.savedLayer = nullptr;
    state.presentPending = false;
}

void BeginHintChange(H3CombatManager *combat)
{
    if (state.hintHexes.empty() && state.hintContourHexes.empty())
        state.hintAnchor = CursorHex(combat);
}

// !!FU(BattleShadow_Radius):P(hex)/(radius)/(contour);
// x1 = center hex (for example from !!MM:D), -1 = nothing; x2 = radius 0..4, 0 = only the center hex;
// x3 = 1 to outline the stacks inside the area.
void __stdcall OnRadius(Era::TEvent *)
{
    H3CombatManager *combat = ActiveCombat();
    if (!combat)
        return;
    BeginHintChange(combat);
    std::vector<INT32> area;
    AppendArea(combat, Era::x[1], Era::x[2], area);
    state.hintHexes.insert(state.hintHexes.end(), area.begin(), area.end());
    if (Era::x[3] != 0)
        state.hintContourHexes.insert(state.hintContourHexes.end(), area.begin(), area.end());
    CommitSoon();
}

// !!FU(BattleShadow_Hexes):P(hex1)/(hex2)/.../(hex10)/(contour);
// x1..x10 = hexes to shade, -1 = unused slot; x11 = 1 to outline the stacks on those hexes.
void __stdcall OnHexes(Era::TEvent *)
{
    H3CombatManager *combat = ActiveCombat();
    if (!combat)
        return;
    BeginHintChange(combat);
    const bool contour = Era::x[FREE_HEX_SLOTS + 1] != 0;
    for (INT32 slot = 1; slot <= FREE_HEX_SLOTS; ++slot)
    {
        const INT32 hex = Era::x[slot];
        if (!IsShadable(hex))
            continue;
        state.hintHexes.push_back(hex);
        if (contour)
            state.hintContourHexes.push_back(hex);
    }
    CommitSoon();
}

// !!FU(BattleShadow_Follow):P(radius)/(contour);
// x1 = radius 0..4 around the hex under the mouse, -1 = off; x2 = 1 to outline the stacks inside.
// The plugin follows the mouse itself until switched off or the next battle action.
void __stdcall OnFollow(Era::TEvent *)
{
    const INT32 radius = Era::x[1];
    state.followRadius = radius >= 0 && radius <= MAX_RADIUS ? radius : -1;
    state.followContour = Era::x[2] != 0;
    CommitSoon();
}

// !!FU(BattleShadow_Clear):P;  Removes everything, follow mode included.
void __stdcall OnClear(Era::TEvent *)
{
    ClearHint();
    state.followRadius = -1;
    state.followContour = false;
    CommitSoon();
}

// !!FU(BattleShadow_Debug):P?(version)/?(painted)/?(hint)/?(follow)/?(noShade)/?(option)/?(repaints)/?(anchor)/?(cursor);
// x1 = plugin version (0 if this plugin did not answer), x2 = hexes on the screen now,
// x3 = hexes asked for in the current mouse hint, x4 = follow radius (-1 = off), x5 = doNotDrawShade flag,
// x6 = "cursor shadow" game option, x7 = screen updates by this plugin in this battle,
// x8 = hex the hint shapes belong to, x9 = hex under the cursor as the plugin sees it.
void __stdcall OnDebug(Era::TEvent *)
{
    H3CombatManager *combat = ActiveCombat();
    Era::x[1] = PLUGIN_VERSION;
    Era::x[2] = INT32(state.paintedHexes.size());
    Era::x[3] = INT32(state.hintHexes.size());
    Era::x[4] = state.followRadius;
    Era::x[5] = combat ? INT32(combat->doNotDrawShade) : -1;
    Era::x[6] = INT32(DwordAt(CURSOR_SHADOW_OPTION));
    Era::x[7] = state.repaints;
    Era::x[8] = state.hintAnchor;
    Era::x[9] = combat ? CursorHex(combat) : -1;
}

// ERA calls plugin handlers of a trigger before its ERM code: every !?MM0 starts with no hint shapes.
// Nothing is drawn yet; the result is committed once, when the trigger is over.
void __stdcall OnBattleMouseHint(Era::TEvent *)
{
    INT32 quitId = 0;
    if (Era::AllocErmFunc)
        Era::AllocErmFunc(MOUSE_HINT_QUIT_EVENT, quitId);
    ClearHint();
    H3CombatManager *combat = ActiveCombat();
    state.hintAnchor = combat ? CursorHex(combat) : -1;
    state.inHint = true;
}

void __stdcall OnBattleMouseHintQuit(Era::TEvent *)
{
    state.inHint = false;
    Commit(ActiveCombat());
}

void __stdcall OnBeforeBattleAction(Era::TEvent *)
{
    ClearHint();
    state.inHint = false;
    state.followRadius = -1;
    state.followContour = false;
    Commit(ActiveCombat(), false); // puts the layer back; the action redraws the battlefield itself
    state.presentPending = false;
}

void __stdcall OnLeaveBattleContext(Era::TEvent *)
{
    Forget();
}

void __stdcall OnBattleStart(Era::TEvent *)
{
    Forget();
    state.repaints = 0;
    INT32 quitId = 0;
    if (Era::AllocErmFunc)
        Era::AllocErmFunc(MOUSE_HINT_QUIT_EVENT, quitId); // lets ERA fire _Quit from the first hint
}

int __stdcall HookDrawGrid(HiHook *hook, H3CombatManager *combat, INT32 arg1, INT32 arg2)
{
    // Undo our paint first: when the game leaves the layer untouched, nothing else would erase it.
    RestoreSavedAreas(combat);
    const int result = THISCALL_3(int, hook->GetDefaultFunc(), combat, arg1, arg2);
    if (CanDraw(combat))
        PaintLayer(combat);
    return result;
}

// Every combat screen message passes here, mouse moves included, so follow mode reacts as fast as the game's own
// cursor shadow and hint shapes are committed right after the !?MM0 that built them.
INT __stdcall HookCombatProcedure(HiHook *hook, H3CombatManager *combat, H3Msg *msg)
{
    ++state.procedureDepth;
    const INT result = THISCALL_2(INT, hook->GetDefaultFunc(), combat, msg);
    --state.procedureDepth;
    if (state.procedureDepth == 0 && combat && combat->dlg && combat == ActiveCombat())
    {
        state.inHint = false;
        // The mouse reached another hex without a new !?MM0: the old hint shapes no longer apply.
        if (msg && msg->IsMouseOver() && !state.hintHexes.empty() && CursorHex(combat) != state.hintAnchor)
            ClearHint();
        Commit(combat, false);
        if (state.presentPending)
            Present(combat);
    }
    return result;
}
} // namespace

void RegisterEvents()
{
    Era::RegisterHandler(OnRadius, "BattleShadow_Radius");
    Era::RegisterHandler(OnHexes, "BattleShadow_Hexes");
    Era::RegisterHandler(OnFollow, "BattleShadow_Follow");
    Era::RegisterHandler(OnClear, "BattleShadow_Clear");
    Era::RegisterHandler(OnDebug, "BattleShadow_Debug");
    Era::RegisterHandler(OnBattleMouseHint, MOUSE_HINT_EVENT);
    Era::RegisterHandler(OnBattleMouseHintQuit, MOUSE_HINT_QUIT_EVENT);
    Era::RegisterHandler(OnBeforeBattleAction, "OnBeforeBattleAction");
    Era::RegisterHandler(OnBattleStart, "OnBeforeBattleUniversal");
    Era::RegisterHandler(OnLeaveBattleContext, "OnAfterBattleUniversal");
    Era::RegisterHandler(OnLeaveBattleContext, "OnGameLeave");
    Era::RegisterHandler(OnLeaveBattleContext, "OnBeforeLoadGame");
}

void InstallHooks(PatcherInstance *patcher)
{
    static bool installed = false;
    if (installed || !patcher)
        return;
    installed = true;
    // Splice the function entries explicitly: these are callees, not call sites.
    patcher->WriteHiHook(DRAW_GRID, SPLICE_, THISCALL_, HookDrawGrid);
    patcher->WriteHiHook(COMBAT_DLG_PROC, SPLICE_, THISCALL_, HookCombatProcedure);
}
}
