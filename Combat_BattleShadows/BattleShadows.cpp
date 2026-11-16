#include "framework.h"
#include "BattleShadows.h"
#include <algorithm>
#include <cstdint>
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
constexpr INT32 HEX_SLOTS = 10;  // BattleShadow_Hexes / BattleShadow_Stacks: x1..x10, x11 = contour
constexpr INT32 SKIP_SLOTS = 16; // BattleShadow_SkipStacks: x1..x16
constexpr INT32 PLUGIN_VERSION = 6;
constexpr const char *MOUSE_HINT_EVENT = "OnBattleMouseHint"; // !?MM0
// ERA fires "<trigger name>_Quit" after all handlers of a trigger when a function with that name exists.
constexpr const char *MOUSE_HINT_QUIT_EVENT = "OnBattleMouseHint_Quit";

// How the shadows reach the screen (the method of Interface_SpellsDescription/SpellDescriptionOverlay.cpp, which
// was checked against h3era.exe): the plugin darkens hexes directly in the window canvas, the picture the game has
// already composed, and shows only the changed rectangle with WindowManager::H3Redraw. The battlefield itself is not
// redrawn, so moving a shadow costs about as much as moving the cursor. Native drawing that covers our pixels
// (stack animations, redraws) is followed by repainting the layer.
constexpr UINT COMBAT_DLG_PROC = 0x4746B0;    // thiscall(combat, H3Msg *) -> INT: every combat screen message
constexpr UINT DRAW_STACK = 0x43DE60;         // thiscall(stack, x, y, BOOL numberBoxOnly), ret 0Ch
constexpr UINT PRESENT_RECT = 0x603190;       // thiscall(windowManager, x, y, width, height)
constexpr UINT SET_STACK_CONTOUR = 0x43ED00;  // thiscall(stack, BOOL8 on) -> BOOL8 changed
constexpr UINT REDRAW_BATTLEFIELD = 0x493FC0; // thiscall(combat, flip, setRedraws, useRedraws, wait, background, doWait)
constexpr UINT COLLECTING_BOUNDS = 0x13D34;   // combat manager DWORD: stack drawing only measures, draws nothing
// CCellShd.pcx is the hex mask. Its pixels use the Heroes 3 special palette slots, which mean "darken" rather than
// a color (copying its raw colors gives solid blue hexes). Amount to darken, out of 256, per slot; VCMI uses the
// same table for this picture: 0 transparent, 1..2 shadow border, 3..4 shadow body.
constexpr UINT8 SHADOW_ALPHA[] = {0, 32, 64, 128, 128};
constexpr UINT8 SHADOW_ALPHA_OTHER = 128;
constexpr INT32 HEX_WIDTH = 45;
constexpr INT32 HEX_HEIGHT = 52;

struct PixelRect
{
    INT32 x = 0;
    INT32 y = 0;
    INT32 width = 0;
    INT32 height = 0;
    bool Empty() const
    {
        return width <= 0 || height <= 0;
    }
    bool Intersects(const PixelRect &other) const
    {
        return !Empty() && !other.Empty() && x < other.x + other.width && other.x < x + width &&
               y < other.y + other.height && other.y < y + height;
    }
};

PixelRect ClipRect(INT32 x, INT32 y, INT32 width, INT32 height, INT32 canvasWidth, INT32 canvasHeight)
{
    const INT32 left = std::max(x, 0);
    const INT32 top = std::max(y, 0);
    const INT32 right = std::min(x + width, canvasWidth);
    const INT32 bottom = std::min(y + height, canvasHeight);
    PixelRect rect;
    if (left < right && top < bottom)
    {
        rect.x = left;
        rect.y = top;
        rect.width = right - left;
        rect.height = bottom - top;
    }
    return rect;
}

