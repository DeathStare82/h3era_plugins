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

    std::ifstream file("Data/s/text.ert");
    if (!file.is_open())
    {
        file.open("text.ert");
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

// 상태, 마법횟수, 여분발사횟수 및 ERM 연동 전투력/지속시간 추가
void CreatureDlgHandler::AddCustomStats()
{
    if (!dlg || !stack)
        return;

    // 1. 상태 (ERT 202755 / 202714~202717)
    std::string statusLabelText = GetErtText(202755, "{~White}상태{~}");
    auto *statusLabel = H3DlgText::Create(311, 48, 110, 17, statusLabelText.c_str(), NH3Dlg::Text::MEDIUM, eTextColor::WHITE, 111, eTextAlignment::TOP_LEFT);
    if (statusLabel) dlg->AddItem(statusLabel);

    std::string statusText = GetErtText(202714, "행동 전");
    if (stack->IsWaiting())
        statusText = GetErtText(202715, "대기중");
    else if (stack->IsDefending())
        statusText = GetErtText(202717, "행동 끝 (방어)");
    else if (stack->IsDone())
        statusText = GetErtText(202716, "행동 끝");

    auto *statusVal = H3DlgText::Create(311, 48, 118, 17, statusText.c_str(), NH3Dlg::Text::SMALL, eTextColor::WHITE, 211, eTextAlignment::TOP_RIGHT);
    if (statusVal) dlg->AddItem(statusVal);

    // 2. 마법횟수 (ERT 202756)
    std::string spellLabelText = GetErtText(202756, "{~White}마법횟수{~}");
    auto *spellLabel = H3DlgText::Create(311, 67, 110, 17, spellLabelText.c_str(), NH3Dlg::Text::MEDIUM, eTextColor::WHITE, 110, eTextAlignment::TOP_LEFT);
    if (spellLabel) dlg->AddItem(spellLabel);

    char spellBuf[16];
    std::snprintf(spellBuf, sizeof(spellBuf), "%d", stack->info.spellCharges);
    auto *spellVal = H3DlgText::Create(311, 67, 118, 17, spellBuf, NH3Dlg::Text::SMALL, eTextColor::WHITE, 210, eTextAlignment::TOP_RIGHT);
    if (spellVal) dlg->AddItem(spellVal);

    // 3. 여분발사횟수 (ERT 202757 / ERM v변수 연동)
    if (stack->info.shooter)
    {
        std::string shotLabelText = GetErtText(202757, "{~White}여분발사횟수{~}");
        auto *shotLabel = H3DlgText::Create(311, 86, 110, 17, shotLabelText.c_str(), NH3Dlg::Text::MEDIUM, eTextColor::WHITE, 104, eTextAlignment::TOP_LEFT);
        if (shotLabel) dlg->AddItem(shotLabel);

        // stack 객체 고유 속성(예: stack->GetStackIndex() 등) 또는 인덱스 활용
        int extraShots = 0;
        int stackIndex = armySlotIndex; // 생성자에 전달된 slot index 활용 예시
        if (stackIndex >= 0 && stackIndex < 42)
        {
            extraShots = Era::v[942 + stackIndex];
        }

        char shotBuf[16];
        std::snprintf(shotBuf, sizeof(shotBuf), "%d", extraShots);
        auto *shotVal = H3DlgText::Create(311, 86, 118, 17, shotBuf, NH3Dlg::Text::SMALL, eTextColor::WHITE, 204, eTextAlignment::TOP_RIGHT);
        if (shotVal) dlg->AddItem(shotVal);
    }

    // 4. 전투력 및 보조마법 지속시간 문구 (ERM z 변수 직접 연결)
    const char* fightText = Era::z[5]; 
    if (fightText && *fightText)
    {
        auto *fightVal = H3DlgText::Create(17, 208, 280, 28, fightText, NH3Dlg::Text::TINY, eTextColor::WHITE, 300, eTextAlignment::TOP_LEFT);
        if (fightVal) dlg->AddItem(fightVal);
    }
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
        AddSpellEffects();
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
    ResetExperienceSkillsPanel();
    H3DlgText *description = FindOriginalDescription();
    const std::string descriptionText = GetDescriptionText(description);
    if (description)
        description->HideDeactivate();

    if (GetPluginSettings().showCreatureSkills && !Era::IsCommanderId(dlg->creatureId) && CreateCreatureSkillsList() &&
        !creatureSkills.empty())
    {
        if (BuildExperienceSkillsPanel(description, descriptionText))
            return TRUE;
    }

    ReleaseUnownedSkillImages();
    
    const int customDescY = 255;
    const int availableHeight = std::max(100, CONTENT_BOTTOM - customDescY);

    // '특수능력:' 라벨 추가 (ERT 202758 사용)
    std::string specTitle = GetErtText(202758, "{~White}특수능력:{~}");
    auto *specLabel = H3DlgText::Create(descriptionX, customDescY - 15, descriptionWidth, 15, 
                                        specTitle.c_str(), NH3Dlg::Text::SMALL, eTextColor::WHITE, 301, eTextAlignment::TOP_LEFT);
    if (specLabel) dlg->AddItem(specLabel);

    if (description)
    {
        description->SetWidth(descriptionWidth);
        description->SetHeight(availableHeight);
        description->SetX(descriptionX);
        description->SetY(customDescY);
        description->ShowActivate();
    }
    else if (!descriptionText.empty())
    {
        description =
            H3DlgText::Create(descriptionX, customDescY, descriptionWidth, availableHeight, descriptionText.c_str(),
                              NH3Dlg::Text::TINY, eTextColor::WHITE, DLG_DESCRIPTION_ID, eTextAlignment::TOP_LEFT);
        if (description)
            dlg->AddItem(description);
    }
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