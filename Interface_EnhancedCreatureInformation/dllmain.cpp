// dllmain.cpp : 플러그인 시작점 (진단 로그 포함 버전)
#include "pch.h"
#include "CreatureDlgHooks.h"
#include "PluginSettings.h"

Patcher *globalPatcher = nullptr;
PatcherInstance *_PI = nullptr;

using namespace h3;

// ===== 진단용: 게임 폴더\Debug\plugin_trace.txt 에 한 줄씩 메모 남기기 =====
// (C++ 표준 라이브러리를 쓰지 않고 윈도우 기본 기능만 사용 -> 가장 안전)
static void Trace(const char *msg)
{
    HANDLE h = CreateFileA("Debug\\plugin_trace.txt", FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    WriteFile(h, msg, (DWORD)lstrlenA(msg), &written, nullptr);
    WriteFile(h, "\r\n", 2, &written, nullptr);
    CloseHandle(h);
}

static void TraceExport(HMODULE hEra, const char *name)
{
    char buf[128];
    FARPROC p = hEra ? GetProcAddress(hEra, name) : nullptr;
    lstrcpyA(buf, p ? "    OK       " : "    MISSING  ");
    lstrcatA(buf, name);
    Trace(buf);
}

_LHF_(HooksInit)
{
    Trace("[7] HooksInit start (game init reached)");
    preview::MonPreview::Get();
    Trace("[8] MonPreview OK");
    Dlg_CreatureInfo_HooksInit(_PI);
    Trace("[9] CreatureInfo hooks OK");
    Dlg_CreatureSpellInfo_HooksInit(_PI);
    Trace("[10] CreatureSpellInfo hooks OK");
    creatureInfo::RegisterPluginSettingsButton();
    Trace("[11] HooksInit end");
    return EXEC_DEFAULT;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    static _bool_ plugin_On = 0;

    if (ul_reason_for_call == DLL_PROCESS_ATTACH && !plugin_On)
    {
        plugin_On = 1;
        Trace("==== [1] DllMain start ====");

        HMODULE hEra = GetModuleHandleA("era.dll");
        Trace(hEra ? "[2] era.dll is in process" : "[2] era.dll NOT in process");
        const char *names[] = {"CreatePlugin", "FatalError", "Hook", "Splice", "WriteAtCode",
                               "RegisterMemoryConsumer", "_ClientMemAlloc", "_ClientMemFree",
                               "_ClientMemRealloc", "LoadImageAsPcx16", "PcxPngExists",
                               "trStatic", "trTemp", "GetVersionNum", "SetMultiPurposeDlgHandler", nullptr};
        for (int i = 0; names[i]; i++)
            TraceExport(hEra, names[i]);

        globalPatcher = GetPatcher();
        Trace(globalPatcher ? "[3] GetPatcher OK" : "[3] GetPatcher FAILED");
        if (!globalPatcher)
            return FALSE;

        _PI = globalPatcher->CreateInstance("EraPlugins.CreatureInformation.daemon_n");
        Trace(_PI ? "[4] CreateInstance OK" : "[4] CreateInstance FAILED (same name already registered?)");
        if (!_PI)
            return FALSE;

        Era::ConnectEra(hModule, "EraPlugins.CreatureInformation.daemon_n");
        Trace("[5] ConnectEra OK");

        _PI->WriteLoHook(0x4EEAF2, HooksInit);
        Trace("[6] WriteLoHook OK - DllMain end");
    }
    return TRUE;
}
