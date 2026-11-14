#pragma once
#include "pch.h"

// 모험 지도 오른쪽 아래(영웅 패널과 자원 바 사이) 정보 패널.
//  - 현재 시간 / 플레이 시간 / VISIT BLOG 버튼
//  - 패널 전체를 194x105 캔버스(H3LoadedPcx16) 하나에 그리고, 대화상자에는 그 캔버스를 보여주는 항목 1개만 넣는다.
//  - 마우스 오버: 하단 힌트 바에 ERT 202785~202787,  우클릭: 같은 문구를 팝업으로
namespace infopanel
{
// 다시 그려야 한다고 표시 (게임 시작/불러오기 등에서 호출)
void MarkDirty();

// 진단 기록: 게임 폴더의 AdventureInfoPanel.log (+ ERA 로그)
constexpr const char *PLUGIN_VERSION = "0.4";
void DebugLogReset();
void DebugLog(const char *operation, const char *format, ...);

class InfoPanel : public IGamePatch
{
  public:
    static InfoPanel &Get();
    void CreatePatches() override;

  private:
    InfoPanel();

    // ------------------------------------------------------------ 리소스
    bool LoadAssets();
    // ------------------------------------------------------------ 대화상자 항목
    bool HasSpace(h3::H3AdventureMgrDlg *dlg, int panelY) const;
    bool EnsureCreated(h3::H3AdventureMgrDlg *dlg);
    bool IsActiveFor(const h3::H3AdventureMgrDlg *dlg) const;
    // ------------------------------------------------------------ 그리기
    void Render();
    void DrawTime(int index, const char *text, int iconFrame, bool night);
    bool Present(h3::H3AdventureMgrDlg *dlg);
    void DrawItemOnly(h3::H3AdventureMgrDlg *dlg);
    void Update(h3::H3AdventureMgrDlg *dlg, bool force);
    void SetPressed(h3::H3AdventureMgrDlg *dlg, bool pressed);
    // ------------------------------------------------------------ 입력
    int ZoneAt(int x, int y) const;
    const char *HintText(int zone) const;
    void ShowPopup(int zone) const;
    void OpenBlog() const;

    // ------------------------------------------------------------ 후킹
    static int __stdcall AdvMgr_OnShow(LoHook *h, HookContext *c);
    static int __stdcall AdvMgr_Proc(HiHook *h, h3::H3AdventureManager *adv, h3::H3Msg *msg);
    static char __stdcall AdvMgrDlg_MoveHint(HiHook *h, h3::H3AdventureMgrDlg *dlg, int x, int y);
    static void __stdcall AdvMgr_FullUpdate(HiHook *h, h3::H3AdventureMgrDlg *dlg, char redraw);
    static void __stdcall AdvMgr_BeforeScreenRedraw(HiHook *h, h3::H3WindowManager *mgr, int x, int y, int w, int hgt);

    friend void MarkDirty();

  private:
    static InfoPanel *instance;

    // 리소스 (한 번 읽어 두고 계속 사용)
    bool assetsTried = false;
    bool assetsOk = false;
    h3::H3LoadedDef *backgroundDef = nullptr; // DsIpBg.def (우선)
    h3::H3LoadedPcx *background = nullptr;    // DsIpBg.pcx (예비)
    h3::H3LoadedDef *plaqueDef = nullptr;
    h3::H3LoadedDef *iconDef = nullptr;
    h3::H3LoadedDef *digitDef = nullptr;
    h3::H3LoadedDef *buttonDef = nullptr;

    // 패널이 붙어 있는 대화상자 / 캔버스
    h3::H3AdventureMgrDlg *ownerDlg = nullptr;
    h3::H3LoadedPcx16 *canvas = nullptr;
    int panelX = 0;
    int panelY = 0;

    // 표시 상태
    bool dirty = true;
    bool needsPresent = true;   // 캔버스를 화면에 다시 올려야 하는지
    DWORD lastPresentTick = 0;
    bool loggedFirstPresent = false;
    bool loggedNoSpace = false;
    bool loggedShow = false;
    bool loggedNotTop = false;
    bool pressed = false;       // 버튼이 눌린 모양인지
    bool trackingPress = false; // 버튼 위에서 왼쪽 버튼을 누른 상태인지
    DWORD lastCheckTick = 0;
    int shownClock = -1;   // 시*60+분
    int shownPlay = -1;    // 누적 분
    bool shownNight = false;
};
} // namespace infopanel
