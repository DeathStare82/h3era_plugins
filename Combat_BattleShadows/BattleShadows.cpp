#include "framework.h"
#include "BattleShadows.h"
#include <algorithm>
#include <cstdlib>
#include <vector>

namespace battle_shadows
{
namespace
{
constexpr INT32 COLUMNS = 17;
constexpr INT32 BATTLE_HEXES = 187;
constexpr INT32 MAX_RADIUS = 4;
constexpr INT32 HEX_SLOTS = 10;  // BattleShadow_Hexes / BattleShadow_Stacks: x1..x10, x11 = contour
constexpr INT32 SKIP_SLOTS = 16; // BattleShadow_SkipStacks / BattleShadow_SkipHexes: x1..x16
constexpr INT32 PLUGIN_VERSION = 9;
constexpr const char *MOUSE_HINT_EVENT = "OnBattleMouseHint"; // !?MM0

// How the shadows are drawn (v7): with the game's own cursor shadow layer, the one Fireball aiming uses.
// Checked against the disassembly of the game executable:
//   0x493A20 BattleMgr::ShadeHexes(anchorHex, H3Vector<INT32> *hexes, BOOL8 force) takes the previous list off the
//   battlefield background (combat->drawBuffer) from its save buffer, saves the ground under the new hexes, shades
//   them with CCellShd.pcx, then redraws and shows only the rectangle around old and new hexes. It returns at once
//   when the "cursor shadow" option is off, in hidden battles, and when anchorHex is the last one and force is 0.
//   Its save buffer has 19 slots (855 x 52 pixels), so the game never asks it for more than 19 hexes (Inferno).
//   0x493F10(hex, flag) is the one-hex cursor shadow; the mouse-move branch of the combat screen calls it at
//   0x474D0F, before the battle hint (and with it !?MM0) is built.
// So: the plugin takes the cursor request of the mouse-move branch, waits for !?MM0, and draws the cursor hex
// together with the ERM shapes in ONE call when the combat screen message is done. Up to 19 hexes it calls the
// game's function itself. Bigger lists use the same steps with a 187-slot save buffer of the plugin and the
// full battlefield redraw that Fireball aiming also does on every hex change (0x59FE1F).
constexpr UINT COMBAT_DLG_PROC = 0x4746B0;       // thiscall(combat, H3Msg *) -> INT: every combat screen message
constexpr UINT MOUSE_MOVE_SHADE_CALL = 0x474D0F; // call 0x493F10(combat, hex, 0) in the mouse-move branch
constexpr UINT CURSOR_SHADE = 0x493F10;          // thiscall(combat, hex, flag), ret 8
constexpr UINT SHADE_HEXES = 0x493A20;           // thiscall(combat, anchorHex, H3Vector<INT32> *, BOOL8 force), ret 0Ch
constexpr UINT SET_STACK_CONTOUR = 0x43ED00;     // thiscall(stack, BOOL8 on) -> BOOL8 changed
constexpr UINT REDRAW_BATTLEFIELD = 0x493FC0;    // thiscall(combat, 6 args); (1,1,0,0,1,0) as at 0x59FE1F
constexpr UINT PCX16_COPY = 0x44DF80;  // thiscall(src, sx, sy, w, h, dstBuffer, dx, dy, dstW, dstH, dstStride, transparent)
constexpr UINT PCX16_SHADE = 0x44E370; // thiscall(dst, x, y, w, h, H3LoadedPcx *mask, 0, 0): CCellShd onto the ground
constexpr UINT PCX16_CTOR = 0x44DC40;  // thiscall(pcx, width, height): how the game makes its own save buffer (0x462793)
constexpr UINT DRAW_MOVEMENT_GRID = 0x4934B0; // thiscall(combat, full, recompute) -> BOOL8: movement range shade + grid
constexpr UINT CURSOR_SHADOW_OPTION = 0x698810;   // game option "cursor shadow"
constexpr UINT MOVEMENT_SHADOW_OPTION = 0x698814; // game option "movement shadow" (only 0x4934B0 draws with it)
constexpr UINT ACTION_IN_PROGRESS = 0x698A3C;   // 0x493F10 ignores flag-0 calls while it is set
constexpr UINT LAST_SHADE_ANCHOR = 0x6772D8;    // anchorHex of the last 0x493A20 drawing
constexpr UINT NATIVE_SAVE_BUFFER = 0x53B4;     // H3CombatManager: H3LoadedPcx16 * of 0x493A20 (19 slots)
constexpr UINT SQUARE_SAVE_SLOT = 0x4D;         // H3CombatSquare: INT8 slot in that buffer, -1 = none
constexpr INT32 NATIVE_SLOTS = 19;
constexpr INT32 HEX_WIDTH = 0x2D;
constexpr INT32 HEX_HEIGHT = 0x34;
constexpr INT32 FIELD_HEIGHT = 0x22C;    // hexes below y 0x1F8 are cut at the bottom of the background
constexpr INT32 LOWEST_FULL_TOP = 0x1F8;

struct ShadowState
{
    // Set by ERM during the current mouse hint; dropped before every !?MM0.
    std::vector<INT32> hintHexes;
    std::vector<INT32> hintContourHexes;
    std::vector<INT32> skipHexes;     // stacks standing on these hexes are never shaded or outlined
    std::vector<INT32> excludedHexes; // these hexes are never shaded (BattleShadow_SkipHexes)
    INT32 hintAnchor = -1;        // cursor hex the hint shapes belong to
    // Follow mode: a radius around the hex under the mouse, kept until switched off or the next battle action.
    INT32 followRadius = -1;
    bool followContour = false;
    bool dirty = false;
    // The game's own one-hex cursor shadow, taken from the mouse-move branch.
    INT32 cursorHex = -1;
    bool cursorPending = false;
    INT32 procDepth = 0; // inside the combat screen procedure: drawing waits until the message is done
    // Layer drawn with the plugin's save buffer (only when more than 19 hexes were needed).
    bool portActive = false;
    std::vector<INT32> portHexes; // sorted
    // Stack outlines switched on by this plugin.
    std::vector<H3CombatCreature *> litStacks;
    // BattleShadow_HideMovement: the movement range shade is left out of the battlefield background.
    bool hideMovement = false;
    bool showMovementAtAction = false;
    // Diagnostics.
    INT32 nativeDraws = 0;
    INT32 portDraws = 0;
    INT32 fullRedraws = 0;
};
ShadowState state;
HiHook *shadeHook = nullptr;          // 0x493A20
bool mouseMoveHookInstalled = false;  // 0x474D0F
H3LoadedPcx16 *saveBuffer = nullptr;  // 187 slots of 45 x 52, slot = hex number

H3CombatManager *ActiveCombat()
{
    H3CombatManager *combat = H3CombatManager::Get();
    return combat && combat->dlg ? combat : nullptr;
}

bool CanDraw(H3CombatManager *combat)
{
    return combat && combat == H3CombatManager::Get() && combat->dlg && !combat->finished && !combat->IsHiddenBattle();
}

// The battlefield is the window on top (nothing like the spell book covers it).
bool BattlefieldOnTop(H3CombatManager *combat)
{
    H3WindowManager *windows = H3WindowManager::Get();
    return CanDraw(combat) && windows && windows->lastDlg == combat->dlg;
}

INT32 CursorHex(H3CombatManager *combat)
{
    INT32 x = 0;
    INT32 y = 0;
    H3POINT::GetCursorPosition(x, y);
    return combat->SquareAtCoordinates(x - combat->dlg->GetX(), y - combat->dlg->GetY());
}

// Columns 0 and 16 are the hero edges of the field (0x493F10 never shades them either). Skipping them also makes
// omitted ERM arguments (0) harmless.
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

H3CombatCreature *LiveStackAt(H3CombatManager *combat, INT32 hex)
{
    if (hex < 0 || hex >= BATTLE_HEXES)
        return nullptr;
    H3CombatCreature *stack = combat->squares[hex].GetCreature();
    return stack && stack->numberAlive > 0 ? stack : nullptr;
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

// ---------------------------------------------------------------------------------------------------------------
// The cursor shadow layer
// ---------------------------------------------------------------------------------------------------------------

UINT ShadeHexesOriginal()
{
    return shadeHook ? shadeHook->GetDefaultFunc() : SHADE_HEXES;
}

// Hexes shaded on the battlefield background now: the game's list, or ours while our save buffer holds them.
std::vector<INT32> ShownHexes()
{
    if (state.portActive)
        return state.portHexes;
    std::vector<INT32> shown;
    H3Vector<INT32> *list = H3BattleShadowHexes::Get();
    for (UINT32 i = 0; i < list->Size(); ++i)
        shown.push_back((*list)[i]);
    SortUnique(shown);
    return shown;
}

INT32 BytesPerPixel(const H3LoadedPcx16 *pcx)
{
    return pcx->scanlineSize >= pcx->width * 4 ? 4 : 2; // HD 32-bit mode or the game's 16-bit mode
}

INT32 SaveHeight(const H3CombatSquare &square)
{
    return square.top > LOWEST_FULL_TOP ? FIELD_HEIGHT - square.top : HEX_HEIGHT;
}

void CopyPixels(H3LoadedPcx16 *from, INT32 fromX, INT32 fromY, INT32 width, INT32 height, H3LoadedPcx16 *to, INT32 toX,
                INT32 toY)
{
    THISCALL_12(void, PCX16_COPY, from, fromX, fromY, width, height, to->buffer, toX, toY, to->width, to->height,
                to->scanlineSize, FALSE);
}

// The plugin's save buffer, made like the game's own one, so HD mode gives it the same pixel format.
bool EnsureSaveBuffer(H3CombatManager *combat)
{
    const H3LoadedPcx16 *ground = combat->drawBuffer;
    if (!ground || !ground->buffer || ground->width <= 0)
        return false;
    if (saveBuffer && saveBuffer->buffer && BytesPerPixel(saveBuffer) == BytesPerPixel(ground))
        return true;
    H3LoadedPcx16 *pcx = H3ObjectAllocator<H3LoadedPcx16>().allocate(1);
    if (!pcx)
        return false;
    THISCALL_3(H3LoadedPcx16 *, PCX16_CTOR, pcx, BATTLE_HEXES * HEX_WIDTH, HEX_HEIGHT);
    if (!pcx->buffer || BytesPerPixel(pcx) != BytesPerPixel(ground))
        return false; // kept allocated: harmless, and never reached in practice
    saveBuffer = pcx;
    return true;
}

// The same early returns as 0x493A20 (plus the pictures it needs).
bool LayerDrawable(H3CombatManager *combat, INT32 anchor, bool force)
{
    if (combat->finished || combat->IsHiddenBattle() || !DwordAt(CURSOR_SHADOW_OPTION))
        return false;
    if (anchor == IntAt(LAST_SHADE_ANCHOR) && !force)
        return false;
    const H3LoadedPcx16 *ground = combat->drawBuffer;
    return ground && ground->buffer && combat->CCellShdPcx && saveBuffer;
}

// Puts back the ground under the hexes our buffer holds; the layer is then the game's again (empty).
void RestorePortHexes(H3CombatManager *combat)
{
    for (const INT32 hex : state.portHexes)
    {
        const H3CombatSquare &square = combat->squares[hex];
        CopyPixels(saveBuffer, hex * HEX_WIDTH, 0, HEX_WIDTH, SaveHeight(square), combat->drawBuffer, square.left,
                   square.top);
    }
    state.portHexes.clear();
    state.portActive = false;
}

// Step 1 of 0x493A20: puts back the ground under the game's list from the game's save buffer and empties the list.
void RestoreNativeList(H3CombatManager *combat)
{
    H3Vector<INT32> *list = H3BattleShadowHexes::Get();
    H3LoadedPcx16 *ground = combat->drawBuffer;
    H3LoadedPcx16 *nativeSave =
        *reinterpret_cast<H3LoadedPcx16 **>(reinterpret_cast<UINT8 *>(combat) + NATIVE_SAVE_BUFFER);
    for (UINT32 i = 0; i < list->Size(); ++i)
    {
        const INT32 hex = (*list)[i];
        if (hex < 0 || hex >= BATTLE_HEXES)
            continue;
        H3CombatSquare &square = combat->squares[hex];
        UINT8 &slot = reinterpret_cast<UINT8 *>(&square)[SQUARE_SAVE_SLOT];
        const INT32 index = INT32(INT8(slot));
        if (ground && ground->buffer && nativeSave && nativeSave->buffer && index >= 0 && index < NATIVE_SLOTS)
            CopyPixels(nativeSave, index * HEX_WIDTH, 0, HEX_WIDTH, SaveHeight(square), ground, square.left,
                       square.top);
        slot = 0xFF;
    }
    list->RemoveAll();
}

// Takes the whole layer off the battlefield background without showing anything (the background is about to be
// rebuilt). The next commit draws it again.
void ClearLayerSilently(H3CombatManager *combat)
{
    if (state.portActive)
        RestorePortHexes(combat);
    else
        RestoreNativeList(combat);
    IntAt(LAST_SHADE_ANCHOR) = -1;
}

// Steps 1-3 of 0x493A20 for any number of hexes: take the old list off (from our buffer, or from the game's buffer
// when the game's list was shown), save the ground under the new hexes, shade them. Then the whole battlefield is
// redrawn, as Fireball aiming does after every change.
bool PortDraw(H3CombatManager *combat, INT32 anchor, const std::vector<INT32> &hexes, bool force)
{
    if (!LayerDrawable(combat, anchor, force))
        return false;
    H3LoadedPcx16 *ground = combat->drawBuffer;
    H3LoadedPcx *mask = combat->CCellShdPcx;

    if (state.portActive)
        RestorePortHexes(combat);
    else
        RestoreNativeList(combat); // the game's list stays empty while our buffer holds the layer
    for (const INT32 hex : hexes)
    {
        const H3CombatSquare &square = combat->squares[hex];
        CopyPixels(ground, square.left, square.top, HEX_WIDTH, SaveHeight(square), saveBuffer, hex * HEX_WIDTH, 0);
    }
    for (const INT32 hex : hexes)
    {
        const H3CombatSquare &square = combat->squares[hex];
        THISCALL_8(void, PCX16_SHADE, ground, INT32(square.left), INT32(square.top), HEX_WIDTH, HEX_HEIGHT, mask, 0, 0);
    }
    IntAt(LAST_SHADE_ANCHOR) = anchor;
    state.portHexes = hexes;
    state.portActive = !hexes.empty();
    ++state.portDraws;
    ++state.fullRedraws;
    THISCALL_7(void, REDRAW_BATTLEFIELD, combat, 1, 1, 0, 0, 1, 0);
    return true;
}

void CallShadeHexes(UINT function, H3CombatManager *combat, INT32 anchor, const std::vector<INT32> &hexes, bool force)
{
    H3Vector<INT32> vector;
    for (const INT32 hex : hexes)
        vector.Add(hex);
    THISCALL_4(void, function, combat, anchor, &vector, BOOL8(force ? 1 : 0));
    ++state.nativeDraws;
}

// Shows exactly `hexes` (sorted, unique) in the cursor shadow layer. Returns true if the battlefield was fully redrawn.
// Only one save buffer holds the layer at a time: the game's (up to 19 hexes) or ours.
bool Draw(H3CombatManager *combat, INT32 anchor, const std::vector<INT32> &hexes, bool force)
{
    if (INT32(hexes.size()) > NATIVE_SLOTS)
    {
        if (state.portActive || EnsureSaveBuffer(combat))
            return PortDraw(combat, anchor, hexes, force);
        // No buffer of our own: the game's layer can still show 19 of them.
        CallShadeHexes(ShadeHexesOriginal(), combat, anchor, std::vector<INT32>(hexes.begin(), hexes.begin() + NATIVE_SLOTS),
                       force);
        return false;
    }
    if (state.portActive)
    {
        // Back to the game's function: our hexes come off the background, the game shades the new ones (showing only
        // their rectangle), and one battlefield redraw shows the places we left.
        if (!LayerDrawable(combat, anchor, force))
            return false;
        RestorePortHexes(combat);
        if (hexes.empty())
            IntAt(LAST_SHADE_ANCHOR) = anchor;
        else
            CallShadeHexes(ShadeHexesOriginal(), combat, anchor, hexes, true);
        ++state.fullRedraws;
        THISCALL_7(void, REDRAW_BATTLEFIELD, combat, 1, 1, 0, 0, 1, 0);
        return true;
    }
    CallShadeHexes(ShadeHexesOriginal(), combat, anchor, hexes, force);
    return false;
}

// ---------------------------------------------------------------------------------------------------------------
// Bringing the layer in line with the cursor and the ERM shapes
// ---------------------------------------------------------------------------------------------------------------

BOOL8 SetContour(H3CombatCreature *stack, BOOL8 on)
{
    return THISCALL_2(BOOL8, SET_STACK_CONTOUR, stack, on);
}

// Returns true if an outline really changed (the stacks must then be redrawn). Wanted stacks are switched on every
// time, because spell aiming switches every outline off when it ends.
bool UpdateContours(const std::vector<H3CombatCreature *> &wanted)
{
    bool changed = false;
    std::vector<H3CombatCreature *> stillLit;
    for (H3CombatCreature *stack : state.litStacks)
        if (!Contains(wanted, stack) && SetContour(stack, FALSE))
            changed = true;
    for (H3CombatCreature *stack : wanted)
    {
        if (SetContour(stack, TRUE))
        {
            stillLit.push_back(stack);
            changed = true;
        }
        else if (Contains(state.litStacks, stack))
            stillLit.push_back(stack); // still ours; one the game outlined itself is not ours to switch off
    }
    state.litStacks = stillLit;
    return changed;
}

void DropContours()
{
    for (H3CombatCreature *stack : state.litStacks)
        SetContour(stack, FALSE);
    state.litStacks.clear();
}

void Commit(H3CombatManager *combat)
{
    state.dirty = false;
    state.cursorPending = false;
    if (!CanDraw(combat))
        return;
    if (!combat->doNotDrawShade)
    {
        // The background waits to be rebuilt (BattleShadow_HideMovement, game options, obstacles). The next redraw
        // would rebuild it and wipe shadows drawn now, so it is rebuilt first; under another window, later.
        if (!BattlefieldOnTop(combat))
        {
            state.dirty = true;
            return;
        }
        ClearLayerSilently(combat);
        ++state.fullRedraws;
        THISCALL_7(void, REDRAW_BATTLEFIELD, combat, 1, 1, 0, 0, 1, 0);
    }
    std::vector<H3CombatCreature *> skipped;
    for (const INT32 hex : state.skipHexes)
        if (H3CombatCreature *stack = LiveStackAt(combat, hex))
            skipped.push_back(stack);

    std::vector<INT32> hexes = state.hintHexes;
    std::vector<INT32> contourHexes = state.hintContourHexes;
    if (state.cursorHex >= 0)
        hexes.push_back(state.cursorHex); // what the game itself shades under the mouse
    if (state.followRadius >= 0)
    {
        std::vector<INT32> follow;
        AppendArea(combat, CursorHex(combat), state.followRadius, follow);
        hexes.insert(hexes.end(), follow.begin(), follow.end());
        if (state.followContour)
            contourHexes.insert(contourHexes.end(), follow.begin(), follow.end());
    }
    std::vector<INT32> wanted;
    for (const INT32 hex : hexes)
    {
        H3CombatCreature *stack = LiveStackAt(combat, hex);
        if (IsShadable(hex) && !Contains(state.excludedHexes, hex) && !(stack && Contains(skipped, stack)))
            wanted.push_back(hex);
    }
    SortUnique(wanted);
    // A stack is outlined through the hexes left in the area: a two-hex stack with one hex excluded still is.
    std::vector<H3CombatCreature *> stacks;
    for (const INT32 hex : contourHexes)
    {
        H3CombatCreature *stack = LiveStackAt(combat, hex);
        if (stack && !Contains(state.excludedHexes, hex) && !Contains(skipped, stack))
            stacks.push_back(stack);
    }
    SortUnique(stacks);

    const bool contoursChanged = UpdateContours(stacks);
    bool redrawn = false;
    if (wanted != ShownHexes())
        redrawn = Draw(combat, state.cursorHex, wanted, true);
    if (contoursChanged && !redrawn)
    {
        ++state.fullRedraws;
        THISCALL_7(void, REDRAW_BATTLEFIELD, combat, 1, 1, 0, 0, 1, 0);
    }
}

// Commits now, or when the current combat screen message is done (the usual case: from !?MM0).
void CommitSoon()
{
    state.dirty = true;
    if (state.procDepth <= 0)
        Commit(ActiveCombat());
}

// Shows or hides the movement range: the background is rebuilt once (what the game does when its options change,
// 0x4798F0), and HookDrawGrid leaves the movement range out while it is hidden. The shadows come off first and are
// drawn again on the new background by the commit (now, or when the battlefield is on top again).
void RebuildBackground(H3CombatManager *combat)
{
    if (!CanDraw(combat))
        return; // the next background the game builds follows the new setting
    ClearLayerSilently(combat);
    combat->doNotDrawShade = FALSE; // = rebuild the background at the next battlefield redraw
    CommitSoon();
}

void ClearHintShapes()
{
    state.hintHexes.clear();
    state.hintContourHexes.clear();
    state.hintAnchor = -1;
}

// Drops everything without touching the game (battle over, game left or loaded).
void Forget()
{
    ClearHintShapes();
    state.skipHexes.clear();
    state.excludedHexes.clear();
    state.followRadius = -1;
    state.followContour = false;
    state.dirty = false;
    state.cursorHex = -1;
    state.cursorPending = false;
    state.portActive = false;
    state.portHexes.clear();
    state.litStacks.clear();
    state.hideMovement = false;
    state.showMovementAtAction = false;
}

void BeginHintChange(H3CombatManager *combat)
{
    if (state.hintHexes.empty() && state.hintContourHexes.empty())
        state.hintAnchor = CursorHex(combat);
}

void AddHintHex(INT32 hex, bool contour)
{
    if (!IsShadable(hex))
        return;
    state.hintHexes.push_back(hex);
    if (contour)
        state.hintContourHexes.push_back(hex);
}

// ---------------------------------------------------------------------------------------------------------------
// ERM functions
// ---------------------------------------------------------------------------------------------------------------

// !!FU(BattleShadow_Follow):P(radius)/(contour);
// x1 = radius 0..4 around the hex under the mouse, -1 = off; x2 = 1 to outline the stacks inside.
void __stdcall OnFollow(Era::TEvent *)
{
    const INT32 radius = Era::x[1];
    state.followRadius = radius >= 0 && radius <= MAX_RADIUS ? radius : -1;
    state.followContour = Era::x[2] != 0;
    CommitSoon();
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
    for (const INT32 hex : area)
        AddHintHex(hex, Era::x[3] != 0);
    CommitSoon();
}

// !!FU(BattleShadow_Hexes):P(hex1)/.../(hex10)/(contour);
// x1..x10 = hexes to shade, -1 = unused slot; x11 = 1 to outline the stacks on those hexes.
void __stdcall OnHexes(Era::TEvent *)
{
    H3CombatManager *combat = ActiveCombat();
    if (!combat)
        return;
    BeginHintChange(combat);
    const bool contour = Era::x[HEX_SLOTS + 1] != 0;
    for (INT32 slot = 1; slot <= HEX_SLOTS; ++slot)
        AddHintHex(Era::x[slot], contour);
    CommitSoon();
}

// !!FU(BattleShadow_Stacks):P(hex1)/.../(hex10)/(contour);
// x1..x10 = a hex of each stack to shade whole (both hexes of a two-hex stack), -1 = unused slot;
// x11 = 1 to outline those stacks. For spells with several targets, such as chain spells.
void __stdcall OnStacks(Era::TEvent *)
{
    H3CombatManager *combat = ActiveCombat();
    if (!combat)
        return;
    BeginHintChange(combat);
    const bool contour = Era::x[HEX_SLOTS + 1] != 0;
    for (INT32 slot = 1; slot <= HEX_SLOTS; ++slot)
    {
        H3CombatCreature *stack = LiveStackAt(combat, Era::x[slot]);
        if (!stack)
            continue;
        for (INT32 hex = 0; hex < BATTLE_HEXES; ++hex)
            if (combat->squares[hex].GetCreature() == stack)
                AddHintHex(hex, contour);
    }
    CommitSoon();
}

// !!FU(BattleShadow_SkipStacks):P(hex1)/.../(hex16);
// Stacks standing on these hexes are left out of every shadow and outline (the spell does not affect them), the
// game's own hex under the mouse included. -1 = unused slot. Kept until the next !?MM0.
void __stdcall OnSkipStacks(Era::TEvent *)
{
    for (INT32 slot = 1; slot <= SKIP_SLOTS; ++slot)
        if (IsShadable(Era::x[slot]))
            state.skipHexes.push_back(Era::x[slot]);
    CommitSoon();
}

// !!FU(BattleShadow_SkipHexes):P(hex1)/.../(hex16);
// These hexes are left out of every shadow, the game's own hex under the mouse included, and outline nothing (a stack
// is still outlined through another of its hexes inside the area). For areas with a hole, such as Frost Ring:
// BattleShadow_Radius (hex, 1) + BattleShadow_SkipHexes (hex). -1 = unused slot. Kept until the next !?MM0.
void __stdcall OnSkipHexes(Era::TEvent *)
{
    for (INT32 slot = 1; slot <= SKIP_SLOTS; ++slot)
        if (IsShadable(Era::x[slot]))
            state.excludedHexes.push_back(Era::x[slot]);
    CommitSoon();
}

// !!FU(BattleShadow_HideMovement):P(hide)/(untilAction);
// x1 = 1 hides the movement range shade of the battlefield (as the game does while a spell is aimed), 0 shows it
// again; x2 = 1 shows it again by itself when the next battle action starts. Only the picture changes: the game's
// "movement shadow" option and the stack's movement stay as they are. It stays hidden, turn after turn, until shown
// again (or the battle ends). Calling it again with the same value does nothing, so it can be called in every !?MM0.
void __stdcall OnHideMovement(Era::TEvent *)
{
    const bool hide = Era::x[1] != 0;
    state.showMovementAtAction = hide && Era::x[2] != 0;
    if (hide == state.hideMovement)
        return;
    state.hideMovement = hide;
    RebuildBackground(ActiveCombat());
}

// !!FU(BattleShadow_Clear):P;  Removes every shadow, follow mode included (not BattleShadow_HideMovement).
void __stdcall OnClear(Era::TEvent *)
{
    ClearHintShapes();
    state.skipHexes.clear();
    state.excludedHexes.clear();
    state.followRadius = -1;
    state.followContour = false;
    CommitSoon();
}

// !!FU(BattleShadow_Debug):P?(version)/?(shown)/?(hint)/?(follow)/?(skip)/?(native)/?(own)/?(full)/?(cursor)/
//                         ?(option)/?(hooked)/?(movementHidden)/?(excluded);
// x1 = plugin version (0 if this plugin did not answer), x2 = hexes in the shadow layer now, x3 = hexes asked for in
// the current mouse hint, x4 = follow radius (-1 = off), x5 = skipped stack hexes, x6 = drawings by the game's
// function, x7 = drawings with the plugin's buffer (more than 19 hexes), x8 = full battlefield redraws, x9 = hex under
// the cursor as the plugin sees it, x10 = the game's "cursor shadow" option (0 = off: no shadows, like Fireball),
// x11 = 1 if the mouse-move hook is in place (one drawing per hex change), x12 = 1 while BattleShadow_HideMovement
// hides the movement range, x13 = hexes received by BattleShadow_SkipHexes.
void __stdcall OnDebug(Era::TEvent *)
{
    H3CombatManager *combat = ActiveCombat();
    Era::x[1] = PLUGIN_VERSION;
    Era::x[2] = INT32(ShownHexes().size());
    Era::x[3] = INT32(state.hintHexes.size());
    Era::x[4] = state.followRadius;
    Era::x[5] = INT32(state.skipHexes.size());
    Era::x[6] = state.nativeDraws;
    Era::x[7] = state.portDraws;
    Era::x[8] = state.fullRedraws;
    Era::x[9] = combat ? CursorHex(combat) : -1;
    Era::x[10] = DwordAt(CURSOR_SHADOW_OPTION) ? 1 : 0;
    Era::x[11] = mouseMoveHookInstalled ? 1 : 0;
    Era::x[12] = state.hideMovement ? 1 : 0;
    Era::x[13] = INT32(state.excludedHexes.size());
}

// ERA calls plugin handlers of a trigger before its ERM code: every !?MM0 starts with no hint shapes and no
// skipped stacks. Nothing is drawn yet; the result is drawn once, when the combat screen message is done.
void __stdcall OnBattleMouseHint(Era::TEvent *)
{
    ClearHintShapes();
    state.skipHexes.clear();
    state.excludedHexes.clear();
    H3CombatManager *combat = ActiveCombat();
    state.hintAnchor = combat ? CursorHex(combat) : -1;
    state.dirty = true;
}

// The action takes the cursor shadow off by itself (0x4786B0 calls 0x493F10(-1, 1), which reaches HookShadeHexes).
// Outlines are switched off without a redraw: the action redraws the battlefield. A movement range hidden with
// "until the next action" comes back with the next background the game builds.
void __stdcall OnBeforeBattleAction(Era::TEvent *)
{
    ClearHintShapes();
    state.skipHexes.clear();
    state.excludedHexes.clear();
    state.followRadius = -1;
    state.followContour = false;
    state.dirty = false;
    state.cursorPending = false;
    DropContours();
    if (state.hideMovement && state.showMovementAtAction)
    {
        state.hideMovement = false;
        state.showMovementAtAction = false;
        H3CombatManager *combat = ActiveCombat();
        if (CanDraw(combat))
        {
            ClearLayerSilently(combat);
            combat->doNotDrawShade = FALSE;
        }
    }
}

void __stdcall OnLeaveBattleContext(Era::TEvent *)
{
    Forget();
}

void __stdcall OnBattleStart(Era::TEvent *)
{
    Forget();
    state.nativeDraws = 0;
    state.portDraws = 0;
    state.fullRedraws = 0;
}

// ---------------------------------------------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------------------------------------------

// The mouse-move branch asks for the one-hex cursor shadow before the battle hint (!?MM0) is built. The request is
// kept and drawn together with the ERM shapes when the message is done, so a hex change is drawn once.
void __stdcall HookMouseMoveShade(HiHook *hook, H3CombatManager *combat, INT32 hex, INT32 flag)
{
    if (state.procDepth <= 0 || combat != ActiveCombat())
    {
        THISCALL_3(void, hook->GetDefaultFunc(), combat, hex, flag);
        return;
    }
    if (DwordAt(ACTION_IN_PROGRESS) && !(flag & 0xFF))
        return; // what 0x493F10 does in this case
    state.cursorHex = IsShadable(hex) ? hex : -1;
    state.cursorPending = true;
}

// Every other user of the layer: Fireball-type spell aiming, Force Field / Fire Wall placing, the area-shot picker,
// the game clearing the cursor shadow (battle action, options). They own the layer from now on: the ERM shapes of
// the last mouse hint are dropped (the next !?MM0 brings new ones). When the layer holds something the caller does
// not know about (our shapes), the drawing is forced, so nothing of ours can stay behind.
void __stdcall HookShadeHexes(HiHook *hook, H3CombatManager *combat, INT32 anchor, H3Vector<INT32> *hexes, INT32 force)
{
    // Without the mouse-move hook (another mod changed that call), the one-hex cursor shadow arrives here: keep it as
    // the cursor request, so the next commit still shows it together with the ERM shapes.
    const bool cursorCall = !mouseMoveHookInstalled && state.procDepth > 0 && anchor >= 0 && hexes &&
                            hexes->Size() == 1 && (*hexes)[0] == anchor;
    if (cursorCall)
    {
        state.cursorHex = anchor;
        state.cursorPending = true;
    }
    else
    {
        ClearHintShapes();
        state.skipHexes.clear();
        state.excludedHexes.clear();
        state.cursorHex = -1;
        state.cursorPending = false;
    }
    if (!combat || !hexes)
    {
        THISCALL_4(void, hook->GetDefaultFunc(), combat, anchor, hexes, force);
        return;
    }
    std::vector<INT32> requested;
    for (UINT32 i = 0; i < hexes->Size(); ++i)
    {
        const INT32 hex = (*hexes)[i];
        if (hex >= 0 && hex < BATTLE_HEXES)
            requested.push_back(hex);
    }
    SortUnique(requested);
    const bool differs = requested != ShownHexes();
    const bool forced = (force & 0xFF) != 0 || differs;
    if (!state.portActive && INT32(requested.size()) <= NATIVE_SLOTS)
    {
        // The game's own call, as it was made (its vector, its order).
        THISCALL_4(void, hook->GetDefaultFunc(), combat, anchor, hexes, BOOL8(forced ? 1 : 0));
        ++state.nativeDraws;
        return;
    }
    Draw(combat, anchor, requested, forced);
}

// The movement range part of the background (0x4934B0) reads the "movement shadow" option. While the movement
// range is hidden, the option reads 0 for this one call and is put back right after it: the game's setting, the
// options dialog and the stack's movement never see the change. The grid and the tactics zone stay as they are.
INT __stdcall HookDrawGrid(HiHook *hook, H3CombatManager *combat, INT32 full, INT32 recompute)
{
    if (!state.hideMovement)
        return THISCALL_3(INT, hook->GetDefaultFunc(), combat, full, recompute);
    const UINT option = DwordAt(MOVEMENT_SHADOW_OPTION);
    DwordAt(MOVEMENT_SHADOW_OPTION) = 0;
    const INT result = THISCALL_3(INT, hook->GetDefaultFunc(), combat, full, recompute);
    DwordAt(MOVEMENT_SHADOW_OPTION) = option;
    return result;
}

// Every combat screen message passes here, mouse moves included: follow mode reacts as fast as the cursor, and the
// cursor request plus the shapes of the !?MM0 inside this message are drawn once, right after it.
INT __stdcall HookCombatProcedure(HiHook *hook, H3CombatManager *combat, H3Msg *msg)
{
    ++state.procDepth;
    const INT result = THISCALL_2(INT, hook->GetDefaultFunc(), combat, msg);
    --state.procDepth;
    if (!combat || combat != ActiveCombat())
    {
        state.cursorPending = false;
        return result;
    }
    const bool mouseMoved = msg && msg->IsMouseOver();
    // The mouse reached another hex without a new !?MM0: the old hint shapes no longer apply.
    if (mouseMoved && !(state.hintHexes.empty() && state.hintContourHexes.empty()) &&
        CursorHex(combat) != state.hintAnchor)
    {
        ClearHintShapes();
        state.dirty = true;
    }
    if (state.cursorPending || state.dirty || (mouseMoved && state.followRadius >= 0))
        Commit(combat);
    return result;
}
} // namespace

void RegisterEvents()
{
    Era::RegisterHandler(OnFollow, "BattleShadow_Follow");
    Era::RegisterHandler(OnRadius, "BattleShadow_Radius");
    Era::RegisterHandler(OnHexes, "BattleShadow_Hexes");
    Era::RegisterHandler(OnStacks, "BattleShadow_Stacks");
    Era::RegisterHandler(OnSkipStacks, "BattleShadow_SkipStacks");
    Era::RegisterHandler(OnSkipHexes, "BattleShadow_SkipHexes");
    Era::RegisterHandler(OnClear, "BattleShadow_Clear");
    Era::RegisterHandler(OnHideMovement, "BattleShadow_HideMovement");
    Era::RegisterHandler(OnDebug, "BattleShadow_Debug");
    Era::RegisterHandler(OnBattleMouseHint, MOUSE_HINT_EVENT);
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
    patcher->WriteHiHook(COMBAT_DLG_PROC, SPLICE_, THISCALL_, HookCombatProcedure);
    shadeHook = patcher->WriteHiHook(SHADE_HEXES, SPLICE_, THISCALL_, HookShadeHexes);
    patcher->WriteHiHook(DRAW_MOVEMENT_GRID, SPLICE_, THISCALL_, HookDrawGrid);
    // Only when the call site is the one this plugin was written for (call 0x493F10, not already taken).
    const INT32 relative = INT32(CURSOR_SHADE) - INT32(MOUSE_MOVE_SHADE_CALL + 5);
    if (ByteAt(MOUSE_MOVE_SHADE_CALL) == 0xE8 && IntAt(MOUSE_MOVE_SHADE_CALL + 1) == relative)
    {
        patcher->WriteHiHook(MOUSE_MOVE_SHADE_CALL, CALL_, THISCALL_, HookMouseMoveShade);
        mouseMoveHookInstalled = true;
    }
}
}
