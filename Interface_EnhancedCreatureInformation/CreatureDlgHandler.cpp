#include "pch.h"

#include "CreatureDlgHandler.h"
#include "CreatureDlgHooks.h"
#include "CreatureDlgLayout.h"
#include "PluginSettings.h"
#include "CreatureSpellEffects.h"

#include <algorithm>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <map>

using namespace h3;
using namespace creatureInfo;

// ERT 파일의 데이터를 담을 전역 맵
static std::map<int, std::string> g_ertStrings;
static bool g_isErtLoaded = false;

// .ert 파일 파싱
void LoadErtFile()
{
    if (g_isErtLoaded)
        return;

    std::ifstream file("Data/s/The Multiverse - 10. Hint.ert");
    if (!file.is_open())
    {
        file.open("The Multiverse - 10. Hint.ert");
    }

    if (!file.is_open())
    {
        g_isErtLoaded = true;
        return;
    }

    std::string line;
    while (std::getline(file, line))
    {
        if (line.empty() || line[0] == '#' || line[0] == ';')
            continue;

        std::stringstream ss(line);
        int id;
        if (ss >> id)
        {
            std::string text;
            std::getline(ss, text);
            
            size_t startPos = text.find_first_not_of(" \t\r\n");
            if (startPos != std::string::npos)
            {
                text = text.substr(startPos);
            }
            else
            {
                text = "";
            }
            g_ertStrings[id] = text;
        }
    }
    g_isErtLoaded = true;
}

std::string GetErtText(int id, const std::string& fallback = "")
{
    if (!g_isErtLoaded)
    {
        LoadErtFile();
    }

    auto it = g_ertStrings.find(id);
    if (it != g_ertStrings.end())
    {
        return it->second;
    }
    return fallback;
}

void CreatureDlgHandler::HideDefaultBattleSpellItems()
{
    if (!dlg)
        return;

    // 원본 창의 걸린 마법 아이콘
    for (int id = DLG_NATIVE_SPELL_FIRST_ID; id <= DLG_NATIVE_SPELL_LAST_ID; ++id)
        if (auto *item = dlg->GetH3DlgItem(id))
            item->HideDeactivate();

    // [Multiverse] 원본 창의 아이콘 아래 지속시간 글자 ("x1", "x12" 등) — 아이콘만 숨기면 이 글자가 남는다
    for (int id = DLG_NATIVE_SPELL_TEXT_FIRST_ID; id <= DLG_NATIVE_SPELL_TEXT_LAST_ID; ++id)
        if (auto *item = dlg->GetH3DlgItem(id))
            item->HideDeactivate();
}

// =====================================================================
// [Multiverse] 커스텀 레이아웃 도우미 (Stackinfo_0.txt 템플릿 좌표 기준, 창 크기 450x419)
// =====================================================================
namespace
{
    // 템플릿 ID + 30000 → 원본 창 항목이나 daemon 항목과 절대 겹치지 않음
    constexpr int MV_ID_BASE = 30000;
    constexpr eTextAlignment MV_TOP_CENTER = static_cast<eTextAlignment>(1);   // 0=왼쪽, 1=가운데, 2=오른쪽

    bool MvIsEmpty(const char* s)
    {
        if (!s) return true;
        for (; *s; ++s)
            if (*s != ' ' && *s != '\t' && *s != '\r' && *s != '\n')
                return false;
        return true;
    }

    const char* MvZ(int index)            // ERM z변수 읽기 (z11 → MvZ(11))
    {
        const char* s = Era::z[index];
        return s ? s : "";
    }

    int MvCountLines(const char* s)       // 줄 수 세기 (적용된 마법 개수용)
    {
        if (MvIsEmpty(s)) return 0;
        int n = 1;
        for (; *s; ++s)
            if (*s == '\n') ++n;
        return n;
    }

    H3DlgText* MvAddText(H3CreatureInfoDlg* dlg, int templateId, int x, int y, int w, int h,
                         const char* text, LPCSTR font, eTextAlignment align, const char* hint = nullptr)
    {
        if (!dlg || MvIsEmpty(text)) return nullptr;
        auto* item = H3DlgText::Create(x, y, w, h, text, font, eTextColor::WHITE,
                                       MV_ID_BASE + templateId, align);
        if (item) 
        {
            if (hint && *hint)
            {
                // 하단 상태바에 힌트 출력 설정
                item->SetHints(hint, h3_NullString, TRUE);
            }
            dlg->AddItem(item);
        }
        return item;
    }

    void MvHide(H3CreatureInfoDlg* dlg, int id)
    {
        if (!dlg) return;
        if (auto* item = dlg->GetH3DlgItem(id))
            item->HideDeactivate();
    }

    // "%d" 자리에 숫자 끼워넣기 (ERT 문구용)
    std::string MvFormatCount(std::string fmt, int value)
    {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d", value);
        const auto pos = fmt.find("%d");
        if (pos != std::string::npos) fmt.replace(pos, 2, buf);
        else fmt += buf;
        return fmt;
    }

    // ---------------------------------------------------------------
    // [Multiverse] 적용된 마법 개별 행 + 우클릭 안내 팝업
    // ---------------------------------------------------------------
    constexpr int MV_SPELL_ROW_FIRST     = 500;     // 템플릿 ID 500~579 → 아이템 ID 30500~30579
    constexpr int MV_SPELL_ROW_MAX       = 80;
    constexpr int MV_SPELL_SLOT_COUNT    = 81;      // ERM 루프 r = 0~80
    // ERT 번호 2개 (ERT에 없으면 아래 코드의 기본 문구 사용. 이미 쓰는 번호라면 여기만 바꾸면 됨)
    constexpr int MV_ERT_SPELL_ROW_HINT  = 202774;  // 행 위 마우스오버 힌트
    constexpr int MV_ERT_SPELL_NO_DESC   = 202775;  // 설명을 찾지 못했을 때

    // 게임 내부 안내창 (마법책 우클릭 창, ERM IF:Q 와 같은 함수)
    //   style 4 = 우클릭을 누르고 있는 동안만 표시 / 그림 종류 9 = 마법 (SPELLSCR 그림 + 아래에 마법 이름)
    //   창 크기는 글 길이에 맞춰 게임이 알아서 정한다.
    using MvNativeMessageBox = void(__fastcall *)(const char* text, int style, int x, int y,
                                                  int type1, int subtype1, int type2, int subtype2,
                                                  int par, int time, int type3, int subtype3);
    constexpr DWORD MV_NATIVE_MESSAGE_BOX = 0x4F6C00;
    constexpr int   MV_MSGBOX_RMB = 4;
    constexpr int   MV_PIC_SPELL  = 9;

    // spellId < 0 이면 그림 없이 글만 (단순 메시지)
    void MvShowSpellRmbBox(const char* text, int spellId)
    {
        const bool hasPicture = spellId >= 0 && spellId < MV_SPELL_SLOT_COUNT;
        reinterpret_cast<MvNativeMessageBox>(MV_NATIVE_MESSAGE_BOX)(
            text, MV_MSGBOX_RMB, -1, -1,
            hasPicture ? MV_PIC_SPELL : -1, hasPicture ? spellId : 0,
            -1, 0, -1, 0, -1, 0);
    }

    // ---------------------------------------------------------------
    // ERM SN:W 변수 읽기 (z변수와 달리 글자 수 제한 없음)
    //   ERM(CM4)이 줄마다 남겨 두는 값 (n = 1부터):
    //     SN:W^Mv_AppSp_Count^              줄 수
    //     SN:W^Mv_AppSp_<n>_Text^           목록 한 줄
    //     SN:W^Mv_AppSp_<n>_Slot/Code/Lev^  효과 칸(r) / 마법 번호(s) / 걸린 수준(l)
    //     SN:W^Mv_AppSp_<n>_Popup^          우클릭 안내문
    // ---------------------------------------------------------------
    int MvSnInt(const char* name)
    {
        return Era::GetAssocVarIntValue(name);
    }

