// Battle hex shadows for ERM: !!FU(BattleShadow_Radius), !!FU(BattleShadow_Hexes), !!FU(BattleShadow_Clear).
#include "framework.h"
#include "BattleShadows.h"

Patcher *globalPatcher = nullptr;
PatcherInstance *_PI = nullptr;

namespace dllText
{
LPCSTR instanceName = "EraPlugin." PROJECT_NAME ".daemon_n";
}

static _ERH_(OnAfterWog)
{
    battle_shadows::InstallHooks(_PI);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    static bool initialized = false;
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        if (!initialized)
        {
            initialized = true;
            globalPatcher = GetPatcher();
            _PI = globalPatcher->CreateInstance(dllText::instanceName);
            Era::ConnectEra(hModule, dllText::instanceName);
            _REH_(OnAfterWog);
            battle_shadows::RegisterEvents();
        }
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