PixelRect UnionRect(const PixelRect &a, const PixelRect &b)
{
    if (a.Empty())
        return b;
    if (b.Empty())
        return a;
    PixelRect rect;
    rect.x = std::min(a.x, b.x);
    rect.y = std::min(a.y, b.y);
    rect.width = std::max(a.x + a.width, b.x + b.width) - rect.x;
    rect.height = std::max(a.y + a.height, b.y + b.height) - rect.y;
    return rect;
}

// Adapted from Interface_SpellsDescription/SpellDescriptionOverlayPixels.h: every rectangle is saved BEFORE
// painting, the painted result is recorded AFTER painting, and only pixels that still hold our paint are put back.
// So native drawing done in between (a stack animation over a shaded hex) is never erased.
class PixelBackup
{
    struct Patch
    {
        PixelRect rect;
        size_t offset;
    };
    std::vector<Patch> patches;
    std::vector<UINT8> original;
    std::vector<UINT8> painted;
    INT32 bytesPerPixel = 0;
    bool recorded = false;

    template <class Pixel> static void RestoreRow(PUINT8 destination, const UINT8 *source, const UINT8 *overlay, INT32 width)
    {
        const size_t rowSize = size_t(width) * sizeof(Pixel);
        if (std::memcmp(destination, overlay, rowSize) == 0)
        {
            std::memcpy(destination, source, rowSize);
            return;
        }
        for (INT32 column = 0; column < width; ++column)
        {
            const size_t offset = size_t(column) * sizeof(Pixel);
            Pixel current, expected;
            std::memcpy(&current, destination + offset, sizeof(Pixel));
            std::memcpy(&expected, overlay + offset, sizeof(Pixel));
            if (current == expected)
                std::memcpy(destination + offset, source + offset, sizeof(Pixel));
        }
    }

    void Copy(const UINT8 *buffer, INT32 stride, std::vector<UINT8> &into) const
    {
        for (const Patch &patch : patches)
        {
            const size_t rowSize = size_t(patch.rect.width) * bytesPerPixel;
            for (INT32 row = 0; row < patch.rect.height; ++row)
                std::memcpy(into.data() + patch.offset + row * rowSize,
                            buffer + size_t(patch.rect.y + row) * stride + size_t(patch.rect.x) * bytesPerPixel,
                            rowSize);
        }
    }

  public:
    void Release()
    {
        patches.clear();
        recorded = false;
        std::vector<UINT8>().swap(original);
        std::vector<UINT8>().swap(painted);
    }

    void Capture(const UINT8 *buffer, INT32 stride, INT32 pixelSize, const std::vector<PixelRect> &rects)
    {
        patches.clear();
        recorded = false;
        bytesPerPixel = pixelSize;
        size_t size = 0;
        for (const PixelRect &rect : rects)
        {
            patches.push_back({rect, size});
            size += size_t(rect.width) * rect.height * pixelSize;
        }
        original.resize(size); // capacity is kept between moves
        painted.resize(size);
        Copy(buffer, stride, original);
    }

    void RecordPaint(const UINT8 *buffer, INT32 stride)
    {
        Copy(buffer, stride, painted);
        recorded = true;
    }

    void RestoreUnchanged(PUINT8 buffer, INT32 stride) const
    {
        if (!recorded)
            return;
        for (const Patch &patch : patches)
        {
            const size_t rowSize = size_t(patch.rect.width) * bytesPerPixel;
            for (INT32 row = 0; row < patch.rect.height; ++row)
            {
                PUINT8 destination = buffer + size_t(patch.rect.y + row) * stride + size_t(patch.rect.x) * bytesPerPixel;
                const size_t offset = patch.offset + row * rowSize;
                if (bytesPerPixel == 4)
                    RestoreRow<UINT32>(destination, original.data() + offset, painted.data() + offset, patch.rect.width);
                else
                    RestoreRow<UINT16>(destination, original.data() + offset, painted.data() + offset, patch.rect.width);
            }
        }
    }
};