    std::string MvSnStr(const char* name)
    {
        const char* s = Era::GetAssocVarStrValue(name);   // Era가 새로 할당해서 돌려줌 → 반드시 MemFree
        std::string out = s ? s : "";
        if (s)
            Era::MemFree((void*)s);
        return out;
    }

    // 스크롤바 (마법이 목록 칸을 넘칠 때만 생김)
    constexpr int MV_SCROLLBAR_TEMPLATE_ID = 599;   // 아이템 ID 30599
    constexpr int MV_SCROLLBAR_WIDTH       = 16;
    constexpr int MV_SCROLLBAR_GAP         = 2;     // 글자와 스크롤바 사이

    // 적용된 마법 목록 칸 (Stackinfo_0.txt 템플릿 36번 자리)
    constexpr int MV_APPLIED_LIST_X      = 286;
    constexpr int MV_APPLIED_LIST_Y      = 137;
    constexpr int MV_APPLIED_LIST_WIDTH  = 145;
    constexpr int MV_APPLIED_LIST_HEIGHT = 188;

    // 스크롤바 콜백이 어느 창의 목록을 움직일지 (정보창은 한 번에 하나만 열림)
    CreatureDlgHandler* g_mvAppliedSpellsHandler = nullptr;

    // 마우스 휠 메시지 (H3API eMsgCommand::WHEEL_BUTTON 과 같은 값)
    //   subtype > 0 : 휠을 위로(앞으로) = 목록 위로 / subtype < 0 : 아래로
    constexpr int MV_MSG_MOUSE_WHEEL = 0x309;

    // 휠이 안 먹을 때 원인 확인용: true로 바꿔 빌드하면, 창에 들어오는 메시지 번호가 아래 상태바에 보인다.
    //   휠을 굴렸을 때 나오는 "cmd=..., sub=..." 값을 알려주면 위 숫자를 맞출 수 있다. 확인 후 다시 false로.
    constexpr bool MV_WHEEL_DEBUG = false;

    // 휠로 움직였을 때 스크롤바 손잡이 위치도 맞춘다
    // ※ SetTick / SetButtonPosition 에서 컴파일 오류가 나면 그 줄만 지워도 된다 (목록은 움직이고 손잡이만 그대로)
    void MvSyncScrollbar(H3DlgScrollbar* bar, int tick)
    {
        if (!bar)
            return;
        bar->SetTick(tick);
        bar->SetButtonPosition();
        bar->Draw();
        bar->Refresh();
    }

    // 안내창 대신 단순 메시지를 띄우는 효과 칸 (r) 과 그 ERT
    constexpr int MV_SLOT_OGRE_ROAR = 39;   // 오우거의 함성
    constexpr int MV_SLOT_HYPNOTIZE = 60;   // 최면
    constexpr int MV_ERT_OGRE_ROAR  = 202776;
    constexpr int MV_ERT_HYPNOTIZE  = 202777;

    // 스크롤 전용 마법(81번~): 스크롤 마법책 우클릭(ERM)과 같은 방식
    //   SPELLSCR 80번 그림을 Data\defs\SPELLSCR\0_<번호>.png 로 바꾸고, 80번 마법 이름을 잠깐 바꿔서 보여준다.
    constexpr int MV_SCROLL_SPELL_FIRST   = 81;
    constexpr int MV_SCROLL_SPELL_LAST    = 200;
    constexpr int MV_SCROLL_PICTURE_SLOT  = 80;

    // "{~LightGreen}...{~}" 같은 색상 태그 제거
    std::string MvStripColorTags(const std::string& s)
    {
        std::string out;
        for (size_t i = 0; i < s.size(); ++i)
        {
            if (s[i] == '{' && i + 1 < s.size() && s[i + 1] == '~')
            {
                const size_t close = s.find('}', i);
                if (close == std::string::npos) break;
                i = close;
                continue;
            }
            out += s[i];
        }
        return out;
    }

    // z21 한 줄 "(중급)마법의 힘 3턴" → "마법의 힘"
    std::string MvSpellNameFromRow(const std::string& rowText)
    {
        const std::string t = MvStripColorTags(rowText);
        const size_t paren = t.find(')');
        const size_t start = (paren == std::string::npos) ? 0 : paren + 1;
        size_t end = t.rfind(' ');
        if (end == std::string::npos || end < start) end = t.size();
        std::string name = t.substr(start, end - start);
        const size_t first = name.find_first_not_of(' ');
        const size_t last = name.find_last_not_of(' ');
        return first == std::string::npos ? std::string() : name.substr(first, last - first + 1);
    }

    // 폰트 기준 줄 높이 / 줄 수 (긴 마법 이름이 두 줄로 넘어가도 잘리지 않게)
    // ※ H3FontLoader / height / GetLinesCountInText 에서 컴파일 오류가 나면
    //    이 함수 본문을 "return {fallbackLineHeight, 1};" 로 바꿔도 동작합니다.
    struct MvTextMetrics
    {
        int lineHeight;
        int lines;
    };

    MvTextMetrics MvMeasure(LPCSTR fontName, const char* text, int width, int fallbackLineHeight)
    {
        MvTextMetrics m{fallbackLineHeight, 1};
        H3FontLoader font(fontName);
        if (H3Font* f = font.Get())
        {
            if (f->height > 0)
                m.lineHeight = f->height;
            if (!MvIsEmpty(text))
            {
                const int n = f->GetLinesCountInText(text, width);
                if (n > 0) m.lines = n;
            }
        }
        return m;
    }
}

// ERM(CM4)이 SN:W로 줄마다 남겨 둔 적용된 마법을 읽는다. 하나라도 있으면 true
bool CreatureDlgHandler::CollectAppliedSpellRows()
{
    appliedSpellRows.clear();

    const int count = MvSnInt("Mv_AppSp_Count");
    for (int n = 1; n <= count && n <= MV_SPELL_ROW_MAX; ++n)
    {
        char name[64];
        AppliedSpellRow row;

        std::snprintf(name, sizeof(name), "Mv_AppSp_%d_Text", n);
        row.text = MvSnStr(name);
        if (MvIsEmpty(row.text.c_str()))
            continue;

        std::snprintf(name, sizeof(name), "Mv_AppSp_%d_Slot", n);
        row.spellId = MvSnInt(name);
        std::snprintf(name, sizeof(name), "Mv_AppSp_%d_Code", n);
        row.spellCode = MvSnInt(name);
        std::snprintf(name, sizeof(name), "Mv_AppSp_%d_Lev", n);
        row.level = MvSnInt(name);
        std::snprintf(name, sizeof(name), "Mv_AppSp_%d_Popup", n);
        row.popup = MvSnStr(name);

        appliedSpellRows.push_back(row);
    }
    return !appliedSpellRows.empty();
}

