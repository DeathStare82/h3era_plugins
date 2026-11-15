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
constexpr INT32 FREE_HEX_SLOTS = 10; // x1..x10 of BattleShadow_Hexes
constexpr const char *MOUSE_HINT_EVENT = "OnBattleMouseHint"; // !?MM0
// ERA fires "<trigger name>_Quit" after all handlers of a trigger when a function with that name exists.
constexpr const char *MOUSE_HINT_QUIT_EVENT = "OnBattleMouseHint_Quit";

// Native calls already used in this repository with the same arguments:
// Combat_SwitchingCreatureAction/aoe_attack.cpp (0x493F10, 0x493A20, 0x43ED00, 0x493FC0) and
// ERA_SystemOptionsExtension/CombatSettings.cpp (0x493EF0). H3API names the drawn list H3BattleShadowHexes.
constexpr UINT SET_CURSOR_SHADOW = 0x493F10;  // thiscall(combat, hex, flag); hex -1 clears
constexpr UINT SET_AREA_SHADOW = 0x493A20;    // thiscall(combat, hex, H3Vector<INT32> *hexes, BOOL8)
constexpr UINT CLEAR_SHADOWS = 0x493EF0;      // cdecl()
constexpr UINT SET_STACK_CONTOUR = 0x43ED00;  // thiscall(stack, BOOL8 on) -> BOOL8 changed
constexpr UINT REDRAW_BATTLEFIELD = 0x493FC0; // thiscall(combat, flip, setRedraws, useRedraws, wait, background, doWait)

struct ShadowState
{
    std::vector<INT32> hexes;                 // requested by ERM, unique
    std::vector<INT32> appended;              // entries this plugin currently owns in the native list
    std::vector<H3CombatCreature *> contours; // stacks whose contour this plugin switched on
    INT32 anchorHex = -1;                     // cursor hex the current shadows belong to
    bool deferRedraw = false;                 // inside !?MM0: redraw once from the _Quit handler
    bool quitEventSeen = false;               // this ERA build fires OnBattleMouseHint_Quit
    std::vector<INT32> hintStartHexes;        // state before the current !?MM0, to skip identical redraws
    std::vector<H3CombatCreature *> hintStartContours;
};
ShadowState state;

H3CombatManager *ActiveCombat()
{
    H3CombatManager *combat = H3CombatManager::Get();
    return combat && combat->dlg ? combat : nullptr;
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

void Redraw(H3CombatManager *combat)
{
    if (combat && !combat->IsHiddenBattle())
        THISCALL_7(void, REDRAW_BATTLEFIELD, combat, TRUE, TRUE, FALSE, 0, TRUE, FALSE);
}

H3Vector<INT32> &NativeHexes()
{
    return *H3BattleShadowHexes::Get();
}

void RemoveNativeHexes()
{
    H3Vector<INT32> &native = NativeHexes();
    for (const INT32 hex : state.appended)
    {
        // Our entries were appended after the game's own ones, so search from the end.
        for (UINT32 i = native.Size(); i-- > 0;)
        {
            if (native[i] == hex)
            {
                native.Remove(i);
                break;
            }
        }
    }
    state.appended.clear();
}

void AppendNativeHexes()
{
    H3Vector<INT32> &native = NativeHexes();
    for (const INT32 hex : state.hexes)
    {
        if (std::find(native.begin(), native.end(), hex) != native.end())
            continue; // already shaded by the game or by an earlier call
        native.Add(hex);
        state.appended.push_back(hex);
    }
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
    state.appended.clear();
    state.contours.clear();
    state.anchorHex = -1;
}

// Removes our list entries and contours. Returns true when something had been shown.
bool Reset()
{
    const bool hadState = HasState();
    RemoveNativeHexes(); // the list is global, so this is valid outside the battle screen too
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
    if (!changed)
        return;
    AppendNativeHexes();
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
    Reset();
    LeaveHint();
}

void __stdcall OnLeaveBattleContext(Era::TEvent *)
{
    RemoveNativeHexes();
    Forget();
    LeaveHint();
}

void __stdcall OnBattleStart(Era::TEvent *event)
{
    OnLeaveBattleContext(event);
    INT32 quitId = 0;
    if (Era::AllocErmFunc)
        Era::AllocErmFunc(MOUSE_HINT_QUIT_EVENT, quitId); // lets ERA fire _Quit from the first hint
}

// The game rebuilt its shadow list. Keep ours only while the cursor is still on the hex they belong to;
// otherwise the mouse moved without !?MM0 (for example while choosing a spell target).
void RestoreAfterNativeUpdate(H3CombatManager *combat)
{
    if (combat && combat->dlg && CursorHex(combat) == state.anchorHex)
    {
        AppendNativeHexes();
        ReapplyContours();
    }
    else
        Reset();
}

int __stdcall HookSetCursorShadow(HiHook *hook, H3CombatManager *combat, INT32 hex, INT32 flag)
{
    if (!HasState())
        return THISCALL_3(int, hook->GetDefaultFunc(), combat, hex, flag);
    RemoveNativeHexes();
    const int result = THISCALL_3(int, hook->GetDefaultFunc(), combat, hex, flag);
    RestoreAfterNativeUpdate(combat);
    return result;
}

int __stdcall HookSetAreaShadow(HiHook *hook, H3CombatManager *combat, INT32 hex, H3Vector<INT32> *hexes, INT32 flag)
{
    if (!HasState())
        return THISCALL_4(int, hook->GetDefaultFunc(), combat, hex, hexes, flag);
    RemoveNativeHexes();
    const int result = THISCALL_4(int, hook->GetDefaultFunc(), combat, hex, hexes, flag);
    RestoreAfterNativeUpdate(combat);
    return result;
}

int __stdcall HookClearShadows(HiHook *hook)
{
    RemoveNativeHexes(); // the requested state stays; the next native update may restore it
    return CDECL_0(int, hook->GetDefaultFunc());
}
} // namespace

void RegisterEvents()
{
    Era::RegisterHandler(OnRadius, "BattleShadow_Radius");
    Era::RegisterHandler(OnHexes, "BattleShadow_Hexes");
    Era::RegisterHandler(OnClear, "BattleShadow_Clear");
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
    patcher->WriteHiHook(SET_CURSOR_SHADOW, SPLICE_, THISCALL_, HookSetCursorShadow);
    patcher->WriteHiHook(SET_AREA_SHADOW, SPLICE_, THISCALL_, HookSetAreaShadow);
    patcher->WriteHiHook(CLEAR_SHADOWS, SPLICE_, CDECL_, HookClearShadows);
}
}
