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
constexpr INT32 PLUGIN_VERSION = 4;  // reported by BattleShadow_Debug
constexpr const char *MOUSE_HINT_EVENT = "OnBattleMouseHint"; // !?MM0
// ERA fires "<trigger name>_Quit" after all handlers of a trigger when a function with that name exists.
constexpr const char *MOUSE_HINT_QUIT_EVENT = "OnBattleMouseHint_Quit";

// Native calls already used in this repository with the same arguments:
// Combat_SwitchingCreatureAction (0x493F10, 0x493A20, 0x43ED00, 0x493FC0, 0x4934B0) and
// ERA_SystemOptionsExtension (0x4934B0 named BattleMgr::DrawGrid).
constexpr UINT SET_CURSOR_SHADOW = 0x493F10;  // thiscall(combat, hex, flag); hex -1 clears
constexpr UINT SET_AREA_SHADOW = 0x493A20;    // thiscall(combat, hex, H3Vector<INT32> *hexes, BOOL8)
constexpr UINT SET_STACK_CONTOUR = 0x43ED00;  // thiscall(stack, BOOL8 on) -> BOOL8 changed
constexpr UINT REDRAW_BATTLEFIELD = 0x493FC0; // thiscall(combat, flip, setRedraws, useRedraws, wait, background, doWait)
// BattleMgr::DrawGrid rebuilds the background layer (H3CombatManager::drawBuffer: ground, grid, shadows) that the
// battlefield redraw reuses; obstacles and stacks are drawn over it later. While doNotDrawShade is set it leaves
// the layer untouched, so this plugin paints its hexes itself right after DrawGrid and saves the pixels under
// them first, to put them back before the next paint.
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
    std::vector<INT32> hexes;                 // requested by ERM, unique
    std::vector<H3CombatCreature *> contours; // stacks whose contour this plugin switched on
    INT32 anchorHex = -1;                     // cursor hex the current shadows belong to
    bool deferRedraw = false;                 // inside !?MM0: redraw once from the _Quit handler
    bool quitEventSeen = false;               // this ERA build fires OnBattleMouseHint_Quit
    std::vector<INT32> hintStartHexes;        // state before the current !?MM0, to skip identical redraws
    std::vector<H3CombatCreature *> hintStartContours;
    INT32 gridRebuilds = 0;                   // DrawGrid calls in this battle (diagnostics)
    INT32 lastDrawn = 0;                      // hexes painted by this plugin at the last DrawGrid (diagnostics)
    std::vector<SavedArea> savedAreas;        // layer pixels under our painted hexes, in paint order
    H3LoadedPcx16 *savedLayer = nullptr;
    INT32 savedBytesPerPixel = 0;
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