// 적용된 마법 목록을 "한 줄 = 한 아이템"으로 만든다. 칸을 넘치면 오른쪽에 스크롤바를 붙인다.
void CreatureDlgHandler::BuildAppliedSpellList(int x, int y, int width, int height)
{
    appliedSpellItems.clear();
    appliedRowTop.clear();
    appliedRowHeight.clear();
    appliedScrollbar = nullptr;
    appliedScrollTick = 0;
    appliedMaxTick = 0;
    appliedListX = x;
    appliedListY = y;
    appliedListHeight = height;

    if (!dlg || appliedSpellRows.empty())
        return;

    // 1) 줄 높이 재기 (긴 이름은 두 줄이 될 수 있음). 넘치면 스크롤바 자리를 빼고 다시 잰다.
    auto measure = [this](int textWidth) {
        appliedRowTop.clear();
        appliedRowHeight.clear();
        int top = 0;
        for (const auto &row : appliedSpellRows)
        {
            const MvTextMetrics m = MvMeasure(NH3Dlg::Text::SMALL, row.text.c_str(), textWidth, 16);
            const int rowHeight = m.lineHeight * m.lines;
            appliedRowTop.push_back(top);
            appliedRowHeight.push_back(rowHeight);
            top += rowHeight;
        }
        return top;
    };

    int textWidth = width;
    int totalHeight = measure(textWidth);
    const bool needScroll = totalHeight > height;
    if (needScroll)
    {
        textWidth = width - MV_SCROLLBAR_WIDTH - MV_SCROLLBAR_GAP;
        totalHeight = measure(textWidth);
    }
    appliedListWidth = textWidth;

    // 2) 최대 스크롤 위치 = 마지막 줄까지 칸 안에 들어오게 되는 첫 줄 번호
    if (needScroll)
    {
        appliedMaxTick = static_cast<int>(appliedSpellRows.size()) - 1;
        for (int t = 0; t < static_cast<int>(appliedRowTop.size()); ++t)
            if (totalHeight - appliedRowTop[t] <= height)
            {
                appliedMaxTick = t;
                break;
            }
    }

    // 3) 줄마다 아이템 (위치와 보이기/숨기기는 LayoutAppliedSpellRows가 정함)
    const std::string rowHint = GetErtText(MV_ERT_SPELL_ROW_HINT, "{~y}우클릭{~}: 이 마법의 상세 정보 보기");
    for (size_t i = 0; i < appliedSpellRows.size(); ++i)
    {
        appliedSpellItems.push_back(MvAddText(dlg, MV_SPELL_ROW_FIRST + static_cast<int>(i), x, y + appliedRowTop[i],
                                              textWidth, appliedRowHeight[i], appliedSpellRows[i].text.c_str(),
                                              NH3Dlg::Text::SMALL, eTextAlignment::TOP_LEFT, rowHint.c_str()));
    }

    // 4) 스크롤바 (칸을 넘칠 때만)
    if (needScroll && appliedMaxTick > 0)
    {
        g_mvAppliedSpellsHandler = this;
        appliedScrollbar = H3DlgScrollbar::Create(x + width - MV_SCROLLBAR_WIDTH, y, MV_SCROLLBAR_WIDTH, height,
                                                  MV_ID_BASE + MV_SCROLLBAR_TEMPLATE_ID, appliedMaxTick + 1,
                                                  AppliedSpellsScrollbarProc, FALSE, 1, TRUE);
        if (appliedScrollbar)
            dlg->AddItem(appliedScrollbar);
    }

    LayoutAppliedSpellRows(false);
}

// 현재 스크롤 위치에 맞게 줄을 올리고, 칸 밖으로 나간 줄은 숨긴다 (숨긴 줄은 마우스에도 반응하지 않음)
void CreatureDlgHandler::LayoutAppliedSpellRows(bool redraw)
{
    if (!dlg)
        return;

    const int offset = (appliedScrollTick >= 0 && appliedScrollTick < static_cast<int>(appliedRowTop.size()))
                           ? appliedRowTop[appliedScrollTick]
                           : 0;
    const int bottom = appliedListY + appliedListHeight;

    for (size_t i = 0; i < appliedSpellItems.size(); ++i)
    {
        H3DlgText *item = appliedSpellItems[i];
        if (!item)
            continue;

        const int rowY = appliedListY + appliedRowTop[i] - offset;
        item->SetY(rowY);
        if (rowY >= appliedListY && rowY + appliedRowHeight[i] <= bottom)
            item->ShowActivate();
        else
            item->HideDeactivate();
    }

    if (redraw)
        dlg->Redraw(appliedListX, appliedListY, appliedListWidth, appliedListHeight);
}

void CreatureDlgHandler::ScrollAppliedSpells(int tick)
{
    appliedScrollTick = Clamp(0, tick, appliedMaxTick);
    ++appliedScrollSerial;
    LayoutAppliedSpellRows(true);
}

// 정보창 안 어디서든 휠을 굴리면 적용된 마법 목록을 한 줄씩 움직인다 (스크롤바가 있을 때만)
//   HD 모드도 "모든 스크롤바를 휠로 조작"하므로, 이번 메시지를 처리하는 동안 이미 목록이 움직였으면
//   (= HD 모드가 스크롤바를 움직였으면) 여기서는 건너뛴다 → 한 번에 두 줄씩 넘어가지 않음
void CreatureDlgHandler::HandleMouseWheel(H3Msg *msg)
{
    if (!dlg || !msg)
        return;

    const int command = static_cast<int>(msg->command);
    const int subtype = static_cast<int>(msg->subtype);

    if (MV_WHEEL_DEBUG && command != static_cast<int>(eMsgCommand::MOUSE_OVER))
    {
        if (auto *hintBar = dlg->GetTextPcx(DLG_HINT_ID))
        {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "cmd=%d, sub=%d", command, subtype);
            hintBar->SetText(buf);
            hintBar->Draw();
            hintBar->Refresh();
        }
    }

    const bool alreadyScrolled = appliedScrollSerial != appliedScrollSerialSeen;
    appliedScrollSerialSeen = appliedScrollSerial;

    if (command != MV_MSG_MOUSE_WHEEL || subtype == 0 || alreadyScrolled)
        return;
    if (!appliedScrollbar || appliedMaxTick <= 0)
        return;

    const int target = Clamp(0, appliedScrollTick + (subtype > 0 ? -1 : 1), appliedMaxTick);
    if (target == appliedScrollTick)
        return;

    ScrollAppliedSpells(target);
    MvSyncScrollbar(appliedScrollbar, target);
    appliedScrollSerialSeen = appliedScrollSerial;
}

void __fastcall CreatureDlgHandler::AppliedSpellsScrollbarProc(INT32 tick, H3BaseDlg *baseDlg)
{
    CreatureDlgHandler *handler = g_mvAppliedSpellsHandler;
    if (handler && reinterpret_cast<H3BaseDlg *>(handler->dlg) == baseDlg)
        handler->ScrollAppliedSpells(tick);
}

// 적용된 마법 한 줄을 우클릭했을 때 (마법책 우클릭과 같은 게임 내부 안내창)
//   본문: ERM(CM4)이 "걸린 수준" 기준으로 미리 만들어 둔 SN:W^Mv_AppSp_<n>_Popup^
//         비어 있으면 게임에 저장된 현재 설명 / 스크롤 마법은 z(400+번호)
//   그림: 효과 칸 39(오우거의 함성)·60(최면) → 그림 없이 단순 메시지
//         0~80번 → SPELLSCR 해당 그림 / 81번~(스크롤 마법) → 80번 그림을 바꿔서 표시
BOOL CreatureDlgHandler::ShowAppliedSpellPopup(int row)
{
    if (row < 0 || row >= static_cast<int>(appliedSpellRows.size()))
        return FALSE;

    const AppliedSpellRow &entry = appliedSpellRows[row];
    const int slot = entry.spellId;                                   // r
    const int code = entry.spellCode >= 0 ? entry.spellCode : slot;   // s
    const int level = Clamp(0, entry.level, 3);
    const bool simpleMessage = slot == MV_SLOT_OGRE_ROAR || slot == MV_SLOT_HYPNOTIZE;
    const bool normalSpell = code >= 0 && code < MV_SPELL_SLOT_COUNT;
    const bool scrollSpell = code >= MV_SCROLL_SPELL_FIRST && code <= MV_SCROLL_SPELL_LAST;

    // ---------- 본문 ----------
    std::string body = entry.popup;

    if (MvIsEmpty(body.c_str()))
    {
        if (slot == MV_SLOT_OGRE_ROAR)
            body = GetErtText(MV_ERT_OGRE_ROAR, "{오우거의 함성}");
        else if (slot == MV_SLOT_HYPNOTIZE)
            body = GetErtText(MV_ERT_HYPNOTIZE, "{최면}");
        else if (normalSpell)
        {
            const char* desc = H3Spell::Get()[code].description[level];
            if (!MvIsEmpty(desc))
                body = desc;
        }
        else if (scrollSpell)
            body = MvZ(400 + code);   // H3_Set_Spell_Descriptions가 채워 두는 z481~
    }
    if (MvIsEmpty(body.c_str()))
        body = GetErtText(MV_ERT_SPELL_NO_DESC, "이 효과에 대한 설명이 없습니다.");

    // ---------- 그림 + 표시 ----------
    if (simpleMessage)
    {
        MvShowSpellRmbBox(body.c_str(), -1);
    }
    else if (normalSpell)
    {
        MvShowSpellRmbBox(body.c_str(), code);
    }
    else if (scrollSpell)
    {
        // 스크롤 마법책 우클릭(ERM)과 같은 방식:
        //   !!SN:R^SPELLSCR.def:0_80.png^/^Data\defs\SPELLSCR\0_<번호>.png^;  + 80번 마법 이름을 잠깐 교체
        char picture[64];
        std::snprintf(picture, sizeof(picture), "Data\\defs\\SPELLSCR\\0_%d.png", code);
        Era::RedirectFile("SPELLSCR.def:0_80.png", picture);

        H3Spell &pictureSpell = H3Spell::Get()[MV_SCROLL_PICTURE_SLOT];
        const auto oldName = pictureSpell.name;
        const std::string scrollName = MvSpellNameFromRow(entry.text);
        if (!scrollName.empty())
            pictureSpell.name = scrollName.c_str();

        MvShowSpellRmbBox(body.c_str(), MV_SCROLL_PICTURE_SLOT);

        pictureSpell.name = oldName;   // 이름은 원래대로 (그림 교체는 ERM 쪽과 마찬가지로 그대로 둠)
    }
    else
    {
        MvShowSpellRmbBox(body.c_str(), -1);
    }
    return TRUE;
}

