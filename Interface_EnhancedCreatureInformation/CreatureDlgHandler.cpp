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
                         const char* text, LPCSTR font, eTextAlignment align)
    {
        if (!dlg || MvIsEmpty(text)) return nullptr;
        auto* item = H3DlgText::Create(x, y, w, h, text, font, eTextColor::WHITE,
                                       MV_ID_BASE + templateId, align);
        if (item) dlg->AddItem(item);
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
}

// 커스텀 유닛 정보창 전체 배치 (z11~z21 + C++ 계산값)
void CreatureDlgHandler::AddCustomStats()
{
    if (!dlg || !stack)
        return;

    // ---------- 0. 원본 항목 숨기기: 이름 / 사기 / 행운 / 특수능력 설명 ----------
    MvHide(dlg, DLG_NAME_ID);
    MvHide(dlg, DLG_MORALE_ID);
    MvHide(dlg, DLG_MORALE_TEXT_ID);
    MvHide(dlg, DLG_LUCK_ID);
    MvHide(dlg, DLG_LUCK_TEXT_ID);
    if (auto* desc = FindOriginalDescription())
        desc->HideDeactivate();

    // ---------- 1. 유닛 이름 (z11)  [템플릿 1: 20,23,409,17 가운데] ----------
    MvAddText(dlg, 1, 20, 23, 409, 17, MvZ(11), NH3Dlg::Text::MEDIUM, eTextAlignment::TOP_CENTER);

    // ---------- 2. 상태  [템플릿 111/211: 311,48] ----------
    std::string statusLabel = GetErtText(202755, "{~White}상태{~}");
    MvAddText(dlg, 111, 311, 48, 110, 17, statusLabel.c_str(), NH3Dlg::Text::MEDIUM, eTextAlignment::TOP_LEFT);

    std::string statusText = GetErtText(202714, "{~Cyan}행동 전{~}");
    if (stack->IsWaiting())
        statusText = GetErtText(202715, "{~y}대기중{~}");
    else if (stack->IsDefending())
        statusText = GetErtText(202717, "방어");
    else if (stack->IsDone())
        statusText = GetErtText(202716, "행동 끝");
    MvAddText(dlg, 211, 311, 48, 118, 17, statusText.c_str(), NH3Dlg::Text::SMALL, eTextAlignment::TOP_RIGHT);

    // ---------- 3. 마법횟수  [템플릿 110/210: 311,67] ----------
    std::string spellLabel = GetErtText(202756, "{~White}마법횟수{~}");
    MvAddText(dlg, 110, 311, 67, 110, 17, spellLabel.c_str(), NH3Dlg::Text::MEDIUM, eTextAlignment::TOP_LEFT);

    char spellBuf[16];
    std::snprintf(spellBuf, sizeof(spellBuf), "%d", stack->info.spellCharges);
    MvAddText(dlg, 210, 311, 67, 118, 17, spellBuf, NH3Dlg::Text::SMALL, eTextAlignment::TOP_RIGHT);

    // ---------- 4. 여분발사횟수  [템플릿 104/204: 311,86]  (사격 유닛만) ----------
    if (stack->info.shooter)
    {
        std::string shotLabel = GetErtText(202757, "{~White}여분발사횟수{~}");
        MvAddText(dlg, 104, 311, 86, 110, 17, shotLabel.c_str(), NH3Dlg::Text::MEDIUM, eTextAlignment::TOP_LEFT);

        const int targetIndex = (armySlotIndex >= 0 && armySlotIndex < 42) ? armySlotIndex : 0;
        const int extraShots = Era::v[942 + targetIndex];
        char shotBuf[16];
        std::snprintf(shotBuf, sizeof(shotBuf), "%d", extraShots);
        MvAddText(dlg, 204, 311, 86, 118, 17, shotBuf, NH3Dlg::Text::SMALL, eTextAlignment::TOP_RIGHT);
    }

    // ---------- 5. 원래 수량 (C++ 직접)  [템플릿 30/40: 48,185,113,17] ----------
    std::string origLabel = GetErtText(202760, "{~White}원래 수량:{~}");
    MvAddText(dlg, 30, 48, 185, 113, 17, origLabel.c_str(), NH3Dlg::Text::MEDIUM, eTextAlignment::TOP_LEFT);

    char origBuf[16];
    std::snprintf(origBuf, sizeof(origBuf), "%d", *reinterpret_cast<int*>(reinterpret_cast<char*>(stack) + 0x50));
    MvAddText(dlg, 40, 48, 185, 113, 17, origBuf, NH3Dlg::Text::SMALL, eTextAlignment::TOP_RIGHT);

    // ---------- 6. 반격횟수 (z12)  [템플릿 109/209: 194,185,80,17] ----------
    std::string retLabel = GetErtText(202761, "{~White}반격횟수{~}");
    MvAddText(dlg, 109, 194, 185, 80, 17, retLabel.c_str(), NH3Dlg::Text::MEDIUM, eTextAlignment::TOP_LEFT);
    MvAddText(dlg, 209, 194, 185, 80, 17, MvZ(12), NH3Dlg::Text::SMALL, eTextAlignment::TOP_RIGHT);

    // ---------- 7. 사기 (z16) + 설명 (z17)  [템플릿 212/312] ----------
    MvAddText(dlg, 212, 48, 204, 227, 17, MvZ(16), NH3Dlg::Text::SMALL, eTextAlignment::TOP_LEFT);
    MvAddText(dlg, 312, 108, 206, 167, 17, MvZ(17), NH3Dlg::Text::TINY, eTextAlignment::TOP_LEFT);

    // ---------- 8. 행운 (z18) + 설명 (z19)  [템플릿 213/313] ----------
    MvAddText(dlg, 213, 48, 223, 227, 17, MvZ(18), NH3Dlg::Text::SMALL, eTextAlignment::TOP_LEFT);
    MvAddText(dlg, 313, 108, 225, 167, 17, MvZ(19), NH3Dlg::Text::TINY, eTextAlignment::TOP_LEFT);

    // ---------- 9. 전투력 + 지속시간 힌트 (z13)  [템플릿 31: 20,244,257,30] ----------
    MvAddText(dlg, 31, 20, 244, 257, 30, MvZ(13), NH3Dlg::Text::TINY, eTextAlignment::TOP_LEFT);

    // ---------- 10. 특수능력 라벨 + 설명 (z14)  [템플릿 37/38] ----------
    std::string specLabel = GetErtText(202758, "{~White}특수능력:{~}");
    MvAddText(dlg, 37, 20, 274, 257, 17, specLabel.c_str(), NH3Dlg::Text::TINY, eTextAlignment::TOP_LEFT);
    MvAddText(dlg, 38, 20, 289, 257, 56, MvZ(14), NH3Dlg::Text::TINY, eTextAlignment::TOP_LEFT);

    // ---------- 11. 추가 설명: 방해광선/복제 등 (z20)  [템플릿 41: 20,345,257,30] ----------
    MvAddText(dlg, 41, 20, 345, 257, 30, MvZ(20), NH3Dlg::Text::TINY, eTextAlignment::TOP_LEFT);

    // ---------- 12. 적용된 마법 (z21)  [템플릿 35/36] ----------
    const char* applied = MvZ(21);
    const int appliedCount = MvCountLines(applied);
    std::string appliedLabel = MvFormatCount(GetErtText(202759, "{~LightBlue}적용된 마법 (%d){~}"), appliedCount);
    MvAddText(dlg, 35, 287, 108, 141, 17, appliedLabel.c_str(), NH3Dlg::Text::MEDIUM, eTextAlignment::TOP_CENTER);
    MvAddText(dlg, 36, 286, 137, 145, 188, applied, NH3Dlg::Text::SMALL, eTextAlignment::TOP_LEFT);
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
        hint->SetY(DLG_HEIGHT - hint->GetHeight() - HINT_MARGIN);
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
