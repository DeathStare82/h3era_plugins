#include "pch.h"

#include "CreatureDlgHandler.h"
#include "CreatureDlgHooks.h"
#include "CreatureDlgLayout.h"
#include "PluginSettings.h"
#include "CreatureSpellEffects.h"

#include <algorithm>
#include <string>
#include <cstdio>
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

    for (int id = DLG_NATIVE_SPELL_FIRST_ID; id <= DLG_NATIVE_SPELL_LAST_ID; ++id)
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

    // ERM 연동: 적용된 마법 줄 우클릭 → ERM(!?FU(OnMvAppliedSpellRmb))이 설명을 만들어 IF:Q로 띄운다.
    // v 번호가 이미 쓰이는 중이면 여기와 ERM 양쪽을 같이 바꿀 것.
    constexpr int MV_V_SPELL_RMB_ID      = 990;   // v990 = 효과 칸 번호 (ERM 루프의 r)
    constexpr int MV_V_SPELL_RMB_LEVEL   = 991;   // v991 = 걸린 수준 (0=기본 ~ 3=고급)
    constexpr int MV_V_SPELL_RMB_HANDLED = 992;   // v992 = ERM이 안내창을 띄웠으면 1
    constexpr LPCSTR MV_EVT_SPELL_RMB    = "OnMvAppliedSpellRmb";

    // 게임 내부 안내창 (마법책 우클릭 창, ERM IF:Q 와 같은 함수)
    //   style 4 = 우클릭을 누르고 있는 동안만 표시 / 그림 종류 9 = 마법 (SPELLSCR 그림 + 아래에 마법 이름)
    //   창 크기는 글 길이에 맞춰 게임이 알아서 정한다.
    using MvNativeMessageBox = void(__fastcall *)(const char* text, int style, int x, int y,
                                                  int type1, int subtype1, int type2, int subtype2,
                                                  int par, int time, int type3, int subtype3);
    constexpr DWORD MV_NATIVE_MESSAGE_BOX = 0x4F6C00;
    constexpr int   MV_MSGBOX_RMB = 4;
    constexpr int   MV_PIC_SPELL  = 9;

    void MvShowSpellRmbBox(const char* text, int spellId)
    {
        const bool hasPicture = spellId >= 0 && spellId < MV_SPELL_SLOT_COUNT;
        reinterpret_cast<MvNativeMessageBox>(MV_NATIVE_MESSAGE_BOX)(
            text, MV_MSGBOX_RMB, -1, -1,
            hasPicture ? MV_PIC_SPELL : -1, hasPicture ? spellId : 0,
            -1, 0, -1, 0, -1, 0);
    }

    struct MvSpellRef
    {
        int spellId;
        int level;
    };

    // z21 → 줄 단위로 분리 (MvCountLines와 같은 기준)
    std::vector<std::string> MvSplitLines(const char* s)
    {
        std::vector<std::string> out;
        if (MvIsEmpty(s)) return out;
        std::string cur;
        for (; *s; ++s)
        {
            if (*s == '\r') continue;
            if (*s == '\n') { out.push_back(cur); cur.clear(); }
            else cur += *s;
        }
        out.push_back(cur);
        return out;
    }

    // z23 = "41:3 46:0 " 형식 → {번호, 레벨} 목록
    std::vector<MvSpellRef> MvParseSpellRefs(const char* s)
    {
        std::vector<MvSpellRef> out;
        if (MvIsEmpty(s)) return out;
        std::istringstream in(s);
        std::string token;
        while (in >> token)
        {
            int id = -1, lv = 0;
            if (std::sscanf(token.c_str(), "%d:%d", &id, &lv) >= 1)
                out.push_back({id, lv});
        }
        return out;
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

// 적용된 마법 목록을 "한 줄 = 한 아이템"으로 만든다.
// z21(표시 문자열)과 z23(번호:레벨)은 ERM의 같은 루프에서 같은 순서로 쌓이므로 n번째끼리 짝이 맞는다.
void CreatureDlgHandler::BuildAppliedSpellList(int x, int y, int width, int height, const std::string &fallbackHint)
{
    appliedSpellRows.clear();

    const char* applied = MvZ(21);
    if (MvIsEmpty(applied))
        return;

    const std::vector<std::string> lines = MvSplitLines(applied);
    const std::vector<MvSpellRef> refs = MvParseSpellRefs(MvZ(23));

    // ERM이 z23을 아직 안 채웠거나 줄 수가 어긋나면 → 예전처럼 통짜 텍스트(우클릭 시 202768 공통 안내)
    if (refs.size() != lines.size())
    {
        MvAddText(dlg, 36, x, y, width, height, applied, NH3Dlg::Text::SMALL, eTextAlignment::TOP_LEFT, fallbackHint.c_str());
        return;
    }

    const std::string rowHint = GetErtText(MV_ERT_SPELL_ROW_HINT, "{~y}우클릭{~}: 이 마법의 상세 정보 보기");
    const int bottom = y + height;
    int rowY = y;

    for (size_t i = 0; i < lines.size() && i < static_cast<size_t>(MV_SPELL_ROW_MAX); ++i)
    {
        const MvTextMetrics m = MvMeasure(NH3Dlg::Text::SMALL, lines[i].c_str(), width, 16);
        const int rowHeight = m.lineHeight * m.lines;
        if (rowY + rowHeight > bottom)
            break; // 영역(286,137 ~ 145x188)을 넘으면 더 그리지 않음

        AppliedSpellRow row;
        row.spellId = refs[i].spellId;
        row.level = refs[i].level;
        row.text = lines[i];
        appliedSpellRows.push_back(row);   // 인덱스 i == 템플릿 ID (500 + i)

        MvAddText(dlg, MV_SPELL_ROW_FIRST + static_cast<int>(i), x, rowY, width, rowHeight,
                  lines[i].c_str(), NH3Dlg::Text::SMALL, eTextAlignment::TOP_LEFT, rowHint.c_str());
        rowY += rowHeight;
    }
}

// 적용된 마법 한 줄을 우클릭했을 때
//   기본: v990(효과 칸 번호)/v991(걸린 수준)을 넣고 ERM 이벤트 OnMvAppliedSpellRmb 호출
//         → ERM이 그 수준으로 설명을 만들어 IF:Q로 띄우고 v992=1
//   예비: v992가 0이면(ERM 쪽이 처리 안 함) 게임에 저장된 현재 설명을 같은 내부 안내창으로 띄움
BOOL CreatureDlgHandler::ShowAppliedSpellPopup(int row)
{
    if (row < 0 || row >= static_cast<int>(appliedSpellRows.size()))
        return FALSE;

    const AppliedSpellRow &entry = appliedSpellRows[row];
    const int spellId = entry.spellId;
    const int level = Clamp(0, entry.level, 3);
    const bool validSpell = spellId >= 0 && spellId < MV_SPELL_SLOT_COUNT;

    // 1) ERM에게 먼저 맡긴다: 걸린 수준으로 설명을 만들어 IF:Q로 띄움
    if (validSpell)
    {
        Era::v[MV_V_SPELL_RMB_ID] = spellId;
        Era::v[MV_V_SPELL_RMB_LEVEL] = level;
        Era::v[MV_V_SPELL_RMB_HANDLED] = 0;
        Era::FireEvent(MV_EVT_SPELL_RMB, nullptr, 0);
        if (Era::v[MV_V_SPELL_RMB_HANDLED])
            return TRUE;
    }

    // 2) ERM이 처리하지 않았을 때(트리거 없음, 영웅 정보 없음 등): 게임에 저장된 현재 설명을 그대로
    //    설명 글 맨 앞의 "{고급 마법 방패}" 제목도 설명 안에 들어 있으므로 따로 붙이지 않는다.
    std::string body;
    if (validSpell)
    {
        const char* desc = H3Spell::Get()[spellId].description[level];
        if (!MvIsEmpty(desc))
            body = desc;
    }
    if (MvIsEmpty(body.c_str()))
        body = GetErtText(MV_ERT_SPELL_NO_DESC, "이 효과에 대한 설명이 없습니다.");

    MvShowSpellRmbBox(body.c_str(), spellId);
    return TRUE;
}

// 커스텀 유닛 정보창 전체 배치 (z11~z23 + C++ 계산값)
void CreatureDlgHandler::AddCustomStats()
{
    if (!dlg || !stack)
        return;

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

    // ---------- 12. 적용된 마법 (z21 표시 + z23 번호:레벨) (ERT 202768번 힌트 연동) ----------
    const char* applied = MvZ(21);
    const int appliedCount = MvCountLines(applied);
    std::string appliedLabel = MvFormatCount(GetErtText(202759, "{~LightBlue}적용된 마법 (%d){~}"), appliedCount);
    std::string appliedHint  = GetErtText(202768, "현재 부대에 영향을 주고 있는 마법 및 지속시간 목록입니다.");

    MvAddText(dlg, 35, 287, 108, 141, 17, appliedLabel.c_str(), NH3Dlg::Text::MEDIUM, MV_TOP_CENTER, appliedHint.c_str());
    // 기존: 통짜 텍스트 1개(템플릿 36) → 변경: 마법 한 줄마다 개별 아이템(템플릿 500+n)
    BuildAppliedSpellList(286, 137, 145, 188, appliedHint);
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
            SetDialogButtonBounds(dlg->GetH3DlgItem(id), x);
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