// =====================================================================
// [Multiverse] 비전투 유닛 정보창 도우미 (영웅·마을 부대 / 고용창)
// =====================================================================
namespace
{
    // 새 ERT 번호 4개 (ERT에 없으면 아래 코드의 기본 문구 사용. 이미 쓰는 번호라면 여기만 바꾸면 됨)
    constexpr int MV_ERT_NB_MORALE      = 202778;  // "사기: %s"   (%s 자리에 +7 같은 값)
    constexpr int MV_ERT_NB_LUCK        = 202779;  // "행운: %s"
    constexpr int MV_ERT_NB_MORALE_HINT = 202780;  // 사기 줄 마우스오버 힌트
    constexpr int MV_ERT_NB_LUCK_HINT   = 202781;  // 행운 줄 마우스오버 힌트

    // 여분발사횟수: 전투 때 ERM과 같은 조건 (사격 유닛만, 투석기 145·쇠뇌 146·화살탑 149 제외)
    //   값은 ERM이 게임 시작 때 !!SN:W^<번호>_Monster_Extra_Shots^/값; 으로 저장해 둔 것을 직접 읽는다.
    //   (SN:W 변수 = ERA 전역 이름 변수라서 C++에서 GetAssocVarIntValue로 바로 읽을 수 있음)
    //   사격 유닛이 아니면 -1 (= 표시 안 함)
    int MvExtraShots(int creatureId)
    {
        if (creatureId == 145 || creatureId == 146 || creatureId == 149)
            return -1;
        if (H3CreatureInformation::Get()[creatureId].numberShots <= 0)
            return -1;

        char varName[48];
        std::snprintf(varName, sizeof(varName), "%d_Monster_Extra_Shots", creatureId);
        return Era::GetAssocVarIntValue(varName);
    }

    // ---------------------------------------------------------------
    // [Multiverse] 타이푼 에디터(MCrEdit)의 MonstersSetup.mop 에서 반격횟수 읽기
    //   파일 구조 (유닛 1000칸 기준, 바이트 위치)
    //     0     ~ 15999 : 전투 DEF 이름 (16바이트씩)
    //     16000 ~ 20999 : 소리 이름 (5바이트씩)
    //     21000 ~ 24999 : 플래그 (4바이트씩)
    //     25000 ~ 26999 : 다른 표 2개 (1바이트씩)
    //     27000 ~ 27999 : 반격횟수 (1바이트씩, -1 = 무제한, 0 = 반격 불가)
    //   게임 실행 후 처음 한 번만 읽는다. 파일 크기가 다르면(형식이 바뀌면) 쓰지 않고 기본값 1로 표시.
    // ---------------------------------------------------------------
    constexpr int  MV_MOP_SLOTS              = 1000;
    constexpr long MV_MOP_FILE_SIZE          = 254000;
    constexpr long MV_MOP_RETALIATION_OFFSET = 27000;
    constexpr int  MV_RETALIATION_UNLIMITED  = 999;   // 100 이상은 ∞ 로 표시됨

    std::vector<signed char> g_mopRetaliations;       // 비어 있으면 파일을 못 읽은 것
    bool g_mopLoaded = false;

    void MvLoadMopRetaliations()
    {
        if (g_mopLoaded)
            return;
        g_mopLoaded = true;

        // 첫 번째: ERA가 모드 폴더를 합쳐서 보여 주는 경로 / 두 번째: 실제 폴더 경로
        const char* paths[] = {
            "EraPlugins/MCrEdit/MonstersSetup.mop",
            "Mods/New Upgrade Mod/EraPlugins/MCrEdit/MonstersSetup.mop",
        };
        for (const char* path : paths)
        {
            std::ifstream file(path, std::ios::binary);
            if (!file.is_open())
                continue;

            file.seekg(0, std::ios::end);
            if (static_cast<long>(file.tellg()) != MV_MOP_FILE_SIZE)
                continue;

            std::vector<char> buf(MV_MOP_SLOTS);
            file.seekg(MV_MOP_RETALIATION_OFFSET, std::ios::beg);
            if (!file.read(buf.data(), MV_MOP_SLOTS))
                continue;

            g_mopRetaliations.assign(buf.begin(), buf.end());
            return;
        }
    }

    // 유닛의 반격횟수 (타이푼 에디터 값, 무제한 = 999). 파일을 못 읽었으면 1
    int MvCreatureRetaliations(int creatureId)
    {
        MvLoadMopRetaliations();
        if (creatureId < 0 || creatureId >= static_cast<int>(g_mopRetaliations.size()))
            return 1;

        const int value = g_mopRetaliations[creatureId];
        return value < 0 ? MV_RETALIATION_UNLIMITED : value;
    }

    // 원래 창 아래쪽의 사기/행운 숫자 글자("+7", "0", "-2")를 읽는다. 글자가 없으면 false
    bool MvReadNativeNumber(H3CreatureInfoDlg* dlg, int textId, int& value)
    {
        H3DlgText* text = dlg ? dlg->GetText(textId) : nullptr;
        if (!text)
            return false;
        const char* s = text->GetH3String().String();
        if (MvIsEmpty(s))
            return false;

        // 숫자가 시작되는 곳까지 건너뛰기 (색상 태그 등이 앞에 붙어 있어도 읽을 수 있게)
        while (*s && !(*s >= '0' && *s <= '9') &&
               !((*s == '+' || *s == '-') && s[1] >= '0' && s[1] <= '9'))
            ++s;
        if (!*s)
            return false;

        value = std::atoi(s);
        return true;
    }

    // 전투 때와 같은 모양: 양수 "+7", 0은 "  0"(자리 맞춤), 음수 "-2"
    std::string MvSignedValue(int value)
    {
        char buf[16];
        if (value > 0)
            std::snprintf(buf, sizeof(buf), "+%d", value);
        else if (value == 0)
            std::snprintf(buf, sizeof(buf), "  0");
        else
            std::snprintf(buf, sizeof(buf), "%d", value);
        return buf;
    }

    // "%s" 자리에 글자 끼워넣기 (ERT 문구용)
    std::string MvReplaceText(std::string fmt, const std::string& value)
    {
        const auto pos = fmt.find("%s");
        if (pos != std::string::npos) fmt.replace(pos, 2, value);
        else fmt += value;
        return fmt;
    }

    // "{~White}10{~}" (전투 때 ERM이 만드는 값과 같은 모양)
    std::string MvWhiteNumber(int value)
    {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "{~White}%d{~}", value);
        return buf;
    }
}

