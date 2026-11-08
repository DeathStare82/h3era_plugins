#pragma once
#include "pch.h"
#include "CreatureDlgLayout.h"

#include <string>
#include <vector>

class CreatureDlgHandler
{
    struct SkillIcon
    {
        const char *defName = nullptr;
        int frame = -1;
        const char *text = nullptr;
        const char *hint = nullptr;
        H3LoadedPcx16 *pcx16 = nullptr;
    };

    struct ScrolledItem
    {
        H3DlgItem *item = nullptr;
        int baseY = 0;
    };

    // [Multiverse] 적용된 마법 목록의 한 줄 (z21의 n번째 줄 ↔ z23의 n번째 "효과칸:마법번호:레벨")
    struct AppliedSpellRow
    {
        int spellId = -1;     // ERM 루프의 r (BM:G 효과 칸 번호)
        int spellCode = -1;   // Fix_Code_for_AppliedSpell 결과 s (목록 이름·설명·그림 기준, 81 이상은 스크롤 마법)
        int level = 0;        // 0=기본, 1=초급, 2=중급, 3=고급
        std::string text;     // z21의 해당 줄 그대로 (색상 태그 포함)
    };

    H3CreatureInfoDlg *dlg = nullptr;
    H3CombatCreature *stack = nullptr;
    H3Army *army = nullptr;
    int armySlotIndex = -1;
    bool wogStackExperience = false;
    const H3Hero *hero = nullptr;
    int playerColor = -1;

    int descriptionX = creatureInfo::DESCRIPTION_DEFAULT_X;
    int descriptionY = creatureInfo::DESCRIPTION_DEFAULT_Y;
    int descriptionWidth = creatureInfo::DESCRIPTION_DEFAULT_WIDTH;
    int descriptionHeight = creatureInfo::DESCRIPTION_DEFAULT_HEIGHT;
    int commanderPanelHeight = 0;

    int viewportX = 0;
    int viewportY = 0;
    int viewportWidth = 0;
    int viewportHeight = 0;
    int maxScrollTick = 0;
    int currentScrollTick = 0;

    // Pixel offset for every logical row. A skill-icon row and a text row
    // both consume exactly one scrollbar tick.
    std::vector<int> scrollRowOffsets;
    std::vector<SkillIcon> creatureSkills;
    std::vector<ScrolledItem> scrolledItems;
    std::vector<H3DlgPcx16 *> skillItems;
    std::vector<H3DlgText *> descriptionLineItems;
    H3DlgScrollbar *skillsScrollbar = nullptr;
    std::vector<char> skillPopupBuffer;

    // [Multiverse] 행 번호 = (아이템 ID - 30000) - 500
    std::vector<AppliedSpellRow> appliedSpellRows;

    void BuildAppliedSpellList(int x, int y, int width, int height, const std::string &fallbackHint);
    BOOL ShowAppliedSpellPopup(int row);

    // [Multiverse] 비전투 창(영웅·마을 부대, 고용창)에 정보를 채운다. AddCustomStats()가 부른다.
    void AddNonBattleStats();

    void ApplyDialogAppearance();
    void HideDefaultBattleSpellItems();
    H3DlgText *FindOriginalDescription() const;
    std::string GetDescriptionText(H3DlgText *description) const;
    BOOL BuildDescriptionArea();
    BOOL BuildExperienceSkillsPanel(H3DlgText *description, const std::string &descriptionText);
    void ReleaseUnownedSkillImages();
    void ResetExperienceSkillsPanel();

  public:
    // [Multiverse] 비전투 창에서 해고 버튼(과 그 테두리)을 오른쪽으로 옮기는 거리(픽셀).
    //   버튼 위치를 바꾸고 싶으면 이 숫자만 바꾸면 된다. (크게 = 오른쪽, 작게 = 왼쪽, 0 = 원래 자리)
    static constexpr int NONBATTLE_DISMISS_SHIFT_X = 104;

    CreatureDlgHandler(H3CreatureInfoDlg *dlg, H3CombatCreature *stack = nullptr, H3Army *army = nullptr,
                       int armySlotIndex = -1, const H3Hero *hero = nullptr, int playerColor = -1);
    ~CreatureDlgHandler();

    void AddCustomStats(); // CreatureDlgHandler.h 내부의 class CreatureDlgHandler 안에 추가
    void ScrollTo(int scrollTick);
    void RefreshCreatureArtifact();
    BOOL AlignItems();
    BOOL AddExperienceButton();
    BOOL AddSpellEffects();
    BOOL AddCommanderSkills();
    BOOL CreateCreatureSkillsList();
    BOOL HandleMsg(H3Msg* msg); // 우클릭 팝업 처리를 위한 메서드 선언
};