struct ShadowState
{
    // Set by ERM during the current mouse hint; dropped before every !?MM0.
    std::vector<INT32> hintHexes;
    std::vector<INT32> hintContourHexes;
    std::vector<INT32> skipHexes; // stacks standing on these hexes are never shaded or outlined
    INT32 hintAnchor = -1;        // cursor hex the hint shapes belong to
    bool inHint = false;          // between OnBattleMouseHint and its _Quit
    // Follow mode: a radius around the hex under the mouse, kept until switched off or the next battle action.
    INT32 followRadius = -1;
    bool followContour = false;
    bool dirty = false;
    // What is shown now.
    std::vector<INT32> shownHexes;                 // sorted
    PixelRect shownBounds;                         // canvas rectangle around shownHexes
    std::vector<H3CombatCreature *> contourStacks; // sorted
    std::vector<H3CombatCreature *> litStacks;     // contours switched on by this plugin
    // Canvas layer.
    PixelBackup pixels;
    H3LoadedPcx16 *canvas = nullptr;
    PUINT8 canvasBuffer = nullptr;
    INT32 canvasStride = 0;
    INT32 canvasWidth = 0;
    INT32 canvasHeight = 0;
    bool applied = false;
    bool painting = false;
    INT32 groundMode = -1; // -1 not decided yet, 1 darken ground only, 0 darken every masked pixel
    // Diagnostics.
    INT32 composes = 0;
    INT32 fastPresents = 0;
    INT32 fullRedraws = 0;
};
ShadowState state;
std::vector<PixelRect> scratchRects;

H3CombatManager *ActiveCombat()
{
    H3CombatManager *combat = H3CombatManager::Get();
    return combat && combat->dlg ? combat : nullptr;
}

bool VisibleBattle(H3CombatManager *combat)
{
    H3WindowManager *windows = H3WindowManager::Get();
    return combat && combat == H3CombatManager::Get() && combat->dlg && !combat->finished &&
           !combat->IsHiddenBattle() && windows && windows->lastDlg == combat->dlg;
}