// 비전투 창 (영웅·마을 부대 / 고용창)
//   전투 때와 같은 자리, 같은 템플릿 번호를 쓰므로
//   마우스오버 힌트(아래 상태바)와 우클릭 안내창(HandleMsg)이 전투 때와 똑같이 동작한다.
void CreatureDlgHandler::AddNonBattleStats()
{
    const int creatureId = dlg->creatureId;
    if (creatureId < 0)
        return;

    // ---------- 0. 필요한 값 모으기 ----------
    int count = 0;                                    // 부대 수량 (고용창처럼 알 수 없으면 0)
    if (army && armySlotIndex >= 0 && armySlotIndex < 7)
        count = army->count[armySlotIndex];

    int morale = 0;
    int luck = 0;
    const bool hasMorale = MvReadNativeNumber(dlg, DLG_MORALE_TEXT_ID, morale);
    const bool hasLuck = MvReadNativeNumber(dlg, DLG_LUCK_TEXT_ID, luck);

    const int extraShots = MvExtraShots(creatureId);             // 여분발사횟수 (-1 = 사격 유닛 아님)
    const int retaliations = MvCreatureRetaliations(creatureId); // 반격횟수 (타이푼 에디터 값)

    // ---------- 1. 마법횟수 (유닛 기본값, MA:B로 바꾼 값도 그대로 반영됨) ----------
    const int charges = H3CreatureInformation::Get()[creatureId].spellCharges;
    if (charges > 0)
    {
        const std::string spellLabel = GetErtText(202756, "{~White}마법횟수{~}");
        const std::string spellHint  = GetErtText(202763, "남은 주문 시전 횟수 / 최대 시전 횟수입니다.");
        MvAddText(dlg, 110, 311, 67, 110, 17, spellLabel.c_str(), NH3Dlg::Text::MEDIUM, eTextAlignment::TOP_LEFT, spellHint.c_str());

        char spellBuf[16];
        std::snprintf(spellBuf, sizeof(spellBuf), "%d", charges);
        MvAddText(dlg, 210, 311, 67, 118, 17, spellBuf, NH3Dlg::Text::SMALL, eTextAlignment::TOP_RIGHT, spellHint.c_str());
    }

    // ---------- 2. 여분발사횟수 (ERM SN:W 값) ----------
    if (extraShots >= 0)
    {
        const std::string shotLabel = GetErtText(202757, "{~White}여분발사횟수{~}");
        const std::string shotHint  = GetErtText(202764, "이번 전투에서 사용할 수 있는 추가 사격 횟수입니다.");
        const std::string shotValue = MvWhiteNumber(extraShots);
        MvAddText(dlg, 104, 311, 86, 110, 17, shotLabel.c_str(), NH3Dlg::Text::MEDIUM, eTextAlignment::TOP_LEFT, shotHint.c_str());
        MvAddText(dlg, 204, 311, 86, 118, 17, shotValue.c_str(), NH3Dlg::Text::SMALL, eTextAlignment::TOP_RIGHT, shotHint.c_str());
    }

    // ---------- 3. 원래 수량 (전투 밖에서는 지금 수량) ----------
    if (count > 0)
    {
        const std::string origLabel = GetErtText(202760, "{~White}원래 수량:{~}");
        const std::string origHint  = GetErtText(202765, "부대의 원래 개체 수입니다.");
        const std::string origValue = MvWhiteNumber(count);
        MvAddText(dlg, 30, 48, 185, 113, 17, origLabel.c_str(), NH3Dlg::Text::MEDIUM, eTextAlignment::TOP_LEFT, origHint.c_str());
        MvAddText(dlg, 40, 48, 185, 113, 17, origValue.c_str(), NH3Dlg::Text::SMALL, eTextAlignment::TOP_RIGHT, origHint.c_str());
    }

    // ---------- 4. 반격횟수 (타이푼 에디터 값 / 100 이상은 ∞) ----------
    {
        const std::string retValue = retaliations > 99 ? "{~White}" + GetErtText(202712, "∞") + "{~}"
                                                       : MvWhiteNumber(retaliations);
        const std::string retLabel = GetErtText(202761, "{~White}반격횟수{~}");
        const std::string retHint  = GetErtText(202766, "한 라운드에 적의 공격에 대응해 반격할 수 있는 횟수입니다.");
        MvAddText(dlg, 109, 194, 185, 80, 17, retLabel.c_str(), NH3Dlg::Text::MEDIUM, eTextAlignment::TOP_LEFT, retHint.c_str());
        MvAddText(dlg, 209, 194, 185, 80, 17, retValue.c_str(), NH3Dlg::Text::SMALL, eTextAlignment::TOP_RIGHT, retHint.c_str());
    }

    // ---------- 5. 사기 / 행운 (아래쪽 원래 아이콘을 숨기고 글자로 표시) ----------
    if (hasMorale)
    {
        MvHide(dlg, DLG_MORALE_ID);
        MvHide(dlg, DLG_MORALE_TEXT_ID);
        const std::string moraleText = MvReplaceText(GetErtText(MV_ERT_NB_MORALE, "사기: %s"), MvSignedValue(morale));
        const std::string moraleHint = GetErtText(MV_ERT_NB_MORALE_HINT, "부대의 사기 수치");
        MvAddText(dlg, 212, 48, 204, 227, 17, moraleText.c_str(), NH3Dlg::Text::SMALL, eTextAlignment::TOP_LEFT, moraleHint.c_str());
    }
    if (hasLuck)
    {
        MvHide(dlg, DLG_LUCK_ID);
        MvHide(dlg, DLG_LUCK_TEXT_ID);
        const std::string luckText = MvReplaceText(GetErtText(MV_ERT_NB_LUCK, "행운: %s"), MvSignedValue(luck));
        const std::string luckHint = GetErtText(MV_ERT_NB_LUCK_HINT, "부대의 행운 수치");
        MvAddText(dlg, 213, 48, 223, 227, 17, luckText.c_str(), NH3Dlg::Text::SMALL, eTextAlignment::TOP_LEFT, luckHint.c_str());
    }

    // ---------- 6. 특수능력 (원래 게임의 특수능력 설명) ----------
    //   전투력 줄(244)이 없으므로 한 줄 위로 올리고, 아래 버튼 줄 바로 위까지 쓴다.
    const std::string specLabel = GetErtText(202758, "{~White}특수능력:{~}");
    const std::string specHint  = GetErtText(202767, "현재 부대가 보유한 고유 특수 능력 정보입니다.");
    const std::string specText  = GetDescriptionText(FindOriginalDescription());

    const int specTop = 259;
    int specBottom = BUTTON_Y - BUTTON_FRAME_BORDER - 2;
    if (specBottom < specTop + 30)
        specBottom = specTop + 30;

    MvAddText(dlg, 37, 20, 244, 257, 17, specLabel.c_str(), NH3Dlg::Text::TINY, eTextAlignment::TOP_LEFT, specHint.c_str());
    MvAddText(dlg, 38, 20, specTop, 257, specBottom - specTop, specText.c_str(), NH3Dlg::Text::TINY, eTextAlignment::TOP_LEFT, specHint.c_str());
}

