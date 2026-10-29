#include "pch.h"

#include "CreatureDlgHandler.h"
#include "CreatureDlgHooks.h"
#include "CreatureDlgLayout.h"
#include "PluginSettings.h"
#include "CreatureSpellEffects.h"

#include <algorithm>
#include <string>
#include <cstdio>

using namespace h3;
using namespace creatureInfo;

void CreatureDlgHandler::HideDefaultBattleSpellItems()
{
    if (!dlg)
        return;

    for (int id = DLG_NATIVE_SPELL_FIRST_ID; id <= DLG_NATIVE_SPELL_LAST_ID; ++id)
        if (auto *item = dlg->GetH3DlgItem(id))
            item->HideDeactivate();
}

// [수정 완료] 상태, 마법횟수, 여분발사횟수를 대화창에 생성 및 연동하는 멤버 함수
void CreatureDlgHandler::AddCustomStats()
{
    if (!dlg || !stack)
        return;

    // 1. 상태 (Status) - Y: 48
    auto *statusLabel = H3DlgText::Create(311, 48, 110, 17, Era::tr("eci.combat_dialog.custom.status_label"), NH3Dlg::Text::MEDIUM, eTextColor::WHITE, 111, eTextAlignment::TOP_LEFT);
    if (statusLabel) dlg->AddItem(statusLabel);

    Era::static_str statusStr = Era::tr("eci.combat_dialog.status.active");
    if (stack->IsWaiting()) statusStr = Era::tr("eci.combat_dialog.status.wait");
    else if (stack->IsDefending()) statusStr = Era::tr("eci.combat_dialog.status.defend");
    else if (stack->IsDone()) statusStr = Era::tr("eci.combat_dialog.status.done");

    auto *statusVal = H3DlgText::Create(311, 48, 118, 17, statusStr, NH3Dlg::Text::SMALL, eTextColor::WHITE, 211, eTextAlignment::TOP_RIGHT);
    if (statusVal) dlg->AddItem(statusVal);

    // 2. 마법횟수 (Spell Charges) - Y: 67
    auto *spellLabel = H3DlgText::Create(311, 67, 110, 17, Era::tr("eci.combat_dialog.custom.spell_label"), NH3Dlg::Text::MEDIUM, eTextColor::WHITE, 110, eTextAlignment::TOP_LEFT);
    if (spellLabel) dlg->AddItem(spellLabel);

    char spellBuf[16];
    std::snprintf(spellBuf, sizeof(spellBuf), "%d", stack->info.spellCharges);
    auto *spellVal = H3DlgText::Create(311, 67, 118, 17, spellBuf, NH3Dlg::Text::SMALL, eTextColor::WHITE, 210, eTextAlignment::TOP_RIGHT);
    if (spellVal) dlg->AddItem(spellVal);

    // 3. 여분발사횟수 (Extra Shots) - Y: 86 (사격 유닛인 경우만 표시)
    if (stack->info.shooter)
    {
        auto *shotLabel = H3DlgText::Create(311, 86, 110, 17, Era::tr("eci.combat_dialog.custom.shot_label"), NH3Dlg::Text::MEDIUM, eTextColor::WHITE, 104, eTextAlignment::TOP_LEFT);
        if (shotLabel) dlg->AddItem(shotLabel);

        char shotBuf[16];
        std::snprintf(shotBuf, sizeof(shotBuf), "%d", stack->info.numberShots);
        auto *shotVal = H3DlgText::Create(311, 86, 118, 17, shotBuf, NH3Dlg::Text::SMALL, eTextColor::WHITE, 204, eTextAlignment::TOP_RIGHT);
        if (shotVal) dlg->AddItem(shotVal);
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
    
    // 대화창에 커스텀 통계(상태, 마법횟수, 여분발사횟수) 추가 호출
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
    const int availableHeight = std::max(descriptionHeight, CONTENT_BOTTOM - descriptionY);

    if (description)
    {
        description->SetWidth(descriptionWidth);
        description->SetHeight(availableHeight);
        description->SetX(descriptionX);
        description->SetY(descriptionY);
        description->ShowActivate();
    }
    else if (!descriptionText.empty())
    {
        description =
            H3DlgText::Create(descriptionX, descriptionY, descriptionWidth, availableHeight, descriptionText.c_str(),
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