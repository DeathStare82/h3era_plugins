#include "pch.h"
#include "InfoPanel.h"

#include "ErtText.h"
#include "InfoPanelLayout.h"
#include "PlayTime.h"

#include <shellapi.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace infopanel
{
using namespace h3;
namespace L = layout;

InfoPanel *InfoPanel::instance = nullptr;

namespace
{
constexpr DWORD UPDATE_INTERVAL_MS = 500; // 시계 확인 주기 (분이 바뀔 때만 실제로 다시 그림)

constexpr DWORD ADDR_ADVMGR_SHOW = 0x40730F; // 모험 지도 표시 (GraphicsEnhancements 와 같은 위치)
constexpr DWORD ADDR_ADVMGR_PROC = 0x408710; // 모험 지도 메시지 처리. 입력이 없을 때도 NONE 으로 계속 불린다
constexpr DWORD ADDR_MOVE_HINT_1 = 0x40EB47; // 마우스 이동 시 힌트 갱신 호출 위치 (MithrilDisplay 와 동일)
constexpr DWORD ADDR_MOVE_HINT_2 = 0x40E1CC;
constexpr DWORD FUNC_SET_ADV_HINT = 0x40B040; // 하단 힌트 바에 문구 설정
constexpr DWORD VAR_LAST_HINT_ITEM = 0x65F228; // 마지막으로 힌트를 띄운 항목 번호 (중복 갱신 방지용)
constexpr int HINT_BUFFER_LIMIT = 500;

bool IsNight(int hour)
{
    return hour < L::DAY_START_HOUR || hour >= L::NIGHT_START_HOUR;
}

int TimeTextWidth(const char *text)
{
    int w = 0;
    for (const char *p = text; *p; ++p)
        if (*p == ':')
            w += L::COLON_W;
        else if (*p >= '0' && *p <= '9')
            w += L::DIGIT_W;
    return w;
}

bool IsLeftDown(const H3Msg *msg)
{
    return (msg->command == eMsgCommand::MOUSE_BUTTON && msg->subtype == eMsgSubtype::LBUTTON_DOWN) ||
           msg->command == eMsgCommand::LCLICK_OUTSIDE;
}

bool IsRightDown(const H3Msg *msg)
{
    return (msg->command == eMsgCommand::MOUSE_BUTTON && msg->subtype == eMsgSubtype::RBUTTON_DOWN) ||
           msg->command == eMsgCommand::RCLICK_OUTSIDE;
}

// 실제로 마우스 버튼이 눌려 있는지 (왼손잡이 설정으로 버튼이 바뀐 경우도 고려)
bool IsMouseButtonHeld(bool primary)
{
    const bool swapped = GetSystemMetrics(SM_SWAPBUTTON) != 0;
    const int vk = (primary != swapped) ? VK_LBUTTON : VK_RBUTTON;
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

int ClampInt(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}
} // namespace

void MarkDirty()
{
    if (InfoPanel::instance)
        InfoPanel::instance->dirty = true;
}

InfoPanel::InfoPanel() : IGamePatch(_PI)
{
}

InfoPanel &InfoPanel::Get()
{
    if (!instance)
        instance = new InfoPanel();
    return *instance;
}

void InfoPanel::CreatePatches()
{
    if (m_isInited)
        return;

    WriteLoHook(ADDR_ADVMGR_SHOW, AdvMgr_OnShow);
    WriteHiHook(ADDR_ADVMGR_PROC, THISCALL_, reinterpret_cast<void *>(AdvMgr_Proc));
    WriteHiHook(ADDR_MOVE_HINT_1, THISCALL_, reinterpret_cast<void *>(AdvMgrDlg_MoveHint));
    WriteHiHook(ADDR_MOVE_HINT_2, THISCALL_, reinterpret_cast<void *>(AdvMgrDlg_MoveHint));

    m_isInited = true;
    m_isEnabled = true;
}

// =====================================================================================================================
// 리소스
// =====================================================================================================================
// 게임이 돌려준 리소스가 "우리가 만든 그 그림"인지 확인한다.
// 파일을 못 찾으면 게임/HD 가 빈 그림이나 default.pcx/def 같은 대체 그림을 돌려주는데,
// 그걸 그대로 그리면 빈 버퍼(주소 0)를 읽다가 게임이 튕긴다. (1차 테스트 크래시 원인)
namespace
{
bool IsValidPcx(const H3LoadedPcx *pcx, int w, int h)
{
    return pcx && pcx->buffer && pcx->width == w && pcx->height == h;
}

bool IsValidDef(const H3LoadedDef *def, int w, int h, int groups, int framesPerGroup)
{
    if (!def || !def->groups || def->widthDEF != w || def->heightDEF != h || def->groupsCount < groups)
        return false;
    for (int g = 0; g < groups; ++g)
        if (!def->groups[g] || !def->groups[g]->frames || def->groups[g]->count < framesPerGroup)
            return false;
    return true;
}

template <typename T> void Release(T *&res)
{
    if (res)
        res->Dereference();
    res = nullptr;
}
} // namespace

bool InfoPanel::LoadAssets()
{
    if (assetsTried)
        return assetsOk;
    assetsTried = true;

    // PCX 는 없으면 게임이 대체 그림을 만들어 버리므로 먼저 존재 여부를 확인한다
    if (Era::PcxPngExists(res::BACKGROUND_PCX))
        background = H3LoadedPcx::Load(res::BACKGROUND_PCX);
    plaqueDef = H3LoadedDef::Load(res::PLAQUE_DEF);
    iconDef = H3LoadedDef::Load(res::ICON_DEF);
    digitDef = H3LoadedDef::Load(res::DIGIT_DEF);
    buttonDef = H3LoadedDef::Load(res::BUTTON_DEF);

    const bool bgOk = IsValidPcx(background, L::PANEL_W, L::PANEL_H);
    const bool plqOk = IsValidDef(plaqueDef, L::PLAQUE_W, L::PLAQUE_H, 1, 2);
    const bool icoOk = IsValidDef(iconDef, L::ICON_W, L::CELL_H, 1, 4);
    const bool dgtOk = IsValidDef(digitDef, L::DIGIT_W, L::CELL_H, 2, 11);
    const bool btnOk = IsValidDef(buttonDef, L::BTN_W, L::BTN_H, 1, 2);

    assetsOk = bgOk && plqOk && icoOk && dgtOk && btnOk;
    if (!assetsOk)
    {
        char msg[256];
        std::snprintf(msg, sizeof(msg),
                      "missing or wrong resources (put them in a .pac): %s%s%s%s%s- panel disabled",
                      bgOk ? "" : "DsIpBg.pcx ", plqOk ? "" : "DsIpPlq.def ", icoOk ? "" : "DsIpIco.def ",
                      dgtOk ? "" : "DsIpDgt.def ", btnOk ? "" : "DsIpBtn.def ");
        Era::WriteLog("AdventureInfoPanel", "LoadAssets", msg);
        Release(background);
        Release(plaqueDef);
        Release(iconDef);
        Release(digitDef);
        Release(buttonDef);
    }
    return assetsOk;
}

// =====================================================================================================================
// 대화상자 항목
// =====================================================================================================================

// 영웅 패널 아래에 105px 이 비어 있는지.
// HD 모드는 영웅/마을 목록을 5칸에서 최대 7칸까지 늘리고(칸당 32px), 상태창은 그만큼 아래로 내려간다.
// 1366x768 / 7칸 기준: 상태창 아래 끝 = 574 + 64 = 638 = 높이 - 130 → 패널이 꼭 맞는다.
bool InfoPanel::HasSpace(H3AdventureMgrDlg *dlg, int panelY) const
{
    const int listSlots = ClampInt(globalPatcher->VarGetValue<int>("HD.AdvMgr.TownList.L", 5), 5, 7);
    const int statusBottom = L::HD_STATUS_BOTTOM_BASE + L::HD_LIST_SLOT_H * (listSlots - 5);
    if (statusBottom <= panelY + L::MAX_OVERLAP)
        return true;

    // 예비 확인: 대화상자가 알고 있는 상태창 위치
    if (dlg && dlg->panel)
    {
        const int panelBottom = dlg->panel->GetY() + dlg->panel->GetHeight();
        if (panelBottom > 0 && panelBottom <= panelY + L::MAX_OVERLAP)
            return true;
    }
    return false;
}

bool InfoPanel::EnsureCreated(H3AdventureMgrDlg *dlg)
{
    if (!dlg)
        return false;

    if (dlg->GetH3DlgItem(ids::CANVAS_ITEM_ID))
    {
        ownerDlg = dlg; // 이미 붙어 있음
        return canvas != nullptr;
    }

    ownerDlg = nullptr;
    const int x = H3GameWidth::Get() - L::PANEL_RIGHT_OFFSET;
    const int y = H3GameHeight::Get() - L::PANEL_BOTTOM_OFFSET;
    if (x < 0 || y < 0 || !HasSpace(dlg, y) || !LoadAssets())
        return false;

    // 새 대화상자마다 새 캔버스를 만든다.
    // 이전 캔버스는 이전 대화상자와 함께 쓰이지 않게 된 것이므로 그대로 둔다 (해제 시점이 불확실해서 안전하게 남겨 둠, 약 40KB).
    H3LoadedPcx16 *pcx = H3LoadedPcx16::Create(L::PANEL_W, L::PANEL_H);
    if (!pcx)
        return false;

    H3DlgPcx16 *item = H3DlgPcx16::Create(x, y, L::PANEL_W, L::PANEL_H, ids::CANVAS_ITEM_ID, nullptr);
    if (!item)
    {
        pcx->Destroy();
        return false;
    }
    item->SetPcx(pcx);
    dlg->AddItem(item);
    item->DeActivate(); // 클릭/힌트는 좌표로 직접 처리한다

    canvas = pcx;
    panelX = x;
    panelY = y;
    ownerDlg = dlg;
    pressed = false;
    trackingPress = false;
    dirty = true;
    Update(dlg, true);
    return true;
}

bool InfoPanel::IsActiveFor(const H3AdventureMgrDlg *dlg) const
{
    return dlg && dlg == ownerDlg && canvas;
}

// =====================================================================================================================
// 그리기
// =====================================================================================================================
void InfoPanel::Render()
{
    if (!canvas || !assetsOk)
        return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    const bool night = IsNight(st.wHour);
    const unsigned int playMinutes = playtime::TotalMinutes();

    char clockText[8];
    std::snprintf(clockText, sizeof(clockText), "%02d:%02d", int(st.wHour), int(st.wMinute));

    char playText[12];
    if (playMinutes >= 1000u * 60u) // 999:59 까지만 표시
        std::snprintf(playText, sizeof(playText), "999:59");
    else
        std::snprintf(playText, sizeof(playText), "%02u:%02u", playMinutes / 60u, playMinutes % 60u);

    // 1) 배경 (돌판 + 테두리 + 구분선)
    background->DrawToPcx16(0, 0, L::PANEL_W, L::PANEL_H, canvas, 0, 0, FALSE);

    // 2) 라벨 (ERT 202782 / 202783, 게임 글꼴)
    const char *labels[2] = {ert::Text(ert::LABEL_CLOCK), ert::Text(ert::LABEL_PLAYTIME)};
    H3Font *font = H3SmallFont::Get();
    for (int i = 0; i < 2; ++i)
    {
        if (font)
            canvas->TextDraw(font, labels[i], L::PLAQUE_X[i], L::LABEL_Y, L::PLAQUE_W, L::LABEL_H,
                             eTextColor::HIGHLIGHT, eTextAlignment::MIDDLE_CENTER);
        plaqueDef->DrawToPcx16(0, night ? L::frame::PLAQUE_NIGHT : L::frame::PLAQUE_DAY, canvas, L::PLAQUE_X[i],
                               L::PLAQUE_Y);
    }

    // 3) 아이콘 + 숫자 (낮: 나무판 + 흰 숫자 + 해 / 밤: 어두운 홈 + 금박 숫자 + 달)
    DrawTime(0, clockText, night ? L::frame::ICON_MOON : L::frame::ICON_SUN, night);
    DrawTime(1, playText, night ? L::frame::ICON_HOURGLASS_NIGHT : L::frame::ICON_HOURGLASS_DAY, night);

    // 4) 버튼 (밤에도 색은 바뀌지 않음)
    buttonDef->DrawToPcx16(0, pressed ? L::frame::BUTTON_PRESSED : L::frame::BUTTON_NORMAL, canvas, L::BTN_X,
                           L::BTN_Y);

    shownClock = st.wHour * 60 + st.wMinute;
    shownPlay = static_cast<int>(playMinutes);
    shownNight = night;
    dirty = false;
}

void InfoPanel::DrawTime(int index, const char *text, int iconFrame, bool night)
{
    const int total = L::ICON_W + L::ICON_GAP + TimeTextWidth(text);
    int x = L::PLAQUE_X[index] + L::PLAQUE_INNER + (L::PLAQUE_INNER_W - total) / 2;
    const int y = L::PLAQUE_Y + L::PLAQUE_INNER;

    iconDef->DrawToPcx16(0, iconFrame, canvas, x, y);
    x += L::ICON_W + L::ICON_GAP;

    const int group = night ? L::frame::DIGIT_GROUP_NIGHT : L::frame::DIGIT_GROUP_DAY;
    for (const char *p = text; *p; ++p)
    {
        const bool colon = *p == ':';
        if (!colon && (*p < '0' || *p > '9'))
            continue;
        const int w = colon ? L::COLON_W : L::DIGIT_W;
        const int frame = colon ? L::frame::DIGIT_COLON : *p - '0';
        // 숫자 칸은 배경까지 들어 있는 불투명 그림이라 겹치지 않게 칸 폭만큼만 잘라 그린다
        digitDef->DrawToPcx16(group, frame, 0, 0, w, L::CELL_H, canvas, x, y);
        x += w;
    }
}

// 모험 지도가 맨 위 창일 때만 화면에 바로 반영한다. (마을/전투 화면 위에 덧그리지 않도록)
// 다른 창이 떠 있으면 캔버스만 갱신해 두고, 지도로 돌아와 전체를 다시 그릴 때 함께 보인다.
void InfoPanel::Present(H3AdventureMgrDlg *dlg)
{
    if (!dlg || P_WindowManager->lastDlg != dlg)
        return;
    if (H3DlgItem *item = dlg->GetH3DlgItem(ids::CANVAS_ITEM_ID))
    {
        item->Draw();
        item->Refresh();
    }
}

void InfoPanel::Update(H3AdventureMgrDlg *dlg, bool force)
{
    const DWORD now = GetTickCount();
    if (!force && !dirty && now - lastCheckTick < UPDATE_INTERVAL_MS)
        return;
    lastCheckTick = now;

    if (!force && !dirty)
    {
        SYSTEMTIME st;
        GetLocalTime(&st);
        const int clock = st.wHour * 60 + st.wMinute;
        const int play = static_cast<int>(playtime::TotalMinutes());
        if (clock == shownClock && play == shownPlay)
            return;
    }
    Render();
    Present(dlg);
}

void InfoPanel::SetPressed(H3AdventureMgrDlg *dlg, bool state)
{
    if (pressed == state)
        return;
    pressed = state;
    Render();
    Present(dlg);
}

// =====================================================================================================================
// 입력
// =====================================================================================================================
int InfoPanel::ZoneAt(int x, int y) const
{
    if (!canvas)
        return L::ZONE_NONE;
    const int lx = x - panelX;
    const int ly = y - panelY;
    for (int i = 0; i < 3; ++i)
    {
        const L::Zone &z = L::ZONES[i];
        if (lx >= z.x && lx < z.x + z.w && ly >= z.y && ly < z.y + z.h)
            return i;
    }
    return L::ZONE_NONE;
}

const char *InfoPanel::HintText(int zone) const
{
    switch (zone)
    {
    case L::ZONE_CLOCK:
        return ert::Text(ert::HINT_CLOCK);
    case L::ZONE_PLAYTIME:
        return ert::Text(ert::HINT_PLAYTIME);
    case L::ZONE_BUTTON:
        return ert::Text(ert::HINT_BUTTON);
    default:
        return "";
    }
}

// 우클릭 팝업: {제목} + 설명. 버튼은 설명 + 이동할 주소.
void InfoPanel::ShowPopup(int zone) const
{
    std::string text;
    switch (zone)
    {
    case L::ZONE_CLOCK:
        text = std::string("{") + ert::Text(ert::LABEL_CLOCK) + "}\n\n" + ert::Text(ert::HINT_CLOCK);
        break;
    case L::ZONE_PLAYTIME:
        text = std::string("{") + ert::Text(ert::LABEL_PLAYTIME) + "}\n\n" + ert::Text(ert::HINT_PLAYTIME);
        break;
    case L::ZONE_BUTTON:
        text = std::string(ert::Text(ert::HINT_BUTTON)) + "\n\n{" + ert::Text(ert::BLOG_URL) + "}";
        break;
    default:
        return;
    }
    H3Messagebox::RMB(text.c_str());
}

void InfoPanel::OpenBlog() const
{
    const char *url = ert::Text(ert::BLOG_URL);
    // 안전장치: 웹 주소만 연다
    if (std::strncmp(url, "https://", 8) != 0 && std::strncmp(url, "http://", 7) != 0)
        return;
    ShellExecuteA(nullptr, "open", url, nullptr, nullptr, SW_SHOWNORMAL);
}

// =====================================================================================================================
// 후킹
// =====================================================================================================================

// 모험 지도가 표시될 때 패널 항목을 붙인다 (이미 있으면 그대로).
int __stdcall InfoPanel::AdvMgr_OnShow(LoHook *h, HookContext *c)
{
    if (instance)
    {
        H3AdventureManager *adv = H3AdventureManager::Get();
        if (adv && adv->dlg)
            instance->EnsureCreated(adv->dlg);
    }
    return EXEC_DEFAULT;
}

// 클릭/우클릭 처리 + 입력이 없을 때 시계 갱신
int __stdcall InfoPanel::AdvMgr_Proc(HiHook *h, H3AdventureManager *adv, H3Msg *msg)
{
    InfoPanel *self = instance;
    H3AdventureMgrDlg *dlg = adv ? adv->dlg : nullptr;

    if (self && msg && self->IsActiveFor(dlg))
    {
        if (IsLeftDown(msg) && self->ZoneAt(msg->GetX(), msg->GetY()) == L::ZONE_BUTTON)
        {
            self->trackingPress = true;
            self->SetPressed(dlg, true);
            return 1;
        }

        if (self->trackingPress)
        {
            if (msg->command == eMsgCommand::LBUTTON_UP)
            {
                const bool inside = self->ZoneAt(msg->GetX(), msg->GetY()) == L::ZONE_BUTTON;
                self->trackingPress = false;
                self->SetPressed(dlg, false);
                if (inside)
                    self->OpenBlog();
                return 1;
            }
            if (msg->command == eMsgCommand::MOUSE_OVER)
            {
                // 누른 채로 버튼 밖으로 나가면 튀어나온 모양, 다시 들어오면 눌린 모양 (일반 H3 버튼과 같은 동작)
                if (!IsMouseButtonHeld(true))
                {
                    self->trackingPress = false; // 창 밖에서 버튼을 놓은 경우
                    self->SetPressed(dlg, false);
                }
                else
                    self->SetPressed(dlg, self->ZoneAt(msg->GetX(), msg->GetY()) == L::ZONE_BUTTON);
            }
        }

        // 같은 우클릭이 두 형태(원시/항목 메시지)로 들어와도 팝업은 한 번만:
        // 팝업은 버튼을 누르고 있는 동안만 뜨므로, 이미 손을 뗀 뒤 도착한 메시지는 무시한다.
        if (IsRightDown(msg) && IsMouseButtonHeld(false))
        {
            const int zone = self->ZoneAt(msg->GetX(), msg->GetY());
            if (zone != L::ZONE_NONE)
            {
                self->ShowPopup(zone);
                return 1;
            }
        }

        self->Update(dlg, false);
    }

    return THISCALL_2(int, h->GetDefaultFunc(), adv, msg);
}

// 마우스를 올리면 하단 힌트 바에 ERT 문구 (자원 표시 바로 위 줄)
char __stdcall InfoPanel::AdvMgrDlg_MoveHint(HiHook *h, H3AdventureMgrDlg *dlg, int x, int y)
{
    InfoPanel *self = instance;
    if (self && self->IsActiveFor(dlg) && !(dlg->screenlogEdit && dlg->screenlogEdit->IsFocused()))
    {
        const int zone = self->ZoneAt(x, y);
        if (zone != L::ZONE_NONE)
        {
            std::snprintf(h3_TextBuffer, HINT_BUFFER_LIMIT, "%s", self->HintText(zone));
            THISCALL_2(void, FUNC_SET_ADV_HINT, H3AdventureManager::Get(), h3_TextBuffer);
            IntAt(VAR_LAST_HINT_ITEM) = ids::HINT_ZONE_ID_BASE + zone;
            return 1;
        }
    }
    return THISCALL_3(char, h->GetDefaultFunc(), dlg, x, y);
}
} // namespace infopanel