// 커스텀 유닛 정보창 전체 배치 (z11~z22, SN:W^Mv_AppSp_..^ + C++ 계산값)
void CreatureDlgHandler::AddCustomStats()
{
    if (!dlg)
        return;

    // [Multiverse] 비전투 창 (영웅·마을 부대, 고용창) → 별도 처리
    if (!stack)
    {
        AddNonBattleStats();
        return;
    }

    // ---------- 0. 원본 항목 숨기기 ----------
    MvHide(dlg, DLG_NAME_ID);
    MvHide(dlg, DLG_MORALE_ID);
    MvHide(dlg, DLG_MORALE_TEXT_ID);
    MvHide(dlg, DLG_LUCK_ID);
    MvHide(dlg, DLG_LUCK_TEXT_ID);
    if (auto* desc = FindOriginalDescription())
        desc->HideDeactivate();

    // ---------- 1. 유닛 이름 (z11) ----------
    MvAddText(dlg, 1, 20, 23, 409, 17, MvZ(11), NH3Dlg::Text::MEDIUM, MV_TOP_CENTER);

    // ---------- 2. 상태 (ERT 202762번 힌트 연동) ----------
    std::string statusLabel = GetErtText(202755, "{~White}상태{~}");
    std::string statusHint  = GetErtText(202762, "현재 부대의 전투 행동 상태입니다.");
    
    MvAddText(dlg, 111, 311, 48, 110, 17, statusLabel.c_str(), NH3Dlg::Text::MEDIUM, eTextAlignment::TOP_LEFT, statusHint.c_str());

    std::string statusText = GetErtText(202714, "{~Cyan}행동 전{~}");
    if (stack->IsWaiting())
        statusText = GetErtText(202715, "{~y}대기중{~}");
    else if (stack->IsDefending())
        statusText = GetErtText(202717, "방어");
    else if (stack->IsDone())
        statusText = GetErtText(202716, "행동 끝");
    MvAddText(dlg, 211, 311, 48, 118, 17, statusText.c_str(), NH3Dlg::Text::SMALL, eTextAlignment::TOP_RIGHT, statusHint.c_str());

    // ---------- 3. 마법횟수 (ERT 202763번 힌트 연동) ----------
    const int baseCharges = (dlg->creatureId >= 0) ? H3CreatureInformation::Get()[dlg->creatureId].spellCharges : 0;
    const int curCharges  = stack->info.spellCharges;
    if (baseCharges > 0 || curCharges > 0)
    {
        std::string spellLabel = GetErtText(202756, "{~White}마법횟수{~}");
        std::string spellHint  = GetErtText(202763, "남은 주문 시전 횟수 / 최대 시전 횟수입니다.");
        
        MvAddText(dlg, 110, 311, 67, 110, 17, spellLabel.c_str(), NH3Dlg::Text::MEDIUM, eTextAlignment::TOP_LEFT, spellHint.c_str());

        char spellBuf[32];
        if (baseCharges == curCharges)
            std::snprintf(spellBuf, sizeof(spellBuf), "%d", curCharges);
        else
            std::snprintf(spellBuf, sizeof(spellBuf), "%d(%d)", baseCharges, curCharges);
        MvAddText(dlg, 210, 311, 67, 118, 17, spellBuf, NH3Dlg::Text::SMALL, eTextAlignment::TOP_RIGHT, spellHint.c_str());
    }

    // ---------- 4. 여분발사횟수 (ERT 202764번 힌트 연동) ----------
    if (!MvIsEmpty(MvZ(15)))
    {
        std::string shotLabel = GetErtText(202757, "{~White}여분발사횟수{~}");
        std::string shotHint  = GetErtText(202764, "이번 전투에서 사용할 수 있는 추가 사격 횟수입니다.");
        
        MvAddText(dlg, 104, 311, 86, 110, 17, shotLabel.c_str(), NH3Dlg::Text::MEDIUM, eTextAlignment::TOP_LEFT, shotHint.c_str());
        MvAddText(dlg, 204, 311, 86, 118, 17, MvZ(15), NH3Dlg::Text::SMALL, eTextAlignment::TOP_RIGHT, shotHint.c_str());
    }

    // ---------- 5. 원래 수량 (ERT 202765번 힌트 연동) ----------
    std::string origLabel = GetErtText(202760, "{~White}원래 수량:{~}");
    std::string origHint  = GetErtText(202765, "부대의 소환 또는 부활 전 원래 개체 수입니다.");
    
    MvAddText(dlg, 30, 48, 185, 113, 17, origLabel.c_str(), NH3Dlg::Text::MEDIUM, eTextAlignment::TOP_LEFT, origHint.c_str());
    MvAddText(dlg, 40, 48, 185, 113, 17, MvZ(22), NH3Dlg::Text::SMALL, eTextAlignment::TOP_RIGHT, origHint.c_str());

    // ---------- 6. 반격횟수 (ERT 202766번 힌트 연동) ----------
    std::string retLabel = GetErtText(202761, "{~White}반격횟수{~}");
    std::string retHint  = GetErtText(202766, "이번 턴에 적의 공격에 대응하여 반격할 수 있는 남은 횟수입니다.");
    
    MvAddText(dlg, 109, 194, 185, 80, 17, retLabel.c_str(), NH3Dlg::Text::MEDIUM, eTextAlignment::TOP_LEFT, retHint.c_str());
    MvAddText(dlg, 209, 194, 185, 80, 17, MvZ(12), NH3Dlg::Text::SMALL, eTextAlignment::TOP_RIGHT, retHint.c_str());

    // ---------- 7. 사기 (z16) + 설명 (z17) ----------
    MvAddText(dlg, 212, 48, 204, 227, 17, MvZ(16), NH3Dlg::Text::SMALL, eTextAlignment::TOP_LEFT, MvZ(17));
    MvAddText(dlg, 312, 108, 206, 167, 17, MvZ(17), NH3Dlg::Text::TINY, eTextAlignment::TOP_LEFT, MvZ(17));

    // ---------- 8. 행운 (z18) + 설명 (z19) ----------
    MvAddText(dlg, 213, 48, 223, 227, 17, MvZ(18), NH3Dlg::Text::SMALL, eTextAlignment::TOP_LEFT, MvZ(19));
    MvAddText(dlg, 313, 108, 225, 167, 17, MvZ(19), NH3Dlg::Text::TINY, eTextAlignment::TOP_LEFT, MvZ(19));

    // ---------- 9. 전투력 + 지속시간 힌트 (z13) ----------
    std::string powerHint = GetErtText(202769, "전투력이 높을수록 걸리는 마법의 지속시간이 짧아짐");
    MvAddText(dlg, 31, 20, 244, 257, 30, MvZ(13), NH3Dlg::Text::TINY, eTextAlignment::TOP_LEFT, powerHint.c_str());

    // ---------- 10. 특수능력 라벨 + 설명 (z14) ----------
    std::string specLabel = GetErtText(202758, "{~White}특수능력:{~}");
    std::string specHint  = GetErtText(202767, "현재 부대가 보유한 고유 특수 능력 정보입니다.");
    
    MvAddText(dlg, 37, 20, 274, 257, 17, specLabel.c_str(), NH3Dlg::Text::TINY, eTextAlignment::TOP_LEFT, specHint.c_str());
    // 설명 부분(38)의 힌트를 MvZ(14)에서 specHint.c_str()로 변경하여 202767 문구가 뜨도록 통일
    MvAddText(dlg, 38, 20, 289, 257, 56, MvZ(14), NH3Dlg::Text::TINY, eTextAlignment::TOP_LEFT, specHint.c_str());

    // ---------- 11. 추가 설명: 방해광선/복제 등 (z20) ----------
    MvAddText(dlg, 41, 20, 345, 257, 30, MvZ(20), NH3Dlg::Text::TINY, eTextAlignment::TOP_LEFT, MvZ(20));

    // ---------- 12. 적용된 마법 (SN:W^Mv_AppSp_..^ 줄 단위) (ERT 202768번 힌트 연동) ----------
    const bool perRow = CollectAppliedSpellRows();
    const char* applied = MvZ(21);   // ERM을 아직 안 바꿨을 때만 쓰는 예전 통짜 목록
    const int appliedCount = perRow ? static_cast<int>(appliedSpellRows.size()) : MvCountLines(applied);
    std::string appliedLabel = MvFormatCount(GetErtText(202759, "{~LightBlue}적용된 마법 (%d){~}"), appliedCount);
    std::string appliedHint  = GetErtText(202768, "현재 부대에 영향을 주고 있는 마법 및 지속시간 목록입니다.");

    MvAddText(dlg, 35, 287, 108, 141, 17, appliedLabel.c_str(), NH3Dlg::Text::MEDIUM, MV_TOP_CENTER, appliedHint.c_str());

    // 목록 칸 아래 끝이 아래쪽 버튼 줄(OK·마법 등)의 테두리에 닿지 않게 한다 (버튼 위치가 바뀌어도 자동으로 맞춰짐)
    int listHeight = MV_APPLIED_LIST_HEIGHT;
    const int buttonTop = BUTTON_Y - BUTTON_FRAME_BORDER - 2;
    if (MV_APPLIED_LIST_Y + listHeight > buttonTop)
        listHeight = std::max(16, buttonTop - MV_APPLIED_LIST_Y);

    if (perRow)   // 한 줄 = 한 아이템(템플릿 500+n), 넘치면 스크롤바
        BuildAppliedSpellList(MV_APPLIED_LIST_X, MV_APPLIED_LIST_Y, MV_APPLIED_LIST_WIDTH, listHeight);
    else
        MvAddText(dlg, 36, MV_APPLIED_LIST_X, MV_APPLIED_LIST_Y, MV_APPLIED_LIST_WIDTH, listHeight, applied,
                  NH3Dlg::Text::SMALL, eTextAlignment::TOP_LEFT, appliedHint.c_str());
}