bool CollectingBounds(H3CombatManager *combat)
{
    return combat && IntAt(reinterpret_cast<UINT_PTR>(combat) + COLLECTING_BOUNDS) != 0;
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

INT32 BytesPerPixel(const H3LoadedPcx16 *buffer)
{
    return buffer->scanlineSize >= buffer->width * 4 ? 4 : 2; // HD 32-bit mode or the game's 16-bit mode
}

PixelRect CellRect(H3CombatManager *combat, const H3LoadedPcx16 *canvas, INT32 hex)
{
    const H3CombatSquare &square = combat->squares[hex];
    return ClipRect(square.left + combat->dlg->GetX(), square.top + combat->dlg->GetY(), HEX_WIDTH, HEX_HEIGHT,
                    canvas->width, canvas->height);
}

// Puts back the canvas pixels under our last paint, keeping every pixel the game has drawn since.
void RemoveLayer()
{
    if (!state.applied)
        return;
    state.applied = false;
    H3WindowManager *windows = H3WindowManager::Get();
    H3LoadedPcx16 *canvas = windows ? windows->GetDrawBuffer() : nullptr;
    // The saved pointers are only an identity check; an old surface is never touched.
    if (canvas && canvas == state.canvas && canvas->buffer == state.canvasBuffer &&
        canvas->scanlineSize == state.canvasStride && canvas->width == state.canvasWidth &&
        canvas->height == state.canvasHeight)
        state.pixels.RestoreUnchanged(canvas->buffer, canvas->scanlineSize);
}

UINT32 DarkenChannel(UINT32 color, UINT32 mask, UINT32 keep)
{
    return (((color & mask) * keep) >> 8) & mask;
}


// Calls visit(pixel, alpha, isGround) for every canvas pixel under the CCellShd mask of the shown hexes.
// A canvas pixel counts as ground when it equals the battlefield background layer (combat->drawBuffer) at the same
// place; stacks and obstacles drawn over the ground differ, and so does a pixel this pass has already darkened.
template <typename Visit>
void ForEachMaskedPixel(H3CombatManager *combat, H3LoadedPcx16 *canvas, const H3LoadedPcx *mask, INT32 bpp, Visit visit)
{
    const INT32 dx = combat->dlg->GetX();
    const INT32 dy = combat->dlg->GetY();
    const H3LoadedPcx16 *ground = combat->drawBuffer;
    const bool canCompare = ground && ground->buffer && BytesPerPixel(ground) == bpp;
    const UINT32 compareMask = bpp == 4 ? 0x00FFFFFF : 0xFFFF;
    const INT32 maskWidth = std::min(mask->width, HEX_WIDTH);
    const INT32 maskHeight = std::min(mask->height, HEX_HEIGHT);
    for (const INT32 hex : state.shownHexes)
    {
        const H3CombatSquare &square = combat->squares[hex];
        for (INT32 row = 0; row < maskHeight; ++row)
        {
            const INT32 groundY = square.top + row;
            const INT32 canvasY = groundY + dy;
            if (canvasY < 0 || canvasY >= canvas->height)
                continue;
            const UINT8 *maskRow = mask->GetRow(row);
            PUINT8 canvasRow = canvas->GetRow(canvasY);
            const bool groundRowOk = canCompare && groundY >= 0 && groundY < ground->height;
            for (INT32 column = 0; column < maskWidth; ++column)
            {
                const UINT8 slot = maskRow[column];
                const UINT32 alpha = size_t(slot) < sizeof(SHADOW_ALPHA) ? SHADOW_ALPHA[slot] : SHADOW_ALPHA_OTHER;
                const INT32 groundX = square.left + column;
                const INT32 canvasX = groundX + dx;
                if (!alpha || canvasX < 0 || canvasX >= canvas->width)
                    continue;
                PUINT8 pixel = canvasRow + canvasX * bpp;
                bool isGround = false;
                if (groundRowOk && groundX >= 0 && groundX < ground->width)
                {
                    UINT32 color = 0;
                    UINT32 groundColor = 0;
                    std::memcpy(&color, pixel, bpp);
                    std::memcpy(&groundColor, ground->buffer + size_t(groundY) * ground->scanlineSize + groundX * bpp, bpp);
                    isGround = ((color ^ groundColor) & compareMask) == 0;
                }
                visit(pixel, alpha, isGround);
            }
        }
    }
}

// Darkens the ground under our hexes, like the game's own shadows (stacks and obstacles stay bright).
void PaintHexes(H3CombatManager *combat, H3LoadedPcx16 *canvas, const H3LoadedPcx *mask, INT32 bpp)
{
    // Decided once per battle: if the background layer does not line up with the canvas (under 15% of the masked
    // pixels match), darken every masked pixel instead of only the ground.
    if (state.groundMode < 0)
    {
        INT32 masked = 0;
        INT32 matching = 0;
        ForEachMaskedPixel(combat, canvas, mask, bpp, [&](PUINT8, UINT32, bool isGround) {
            ++masked;
            matching += isGround ? 1 : 0;
        });
        if (masked > 0)
            state.groundMode = matching * 100 >= masked * 15 ? 1 : 0;
    }
    const bool groundOnly = state.groundMode == 1;

    UINT32 red = H3RedMask::Get();
    UINT32 green = H3GreenMask::Get();
    UINT32 blue = H3BlueMask::Get();
    if (!red || !green || !blue)
    {
        red = 0xF800; // RGB565
        green = 0x07E0;
        blue = 0x001F;
    }
    ForEachMaskedPixel(combat, canvas, mask, bpp, [&](PUINT8 pixel, UINT32 alpha, bool isGround) {
        if (groundOnly && !isGround)
            return;
        const UINT32 keep = 256 - alpha;
        if (bpp == 4)
        {
            for (INT32 channel = 0; channel < 3; ++channel) // B, G, R; the alpha byte stays
                pixel[channel] = UINT8((pixel[channel] * keep) >> 8);
            return;
        }
        UINT16 color = 0;
        std::memcpy(&color, pixel, sizeof(color));
        color = UINT16(DarkenChannel(color, red, keep) | DarkenChannel(color, green, keep) | DarkenChannel(color, blue, keep));
        std::memcpy(pixel, &color, sizeof(color));
    });
}

// Paints the layer into the canvas, after taking our previous paint off. Safe to call again at any time.
void ComposeLayer(H3CombatManager *combat)
{
    if (state.painting || state.shownHexes.empty() || !VisibleBattle(combat) || CollectingBounds(combat))
        return;
    RemoveLayer();
    H3LoadedPcx16 *canvas = H3WindowManager::Get()->GetDrawBuffer();
    const H3LoadedPcx *mask = combat->CCellShdPcx;
    if (!canvas || !canvas->buffer || !mask || !mask->buffer)
        return;
    const INT32 bpp = BytesPerPixel(canvas);
    scratchRects.clear();
    for (const INT32 hex : state.shownHexes)
    {
        const PixelRect rect = CellRect(combat, canvas, hex);
        if (!rect.Empty())
            scratchRects.push_back(rect);
    }
    if (scratchRects.empty())
        return;
    state.pixels.Capture(canvas->buffer, canvas->scanlineSize, bpp, scratchRects);
    state.painting = true;
    PaintHexes(combat, canvas, mask, bpp);
    state.painting = false;
    state.pixels.RecordPaint(canvas->buffer, canvas->scanlineSize);
    state.canvas = canvas;
    state.canvasBuffer = canvas->buffer;
    state.canvasStride = canvas->scanlineSize;
    state.canvasWidth = canvas->width;
    state.canvasHeight = canvas->height;
    state.applied = true;
    ++state.composes;
}

// Shows one canvas rectangle on the screen without redrawing the battlefield.
void PresentRect(const PixelRect &rect)
{
    if (rect.Empty())
        return;
    state.painting = true; // the layer is already in the canvas; PresentWindow must not repaint it
    H3WindowManager::Get()->H3Redraw(rect.x, rect.y, rect.width, rect.height);
    state.painting = false;
    ++state.fastPresents;
}

PixelRect BoundsOf(H3CombatManager *combat, const std::vector<INT32> &hexes)
{
    PixelRect bounds;
    H3LoadedPcx16 *canvas = H3WindowManager::Get()->GetDrawBuffer();
    if (!canvas)
        return bounds;
    for (const INT32 hex : hexes)
        bounds = UnionRect(bounds, CellRect(combat, canvas, hex));
    return bounds;
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

// Brings the screen in line with the requested shapes; does nothing when they did not change.
// Moving shadows only touch the canvas and the changed rectangle. A changed outline needs the stacks redrawn,
// which is the one case that redraws the battlefield.
void Commit(H3CombatManager *combat, bool allowPresent = true)
{
    state.dirty = false;
    if (!combat || !combat->dlg || combat->IsHiddenBattle())
        return;
    std::vector<H3CombatCreature *> skipped;
    for (const INT32 hex : state.skipHexes)
        if (H3CombatCreature *stack = LiveStackAt(combat, hex))
            skipped.push_back(stack);

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
    std::vector<INT32> shown;
    for (const INT32 hex : hexes)
    {
        H3CombatCreature *stack = LiveStackAt(combat, hex);
        if (IsShadable(hex) && !(stack && Contains(skipped, stack)))
            shown.push_back(hex);
    }
    SortUnique(shown);
    std::vector<H3CombatCreature *> stacks;
    for (const INT32 hex : contourHexes)
    {
        H3CombatCreature *stack = LiveStackAt(combat, hex);
        if (stack && !Contains(skipped, stack))
            stacks.push_back(stack);
    }
    SortUnique(stacks);

    const bool hexesChanged = shown != state.shownHexes;
    const bool contoursChanged = stacks != state.contourStacks;
    if (!hexesChanged && !contoursChanged)
        return;
    const PixelRect dirty = UnionRect(state.shownBounds, BoundsOf(combat, shown));
    RemoveLayer();
    state.shownHexes = shown;
    state.shownBounds = BoundsOf(combat, shown);
    if (contoursChanged)
        UpdateContours(stacks);
    if (!allowPresent || !VisibleBattle(combat))
        return;
    if (contoursChanged)
    {
        ++state.fullRedraws;
        THISCALL_7(void, REDRAW_BATTLEFIELD, combat, TRUE, TRUE, FALSE, 0, TRUE, FALSE); // stack drawing repaints us
        return;
    }
    ComposeLayer(combat);
    PresentRect(dirty);
}

// Commits now unless an !?MM0 is still running; its _Quit or the end of the message handler commits then.
void CommitSoon()
{
    state.dirty = true;
    if (!state.inHint)
        Commit(ActiveCombat());
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
    state.inHint = false;
    state.dirty = false;
    state.followRadius = -1;
    state.followContour = false;
    state.shownHexes.clear();
    state.shownBounds = PixelRect();
    state.contourStacks.clear();
    state.litStacks.clear();
    state.applied = false;
    state.painting = false;
    state.pixels.Release();
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
// Stacks standing on these hexes are left out of every shadow and outline (the spell does not affect them).
// -1 = unused slot. Kept until the next !?MM0, so follow mode keeps using it between hints.
void __stdcall OnSkipStacks(Era::TEvent *)
{
    for (INT32 slot = 1; slot <= SKIP_SLOTS; ++slot)
        if (IsShadable(Era::x[slot]))
            state.skipHexes.push_back(Era::x[slot]);
    CommitSoon();
}

// !!FU(BattleShadow_Clear):P;  Removes everything, follow mode included.
void __stdcall OnClear(Era::TEvent *)
{
    ClearHintShapes();
    state.skipHexes.clear();
    state.followRadius = -1;
    state.followContour = false;
    CommitSoon();
}

// !!FU(BattleShadow_Debug):P?(version)/?(shown)/?(hint)/?(follow)/?(skip)/?(composes)/?(fast)/?(full)/?(cursor);
// x1 = plugin version (0 if this plugin did not answer), x2 = hexes shown now, x3 = hexes asked for in the current
// mouse hint, x4 = follow radius (-1 = off), x5 = skipped stack hexes, x6 = layer paints, x7 = quick screen
// updates, x8 = full battlefield redraws (outline changes), x9 = hex under the cursor as the plugin sees it.
void __stdcall OnDebug(Era::TEvent *)
{
    H3CombatManager *combat = ActiveCombat();
    Era::x[1] = PLUGIN_VERSION;
    Era::x[2] = INT32(state.shownHexes.size());
    Era::x[3] = INT32(state.hintHexes.size());
    Era::x[4] = state.followRadius;
    Era::x[5] = INT32(state.skipHexes.size());
    Era::x[6] = state.composes;
    Era::x[7] = state.fastPresents;
    Era::x[8] = state.fullRedraws;
    Era::x[9] = combat ? CursorHex(combat) : -1;
}

// ERA calls plugin handlers of a trigger before its ERM code: every !?MM0 starts with no hint shapes and no
// skipped stacks. Nothing is drawn yet; the result is committed once, when the trigger is over.
void __stdcall OnBattleMouseHint(Era::TEvent *)
{
    INT32 quitId = 0;
    if (Era::AllocErmFunc)
        Era::AllocErmFunc(MOUSE_HINT_QUIT_EVENT, quitId);
    ClearHintShapes();
    state.skipHexes.clear();
    H3CombatManager *combat = ActiveCombat();
    state.hintAnchor = combat ? CursorHex(combat) : -1;
    state.inHint = true;
    state.dirty = true;
}

void __stdcall OnBattleMouseHintQuit(Era::TEvent *)
{
    state.inHint = false;
    Commit(ActiveCombat());
}

void __stdcall OnBeforeBattleAction(Era::TEvent *)
{
    ClearHintShapes();
    state.skipHexes.clear();
    state.inHint = false;
    state.followRadius = -1;
    state.followContour = false;
    Commit(ActiveCombat(), false); // takes the layer and outlines off; the action redraws the battlefield itself
}

void __stdcall OnLeaveBattleContext(Era::TEvent *)
{
    Forget();
}

void __stdcall OnBattleStart(Era::TEvent *)
{
    Forget();
    state.groundMode = -1;
    state.composes = 0;
    state.fastPresents = 0;
    state.fullRedraws = 0;
    INT32 quitId = 0;
    if (Era::AllocErmFunc)
        Era::AllocErmFunc(MOUSE_HINT_QUIT_EVENT, quitId); // lets ERA fire _Quit from the first hint
}

// The game draws a stack into the canvas: take our paint off first so it is never tinted twice, and put it back
// after, because the stack's redraw also restores the ground under it.
void __stdcall HookDrawStack(HiHook *hook, H3CombatCreature *stack, INT32 x, INT32 y, BOOL numberBoxOnly)
{
    H3CombatManager *combat = H3CombatManager::Get();
    const bool measuring = CollectingBounds(combat);
    const bool active = !state.shownHexes.empty();
    if (active && !state.painting && !measuring)
        RemoveLayer();
    THISCALL_4(void, hook->GetDefaultFunc(), stack, x, y, numberBoxOnly);
    if (active && !measuring)
        ComposeLayer(combat);
}

// The game shows a canvas rectangle on the screen: make sure our layer is in it when they overlap.
void __stdcall HookPresentRect(HiHook *hook, H3WindowManager *windows, INT32 x, INT32 y, INT32 width, INT32 height)
{
    if (!state.painting && !state.shownHexes.empty() && windows == H3WindowManager::Get())
    {
        H3CombatManager *combat = H3CombatManager::Get();
        H3LoadedPcx16 *canvas = windows->GetDrawBuffer();
        if (canvas && VisibleBattle(combat) &&
            ClipRect(x, y, width, height, canvas->width, canvas->height).Intersects(state.shownBounds))
            ComposeLayer(combat);
    }
    THISCALL_5(void, hook->GetDefaultFunc(), windows, x, y, width, height);
}

// Every combat screen message passes here, mouse moves included, so follow mode reacts as fast as the cursor and
// hint shapes are committed right after the !?MM0 that built them.
INT __stdcall HookCombatProcedure(HiHook *hook, H3CombatManager *combat, H3Msg *msg)
{
    const INT result = THISCALL_2(INT, hook->GetDefaultFunc(), combat, msg);
    if (!combat || !combat->dlg || combat != ActiveCombat())
        return result;
    state.inHint = false;
    const bool mouseMoved = msg && msg->IsMouseOver();
    // The mouse reached another hex without a new !?MM0: the old hint shapes no longer apply.
    if (mouseMoved && !state.hintHexes.empty() && CursorHex(combat) != state.hintAnchor)
    {
        ClearHintShapes();
        state.dirty = true;
    }
    if (state.dirty || (mouseMoved && state.followRadius >= 0))
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
    // Same entries and signatures as Interface_SpellsDescription and Combat_SwitchingCreatureAction.
    patcher->WriteHiHook(DRAW_STACK, SPLICE_, THISCALL_, HookDrawStack);
    patcher->WriteHiHook(PRESENT_RECT, SPLICE_, THISCALL_, HookPresentRect);
    patcher->WriteHiHook(COMBAT_DLG_PROC, SPLICE_, THISCALL_, HookCombatProcedure);
}
}
