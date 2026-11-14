#pragma once
// 맵 화면 오른쪽 아래 정보 패널의 좌표/리소스 상수.
// asset_tools/make_assets.py 의 값과 반드시 같아야 한다 (그림이 이 좌표에 맞춰 만들어져 있음).

namespace infopanel
{
namespace layout
{
// ---------------------------------------------------------------- 패널 전체 (HD 가죽 영역 + 금색 테두리선까지 덮음)
constexpr int PANEL_W = 194;
constexpr int PANEL_H = 105;
constexpr int PANEL_RIGHT_OFFSET = 195;  // x = 게임 너비 - 195   (원본 800x600 상태창 x=605 와 같은 기준)
constexpr int PANEL_BOTTOM_OFFSET = 130; // y = 게임 높이 - 130   (아래 끝 = 높이 - 26, 자원 바 바로 위)
constexpr int MAX_OVERLAP = 3;           // 위쪽 영웅 패널과 겹쳐도 되는 최대 픽셀 (테두리선 정도)

// HD 모드가 영웅/마을 목록을 늘리지 않았을 때(5칸) 상태창의 아래 끝 y, 칸 하나의 높이
constexpr int HD_STATUS_BOTTOM_BASE = 574;
constexpr int HD_LIST_SLOT_H = 32;

// ---------------------------------------------------------------- 시간 명판 (DsIpPlq.def)
constexpr int PLAQUE_X[2] = {8, 99};
constexpr int PLAQUE_Y = 25;
constexpr int PLAQUE_W = 87;
constexpr int PLAQUE_H = 26;
constexpr int PLAQUE_INNER = 3; // 테두리 두께
constexpr int PLAQUE_INNER_W = PLAQUE_W - PLAQUE_INNER * 2;

// ---------------------------------------------------------------- 라벨 (게임 글꼴로 그림)
constexpr int LABEL_Y = 8;
constexpr int LABEL_H = 17;

// ---------------------------------------------------------------- 아이콘 / 숫자 칸 (명판 안쪽 높이 20줄)
constexpr int CELL_H = PLAQUE_H - PLAQUE_INNER * 2; // 20
constexpr int ICON_W = 15;
constexpr int ICON_GAP = 3;
constexpr int DIGIT_W = 9;
constexpr int COLON_W = 5;

// ---------------------------------------------------------------- 버튼 (DsIpBtn.def)
constexpr int BTN_X = 8;
constexpr int BTN_Y = 63;
constexpr int BTN_W = 178;
constexpr int BTN_H = 29;

// ---------------------------------------------------------------- 힌트/우클릭 영역 (라벨 + 명판, 버튼)
struct Zone
{
    int x, y, w, h;
};
constexpr Zone ZONES[3] = {
    {PLAQUE_X[0], LABEL_Y, PLAQUE_W, PLAQUE_Y + PLAQUE_H - LABEL_Y},
    {PLAQUE_X[1], LABEL_Y, PLAQUE_W, PLAQUE_Y + PLAQUE_H - LABEL_Y},
    {BTN_X, BTN_Y, BTN_W, BTN_H},
};
enum eZone : int
{
    ZONE_NONE = -1,
    ZONE_CLOCK = 0,
    ZONE_PLAYTIME = 1,
    ZONE_BUTTON = 2,
};

// ---------------------------------------------------------------- DEF 프레임 번호
namespace frame
{
constexpr int PLAQUE_DAY = 0;
constexpr int PLAQUE_NIGHT = 1;
constexpr int ICON_SUN = 0;
constexpr int ICON_MOON = 1;
constexpr int ICON_HOURGLASS_DAY = 2;
constexpr int ICON_HOURGLASS_NIGHT = 3;
constexpr int DIGIT_GROUP_DAY = 0;
constexpr int DIGIT_GROUP_NIGHT = 1;
constexpr int DIGIT_COLON = 10;
constexpr int BUTTON_NORMAL = 0;
constexpr int BUTTON_PRESSED = 1;
} // namespace frame

// 06:00 ~ 17:59 = 낮
constexpr int DAY_START_HOUR = 6;
constexpr int NIGHT_START_HOUR = 18;
} // namespace layout

namespace res
{
constexpr const char *BACKGROUND_DEF = "DsIpBg.def"; // 배경 (우선 사용)
constexpr const char *BACKGROUND_PCX = "DsIpBg.pcx"; // 배경 (DEF 가 없을 때 예비)
constexpr const char *PLAQUE_DEF = "DsIpPlq.def";
constexpr const char *ICON_DEF = "DsIpIco.def";
constexpr const char *DIGIT_DEF = "DsIpDgt.def";
constexpr const char *BUTTON_DEF = "DsIpBtn.def";
} // namespace res

namespace ids
{
// 모험 지도 대화상자 안에서 다른 항목과 겹치지 않을 번호
constexpr int CANVAS_ITEM_ID = 27180;
constexpr int HINT_ZONE_ID_BASE = 27181; // 27181~27183: 힌트 캐시용 가상 번호 (실제 항목 아님)
} // namespace ids
} // namespace infopanel
