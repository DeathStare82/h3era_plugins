// dllmain.cpp : Adventure Info Panel 플러그인 시작점
// 맵 화면 오른쪽 아래에 현재 시간 / 플레이 시간 / VISIT BLOG 버튼을 표시한다.
#include "pch.h"
#include "InfoPanel.h"
#include "PlayTime.h"

Patcher *globalPatcher = nullptr;
PatcherInstance *_PI = nullptr;

namespace
{
constexpr const char *INSTANCE_NAME = "EraPlugin.AdventureInfoPanel.DeathStare";
}

// 게임 초기화가 끝난 뒤 후킹 설치 (Interface_EnhancedCreatureInformation 과 같은 시점)
_LHF_(HooksInit)
{
    infopanel::InfoPanel::Get().CreatePatches();
    return EXEC_DEFAULT;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved)
{
    static bool initialized = false;

    if (reason == DLL_PROCESS_ATTACH && !initialized)
    {
        initialized = true;

        globalPatcher = GetPatcher();
        if (!globalPatcher)
            return FALSE;

        _PI = globalPatcher->CreateInstance(INSTANCE_NAME);
        if (!_PI)
            return FALSE;

        Era::ConnectEra(hModule, INSTANCE_NAME);
        infopanel::playtime::RegisterEvents();
        _PI->WriteLoHook(0x4EEAF2, HooksInit);
    }
    return TRUE;
}