CreatureDlgHandler::CreatureDlgHandler(H3CreatureInfoDlg *dlg, H3CombatCreature *stack, H3Army *army, int armySlotIndex,
                                     const H3Hero *hero, int playerColor)
    : dlg(dlg), stack(stack), army(army), armySlotIndex(armySlotIndex), wogStackExperience(WogStackExperienceEnabled()),
      hero(hero), playerColor(playerColor)
{
    if (!dlg)
        return;

    ApplyDialogAppearance();
    ReadDescriptionRect(descriptionX, descriptionY, descriptionWidth, descriptionHeight);
    if (GetPluginSettings().showCommanderSkills)
        AddCommanderSkills();
    AlignItems();
    
    AddCustomStats();

    if (wogStackExperience && dlg->GetDefButton(DLG_OK_ID))
        AddExperienceButton();
    if (stack)
    {
        HideDefaultBattleSpellItems();
        // AddSpellEffects();   // [Multiverse] z21 텍스트 목록으로 대체
    }
}

CreatureDlgHandler::~CreatureDlgHandler()
{
    if (g_mvAppliedSpellsHandler == this)
        g_mvAppliedSpellsHandler = nullptr;
    ReleaseUnownedSkillImages();
}

void CreatureDlgHandler::ApplyDialogAppearance()
{
    if (!dlg)
        return;

    dlg->SetWidth(DLG_WIDTH);
    dlg->SetHeight(DLG_HEIGHT);

    if (auto *background = dlg->GetPcx(DLG_BACKGROUND_ID))
    {
        background->SetWidth(DLG_WIDTH);
        background->SetHeight(DLG_HEIGHT);
        background->SetPcx(DIALOG_BACKGROUND_PCX);

        if (playerColor >= 0 && playerColor < limits::PLAYERS)
            background->AdjustColor(playerColor);
    }

    if (auto *unitDef = dlg->GetDef(27))
    {
        unitDef->SetX(unitDef->GetX() - 1);
    }
    else if (auto *unitDefFallback = dlg->GetDef(0))
    {
        unitDefFallback->SetX(unitDefFallback->GetX() - 1);
    }
}

void CreatureDlgHandler::ScrollTo(int scrollTick)
{
    if (!dlg)
        return;

    scrollTick = Clamp(0, scrollTick, maxScrollTick);
    currentScrollTick = scrollTick;

    int scrollOffset = 0;
    if (!scrollRowOffsets.empty())
    {
        const int rowIndex = std::min(scrollTick, static_cast<int>(scrollRowOffsets.size()) - 1);
        scrollOffset = scrollRowOffsets[rowIndex];
    }

    const int viewportBottom = viewportY + viewportHeight;
    for (auto &entry : scrolledItems)
    {
        if (!entry.item)
            continue;

        const int y = entry.baseY - scrollOffset;
        entry.item->SetY(y);
        if (y >= viewportY && y + entry.item->GetHeight() <= viewportBottom)
            entry.item->ShowActivate();
        else
            entry.item->HideDeactivate();
    }

    dlg->Redraw(viewportX, viewportY, viewportWidth, viewportHeight);
}

H3DlgText *CreatureDlgHandler::FindOriginalDescription() const
{
    H3DlgText *description = dlg ? dlg->GetText(DLG_DESCRIPTION_ID) : nullptr;
    if (!description && dlg)
        description = dlg->GetText(DLG_DESCRIPTION_FALLBACK_ID);
    return description;
}

std::string CreatureDlgHandler::GetDescriptionText(H3DlgText *description) const
{
    const char *text = description ? description->GetH3String().String() : nullptr;
    if ((!text || !*text) && dlg && dlg->creatureId >= 0 && H3CreatureInformation::Get()[dlg->creatureId].description)
        text = H3CreatureInformation::Get()[dlg->creatureId].description;

    std::string result(text ? text : h3_NullString);
    if (commanderPanelHeight)
    {
        const auto first = result.find('[');
        const auto last = first == std::string::npos ? first : result.find(']', first + 1);
        if (last != std::string::npos)
            result.erase(first, last - first + 1);
    }
    return result;
}

BOOL CreatureDlgHandler::AlignItems()
{
    if (!dlg)
        return FALSE;

    if (auto *hint = dlg->GetTextPcx(DLG_HINT_ID))
    {
        hint->SetX(HINT_MARGIN);
        // 기존: hint->SetY(DLG_HEIGHT - hint->GetHeight() - HINT_MARGIN);
        // 변경: 아래로 5픽셀(칸) 내리기 위해 + 5 추가
        hint->SetY(DLG_HEIGHT - hint->GetHeight() - HINT_MARGIN + 5); 
        hint->SetWidth(DLG_WIDTH - HINT_MARGIN * 2);
    }
    if (auto *name = dlg->GetText(DLG_NAME_ID))
        name->SetX((DLG_WIDTH - name->GetWidth()) / 2);

    constexpr struct MoraleLuckPosition
    {
        int iconId, textId, x;
    } positions[] = {{DLG_MORALE_ID, DLG_MORALE_TEXT_ID, MORALE_X},
                     {DLG_LUCK_ID, DLG_LUCK_TEXT_ID, LUCK_X}};
    for (const auto &position : positions)
    {
        if (auto *icon = dlg->GetDef(position.iconId))
        {
            icon->SetX(position.x);
            icon->SetY(DLG_HEIGHT - icon->GetHeight() - MORALE_LUCK_BOTTOM_MARGIN);
            if (auto *text = dlg->GetText(position.textId))
                text->SetY(icon->GetY() + icon->GetHeight() - text->GetHeight());
        }
    }
    constexpr int buttonIds[] = {DLG_OK_ID, DLG_DISMISS_ID, DLG_UPGRADE_ID, DLG_CAST_ID};
    for (const int id : buttonIds)
    {
        int x = 0;
        if (GetDialogButtonX(id, x))
        {
            // [Multiverse] 비전투 창: 해고 버튼을 왼쪽 칸 오른쪽 끝으로 (테두리는 CreatureDlgHooks.cpp에서 같이 이동)
            if (id == DLG_DISMISS_ID && !stack)
                x += NONBATTLE_DISMISS_SHIFT_X;
            SetDialogButtonBounds(dlg->GetH3DlgItem(id), x);
        }
    }
    return BuildDescriptionArea();
}

BOOL CreatureDlgHandler::BuildDescriptionArea()
{
    // [Multiverse] 원본 설명 영역은 사용하지 않는다. 특수능력은 AddCustomStats()에서 z14로 직접 그림.
    ResetExperienceSkillsPanel();
    ReleaseUnownedSkillImages();
    if (H3DlgText* description = FindOriginalDescription())
        description->HideDeactivate();
    return FALSE;
}


BOOL CreatureDlgHandler::AddExperienceButton()
{
    const bool isNpc = Era::IsCommanderId(dlg->creatureId);
    if (isNpc && !stack)
        return FALSE;

    constexpr int x = EXPERIENCE_BUTTON_X;
    const int y = BUTTON_Y;
    H3DlgPcx *frame =
        H3DlgPcx::Create(x - BUTTON_FRAME_BORDER, y - BUTTON_FRAME_BORDER, DLG_DESCRIPTION_ID, BUTTON_FRAME_PCX);
    if (frame)
        dlg->AddItem(frame);

    H3DlgDefButton *button =
        H3DlgDefButton::Create(x, y, WOG_CREATURE_EXP_BUTTON_ID, EXPERIENCE_BUTTON_DEF, 0, 1, false, eVKey::H3VK_E);
    if (!button)
        return FALSE;
    const char *hint = isNpc ? Era::tr("eci.combat_dialog.creature_info.npc_hint")
                             : Era::tr("eci.combat_dialog.creature_info.stack_exp_hint");
    button->SetHints(hint, h3_NullString, true);
    dlg->AddItem(button);
    return TRUE;
}