bool HasState()
{
    return !state.hexes.empty() || !state.contours.empty();
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

// Paints our hexes on the layer DrawGrid has just built. Hexes the game shaded itself are skipped,
// so a hex is never darkened twice.
INT32 PaintOwnShadows(H3CombatManager *combat)
{
    H3LoadedPcx16 *layer = combat->drawBuffer;
    const H3LoadedPcx *mask = combat->CCellShdPcx;
    if (!layer || !layer->buffer || !mask || !mask->buffer)
        return 0;
    state.savedLayer = layer;
    state.savedBytesPerPixel = BytesPerPixel(layer);
    H3Vector<INT32> &native = NativeHexes();
    const bool gameDrewItsList = !combat->doNotDrawShade;
    INT32 painted = 0;
    for (const INT32 hex : state.hexes)
    {
        if (gameDrewItsList && std::find(native.begin(), native.end(), hex) != native.end())
            continue;
        const H3CombatSquare &square = combat->squares[hex];
        PaintHex(layer, mask, square.left, square.top);
        ++painted;
    }
    return painted;
}

void RebuildShadowLayer(H3CombatManager *combat)
{
    if (CanDraw(combat))
        THISCALL_3(void, DRAW_GRID, combat, FALSE, TRUE); // HookDrawGrid adds our hexes
}

void Redraw(H3CombatManager *combat)
{
    if (!CanDraw(combat))
        return;
    RebuildShadowLayer(combat);
    THISCALL_7(void, REDRAW_BATTLEFIELD, combat, TRUE, TRUE, FALSE, 0, TRUE, FALSE);
}

BOOL8 SetContour(H3CombatCreature *stack, BOOL8 on)
{
    return THISCALL_2(BOOL8, SET_STACK_CONTOUR, stack, on);
}

bool AddContour(H3CombatManager *combat, INT32 hex)
{
    H3CombatCreature *stack = combat->squares[hex].GetCreature();
    if (!stack || stack->numberAlive <= 0 ||
        std::find(state.contours.begin(), state.contours.end(), stack) != state.contours.end())
        return false;
    if (!SetContour(stack, TRUE))
        return false; // the game already highlights this stack; leave it to the game
    state.contours.push_back(stack);
    return true;
}

void ReapplyContours()
{
    for (H3CombatCreature *stack : state.contours)
        if (stack->numberAlive > 0)
            SetContour(stack, TRUE);
}

void Forget()
{
    state.hexes.clear();
    state.contours.clear();
    state.anchorHex = -1;
}

// Drops our hexes and contours. Returns true when something had been shown; the caller rebuilds the layer.
bool Reset()
{
    const bool hadState = HasState();
    if (ActiveCombat())
        for (H3CombatCreature *stack : state.contours)
            SetContour(stack, FALSE);
    Forget();
    return hadState;
}

void LeaveHint()
{
    state.deferRedraw = false;
    state.hintStartHexes.clear();
    state.hintStartContours.clear();
}

template <typename T> bool SameItems(std::vector<T> a, std::vector<T> b)
{
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    return a == b;
}

void RequestRedraw(H3CombatManager *combat)
{
    if (!state.deferRedraw)
        Redraw(combat);
}

H3CombatManager *BeginChange()
{
    H3CombatManager *combat = ActiveCombat();
    if (combat && !HasState())
        state.anchorHex = CursorHex(combat);
    return combat;
}

void AddHex(H3CombatManager *combat, INT32 hex, bool contour, bool &changed)
{
    if (!IsShadable(hex))
        return;
    if (std::find(state.hexes.begin(), state.hexes.end(), hex) == state.hexes.end())
    {
        state.hexes.push_back(hex);
        changed = true;
    }
    if (contour && AddContour(combat, hex))
        changed = true;
}

void EndChange(H3CombatManager *combat, bool changed)
{
    if (changed)
        RequestRedraw(combat);
}

// !!FU(BattleShadow_Radius):P(hex)/(radius)/(contour);
// x1 = center hex (for example from !!MM:D), -1 = nothing; x2 = radius 0..4, 0 = only the center hex;
// x3 = 1 to outline the stacks inside the area.
void __stdcall OnRadius(Era::TEvent *)
{
    const INT32 center = Era::x[1];
    const INT32 radius = Era::x[2];
    const bool contour = Era::x[3] != 0;
    if (center < 0 || center >= BATTLE_HEXES || radius < 0 || radius > MAX_RADIUS)
        return;
    H3CombatManager *combat = BeginChange();
    if (!combat)
        return;
    bool changed = false;
    for (INT32 hex = 0; hex < BATTLE_HEXES; ++hex)
        if (HexDistance(combat, center, hex) <= radius)
            AddHex(combat, hex, contour, changed);
    EndChange(combat, changed);
}

// !!FU(BattleShadow_Hexes):P(hex1)/(hex2)/.../(hex10)/(contour);
// x1..x10 = hexes to shade, -1 = unused slot; x11 = 1 to outline the stacks on those hexes.
void __stdcall OnHexes(Era::TEvent *)
{
    const bool contour = Era::x[FREE_HEX_SLOTS + 1] != 0;
    H3CombatManager *combat = BeginChange();
    if (!combat)
        return;
    bool changed = false;
    for (INT32 slot = 1; slot <= FREE_HEX_SLOTS; ++slot)
        AddHex(combat, Era::x[slot], contour, changed);
    EndChange(combat, changed);
}

// !!FU(BattleShadow_Clear):P;
void __stdcall OnClear(Era::TEvent *)
{
    H3CombatManager *combat = ActiveCombat();
    if (Reset() && combat)
        RequestRedraw(combat);
}

// !!FU(BattleShadow_Debug):P?(version)/?(mine)/?(drawn)/?(listSize)/?(noShade)/?(option)/?(rebuilds)/?(anchor)/?(cursor);
// x1 = plugin version (0 if this plugin did not answer), x2 = hexes this plugin wants to shade,
// x3 = hexes it painted at the last layer rebuild, x4 = size of the game's own shadow list,
// x5 = doNotDrawShade flag, x6 = "cursor shadow" game option, x7 = layer rebuilds (DrawGrid) in this battle,
// x8 = hex the shadows belong to, x9 = hex under the cursor as the plugin sees it.
void __stdcall OnDebug(Era::TEvent *)
{
    H3CombatManager *combat = ActiveCombat();
    Era::x[1] = PLUGIN_VERSION;
    Era::x[2] = INT32(state.hexes.size());
    Era::x[3] = state.lastDrawn;
    Era::x[4] = INT32(NativeHexes().Size());
    Era::x[5] = combat ? INT32(combat->doNotDrawShade) : -1;
    Era::x[6] = INT32(DwordAt(CURSOR_SHADOW_OPTION));
    Era::x[7] = state.gridRebuilds;
    Era::x[8] = state.anchorHex;
    Era::x[9] = combat ? CursorHex(combat) : -1;
}

// ERA calls plugin handlers of a trigger before its ERM code: every !?MM0 starts from a clean state.
void __stdcall OnBattleMouseHint(Era::TEvent *)
{
    INT32 quitId = 0;
    // A name created only now was not seen by ERA when this trigger started, so _Quit will not fire.
    const bool quitWillFire =
        Era::AllocErmFunc && !Era::AllocErmFunc(MOUSE_HINT_QUIT_EVENT, quitId) && state.quitEventSeen;
    H3CombatManager *combat = ActiveCombat();
    state.hintStartHexes = state.hexes;
    state.hintStartContours = state.contours;
    const bool removed = Reset();
    state.deferRedraw = combat && quitWillFire;
    if (!combat)
        return;
    state.anchorHex = CursorHex(combat);
    if (removed)
        RequestRedraw(combat);
}

void __stdcall OnBattleMouseHintQuit(Era::TEvent *)
{
    state.quitEventSeen = true;
    const bool changed =
        !SameItems(state.hintStartHexes, state.hexes) || !SameItems(state.hintStartContours, state.contours);
    const bool redraw = state.deferRedraw && changed;
    LeaveHint();
    if (redraw)
        Redraw(ActiveCombat());
}

void __stdcall OnBeforeBattleAction(Era::TEvent *)
{
    if (Reset())
        RebuildShadowLayer(ActiveCombat()); // the action's own redraws must not show our old shadows
    LeaveHint();
}

void __stdcall OnLeaveBattleContext(Era::TEvent *)
{
    Forget();
    LeaveHint();
    state.savedAreas.clear(); // the battle layer is gone or rebuilt; never write old pixels into it
    state.savedLayer = nullptr;
}

void __stdcall OnBattleStart(Era::TEvent *event)
{
    OnLeaveBattleContext(event);
    state.gridRebuilds = 0;
    state.lastDrawn = 0;
    INT32 quitId = 0;
    if (Era::AllocErmFunc)
        Era::AllocErmFunc(MOUSE_HINT_QUIT_EVENT, quitId); // lets ERA fire _Quit from the first hint
}

int __stdcall HookDrawGrid(HiHook *hook, H3CombatManager *combat, INT32 arg1, INT32 arg2)
{
    // Undo our previous paint first: when the game leaves the layer untouched, nothing else would erase it.
    RestoreSavedAreas(combat);
    const int result = THISCALL_3(int, hook->GetDefaultFunc(), combat, arg1, arg2);
    ++state.gridRebuilds;
    state.lastDrawn = HasState() && CanDraw(combat) ? PaintOwnShadows(combat) : 0;
    return result;
}

// The game updated its own cursor shadow. Keep ours only while the cursor is still on the hex they belong to;
// otherwise the mouse moved without !?MM0 (for example while choosing a spell target).
void AfterNativeShadowUpdate(H3CombatManager *combat)
{
    if (!HasState() || !combat || !combat->dlg)
        return;
    if (CursorHex(combat) == state.anchorHex)
    {
        ReapplyContours();
        return;
    }
    Reset();
    RebuildShadowLayer(combat);
}

int __stdcall HookSetCursorShadow(HiHook *hook, H3CombatManager *combat, INT32 hex, INT32 flag)
{
    const int result = THISCALL_3(int, hook->GetDefaultFunc(), combat, hex, flag);
    AfterNativeShadowUpdate(combat);
    return result;
}

int __stdcall HookSetAreaShadow(HiHook *hook, H3CombatManager *combat, INT32 hex, H3Vector<INT32> *hexes, INT32 flag)
{
    const int result = THISCALL_4(int, hook->GetDefaultFunc(), combat, hex, hexes, flag);
    AfterNativeShadowUpdate(combat);
    return result;
}
} // namespace

void RegisterEvents()
{
    Era::RegisterHandler(OnRadius, "BattleShadow_Radius");
    Era::RegisterHandler(OnHexes, "BattleShadow_Hexes");
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
    patcher->WriteHiHook(SET_CURSOR_SHADOW, SPLICE_, THISCALL_, HookSetCursorShadow);
    patcher->WriteHiHook(SET_AREA_SHADOW, SPLICE_, THISCALL_, HookSetAreaShadow);
}
}