BOOL CreatureDlgHandler::AddSpellEffects()
{
    return FALSE;   // [Multiverse] 마법 목록은 z21 텍스트로 대체 — daemon 아이콘/x턴 표시 비활성화
    if (!stack)
        return FALSE;
    const auto spells = CollectActiveSpells(stack);
    const bool needButton = spells.count > SPELL_VISIBLE_ROWS;
    const int visibleCount = needButton ? SPELL_ROWS_WITH_BUTTON : spells.count;
    for (int row = 0; row < visibleCount; ++row)
    {
        const int spellId = spells.ids[row];
        const int y = SPELL_COLUMN_Y + row * SPELL_ROW_HEIGHT;
        auto *icon = H3DlgDef::Create(SPELL_COLUMN_X, y, DLG_SPELL_FIRST_ID + row,
                                      NH3Dlg::Assets::SPELL_SMALL, spellId + SPELL_DEF_FRAME_OFFSET);
        if (!icon)
            continue;
        SetCreatureSpellHints(icon, stack, spellId);
        dlg->AddItem(icon);
        if (HasVisibleSpellDuration(spellId))
        {
            char duration[SPELL_DURATION_BUFFER_SIZE];
            std::snprintf(duration, sizeof(duration), "x%d", stack->activeSpellDuration[spellId]);
            auto *text = H3DlgText::Create(icon->GetX() + icon->GetWidth() - SPELL_DURATION_WIDTH,
                                         icon->GetY() + icon->GetHeight() - SPELL_DURATION_HEIGHT,
                                         SPELL_DURATION_WIDTH, SPELL_DURATION_HEIGHT, duration, NH3Dlg::Text::TINY,
                                         eTextColor::WHITE, DLG_SPELL_DURATION_FIRST_ID + row,
                                         eTextAlignment::BOTTOM_RIGHT);
            if (text)
            {
                SetCreatureSpellHints(text, stack, spellId);
                dlg->AddItem(text);
            }
        }
    }
    if (needButton)
    {
        auto *button = H3DlgDefButton::Create(SPELL_COLUMN_X, SPELL_COLUMN_Y + visibleCount * SPELL_ROW_HEIGHT,
                                              DLG_SPELLS_BTTN_ID, SPELL_LIST_BUTTON_DEF, 0, 1, false, eVKey::H3VK_S);
        if (button)
        {
            button->SetHints(Era::tr("eci.combat_dialog.creature_info.spell_list_hint"), h3_NullString, true);
            dlg->AddItem(button);
        }
    }
    return TRUE;
}


BOOL CreatureDlgHandler::HandleMsg(H3Msg* msg)
{
    if (!dlg || !msg)
        return FALSE;

    // 우클릭 이벤트 감지 (RBUTTON_DOWN)
    if (msg->command == eMsgCommand::MOUSE_BUTTON && msg->subtype == eMsgSubtype::RBUTTON_DOWN)
    {
        int itemID = msg->itemId;
        
        // 커스텀 생성한 아이템 ID 범위 확인 (MV_ID_BASE = 30000)
        if (itemID >= 30000 && itemID < 31000)
        {
            int templateId = itemID - 30000;

            // [추가] 적용된 마법 개별 행 → 해당 마법 안내 창
            if (templateId >= MV_SPELL_ROW_FIRST && templateId < MV_SPELL_ROW_FIRST + MV_SPELL_ROW_MAX)
                return ShowAppliedSpellPopup(templateId - MV_SPELL_ROW_FIRST);

            std::string popupText = "";
            
            // 팝업 창 기본 크기 설정 (일반 항목용)
            int popupWidth = 400;
            int popupHeight = 150;

            switch (templateId)
            {
                case 111: // 상태 라벨
                case 211: // 상태 텍스트
                    popupText = GetErtText(202762, "현재 부대의 전투 행동 상태입니다.");
                    break;

                case 110: // 마법횟수 라벨
                case 210: // 마법횟수 텍스트
                    popupText = GetErtText(202763, "남은 주문 시전 횟수 / 최대 시전 횟수입니다.");
                    break;

                case 104: // 여분발사횟수 라벨
                case 204: // 여분발사횟수 텍스트
                    popupText = GetErtText(202770, "멀티버스 모드에만 있는 시스템으로 사격 유닛이 이동이나 방어를 하면 이 수치로 발사횟수를 다시 늘릴 수 있습니다.\n탄약 수레가 있을 경우 이 수치는 항상 최댓값을 유지합니다.");
                    // ★ 글 내용이 길므로 세로 크기 확대
                    popupHeight = 240; 
                    break;

                case 30:  // 원래 수량 라벨
                case 40:  // 원래 수량 텍스트
                    popupText = GetErtText(202765, "부대의 소환 또는 부활 전 원래 개체 수입니다.");
                    break;

                case 109: // 반격횟수 라벨
                case 209: // 반격횟수 텍스트
                    popupText = GetErtText(202766, "이번 턴에 적의 공격에 대응하여 반격할 수 있는 남은 횟수입니다.");
                    break;

                case 31:  // 전투력
                    popupText = GetErtText(202771, "부대의 전투력은 보조/방해 마법의 지속시간을 결정합니다. 전투력이 높은 부대일수록 적용하려는 마법의 지속시간이 짧아지거나 특정 마법의 경우 걸 수 없게 됩니다.\n이런 경우 영웅의 마력이 더 높아야만 마법을 제대로 걸 수 있습니다.");
                    // ★ 글 내용이 길므로 세로 크기 확대
                    popupHeight = 260; 
                    break;

                case 212: // 사기 텍스트
                case 312: // 사기 설명 텍스트
                    popupText = GetErtText(202772, "{부대 사기}\n\n부대의 사기 수치입니다. 사기가 높을수록 전투 시 추가 행동을 할 확률이 높아집니다.");
                    popupWidth = 460;
                    popupHeight = 200;
                    break;

                case 213: // 행운 텍스트
                case 313: // 행운 설명 텍스트
                    popupText = GetErtText(202773, "{부대 행운}\n\n부대의 행운 수치입니다. 행운이 높을수록 전투 시 공격 피해량이 2배로 증가할 확률이 높아집니다.");
                    popupWidth = 460;
                    popupHeight = 200;
                    break;
                
                case 37:  // 특수능력 라벨
                case 38:  // 특수능력 설명
                    popupText = GetErtText(202767, "현재 부대가 보유한 고유 특수 능력 정보입니다.");
                    break;

                case 35:  // 적용된 마법 라벨
                case 36:  // 적용된 마법 목록
                    popupText = GetErtText(202768, "현재 부대에 영향을 주고 있는 마법 및 지속시간 목록입니다.");
                    break;

                default:
                    break;
            }

            if (!popupText.empty())
            {
                // 동적으로 설정된 크기(popupWidth, popupHeight)를 적용하여 다이얼로그 생성
                H3Dlg popup(popupWidth, popupHeight);
                
                // 텍스트 영역도 창 크기에 맞춰 여유 있게 조절 (좌우 패딩 각 20씩, 높이는 창 높이에 맞춤)
                auto* textItem = H3DlgText::Create(20, 20, popupWidth - 40, popupHeight - 40, popupText.c_str(), NH3Dlg::Text::SMALL, eTextColor::WHITE, 100, eTextAlignment::MIDDLE_CENTER);
                if (textItem)
                    popup.AddItem(textItem);
                    
                popup.PlaceAtMouse();
                popup.RMB_Show();
                return TRUE; // 우클릭 처리 완료
            }
        }
    }

    return FALSE;
}