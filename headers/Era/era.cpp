#include "era.h"
#include <cstdlib>

namespace Era
{

/* WoG vars */
int *v = (int *)0x887664;           // 1..10000
TErmZVar *z = (TErmZVar *)0x9271E8; // 1..1000
int *y = (int *)0xA48D7C;           // 1..100
int *x = (int *)0x91DA34;           // 1..16
bool *f = (bool *)0x91F2DF;         // 1..1000
float *e = (float *)0xA48F14;       // 1..100

HINSTANCE hEra = LoadLibraryA("era.dll");
HINSTANCE hPlugin = 0;
TPlugin plugin = nullptr;

#define ERA_IMPORT(funcName)                                                                                           \
    decltype(funcName) funcName = reinterpret_cast<decltype(funcName)>(GetProcAddress(hEra, #funcName))
#define ERA_IMPORT_RENAMED(origName, newName)                                                                          \
    decltype(newName) newName = reinterpret_cast<decltype(newName)>(GetProcAddress(hEra, #origName))

ERA_IMPORT(AllocErmFunc);
ERA_IMPORT(ClearAllIniCache);
ERA_IMPORT(ClearIniCache);
ERA_IMPORT(CreatePlugin);
ERA_IMPORT(DecorateInt);
ERA_IMPORT(EmptyIniCache);
ERA_IMPORT(ExecErmCmd);
ERA_IMPORT(ExecPersistedErmCmd);
ERA_IMPORT(ExtractErm);
ERA_IMPORT(FastQuitToGameMenu);
ERA_IMPORT(FatalError);
ERA_IMPORT(FindNextObject);
ERA_IMPORT(FireErmEvent);
ERA_IMPORT(FireEvent);
ERA_IMPORT(FormatQuantity);
ERA_IMPORT(FreeAppliedPatch);
ERA_IMPORT(GenerateDebugInfo);
ERA_IMPORT(GetAppliedPatchSize);
ERA_IMPORT(GetArgXVars);
ERA_IMPORT(GetAssocVarIntValue);
ERA_IMPORT(GetAssocVarStrValue);
ERA_IMPORT(GetButtonID);
ERA_IMPORT(GetCampaignFileName);
ERA_IMPORT(GetCampaignMapInd);
ERA_IMPORT(GetCodePage);
ERA_IMPORT(GetEraRegistryIntValue);
ERA_IMPORT(GetEraRegistryStrValue);
ERA_IMPORT(GetGameState);
ERA_IMPORT(GetLanguage);
ERA_IMPORT(GetMapFileName);
ERA_IMPORT(GetProcessGuid);
ERA_IMPORT(GetRealAddr);
ERA_IMPORT(GetRetXVars);
ERA_IMPORT(GetTriggerReadableName);
ERA_IMPORT(GetVersionNum);
ERA_IMPORT(GlobalRedirectFile);
ERA_IMPORT(Hash32);
ERA_IMPORT(IsCampaign);
ERA_IMPORT(IsCommanderId);
ERA_IMPORT(IsPatchOverwritten);
ERA_IMPORT(LoadImageAsPcx16);
ERA_IMPORT(LogMemoryState);
ERA_IMPORT(MemFree);
ERA_IMPORT(MergeIniWithDefault);
ERA_IMPORT(NameColor);
ERA_IMPORT(NameTrigger);
ERA_IMPORT(NotifyError);
ERA_IMPORT(PatchExists);
ERA_IMPORT(PcxPngExists);
ERA_IMPORT(PersistErmCmd);
ERA_IMPORT(PluginExists);
ERA_IMPORT(RandomRangeWithFreeParam);
ERA_IMPORT(ReadSavegameSection);
ERA_IMPORT(ReadStrFromIni);
ERA_IMPORT(RedirectFile);
ERA_IMPORT(RedirectMemoryBlock);
ERA_IMPORT(RegisterHandler);
ERA_IMPORT(RegisterMemoryConsumer);
ERA_IMPORT(ReloadErm);
ERA_IMPORT(ReloadLanguageData);
ERA_IMPORT(ReportPluginVersion);
ERA_IMPORT(RestartCurrentProcess);
ERA_IMPORT(RollbackAppliedPatch);
ERA_IMPORT(SaveIni);
ERA_IMPORT(SetAssocVarIntValue);
ERA_IMPORT(SetAssocVarStrValue);
ERA_IMPORT(SetCodePage);
ERA_IMPORT(SetEraRegistryIntValue);
ERA_IMPORT(SetEraRegistryStrValue);
ERA_IMPORT(SetIsCommanderIdFunc);
ERA_IMPORT(SetIsElixirOfLifeStackFunc);
ERA_IMPORT(SetLanguage);
ERA_IMPORT(SetMultiPurposeDlgHandler);
ERA_IMPORT(SetRegenerationAbility);
ERA_IMPORT(SetStdRegenerationEffect);
ERA_IMPORT(ShowErmError);
ERA_IMPORT(ShowMessage);
ERA_IMPORT(ShowMultiPurposeDlg);
ERA_IMPORT(SplitMix32);
ERA_IMPORT(ToStaticStr);
ERA_IMPORT(trStatic);
ERA_IMPORT(trTemp);
ERA_IMPORT(WriteLog);
ERA_IMPORT(WriteSavegameSection);
ERA_IMPORT(WriteStrToIni);
ERA_IMPORT_RENAMED(GetVersion, GetEraVersion);
ERA_IMPORT_RENAMED(Hook, _Hook);
ERA_IMPORT_RENAMED(Splice, _Splice);
ERA_IMPORT_RENAMED(WriteAtCode, _WriteAtCode);
ERA_IMPORT_RENAMED(tr, _tr);

// =====================================================================================
// [호환 계층] ERA 3.9.3 처럼 CreatePlugin 이 없는 구버전 ERA 에서도 동작하게 하는 장치
//  - 없는 기능은 "아무 일도 하지 않는 안전한 대역 함수"로 바꿔 끼움 (빈 전화번호 호출 방지)
//  - Hook / Splice / WriteAtCode 는 구버전 형식(플러그인 인자 없음)으로 호출
//  - trStatic / trTemp 는 구버전의 tr 로 대체
// =====================================================================================
namespace Compat
{

bool isLegacyEra = false; // true = CreatePlugin 이 없는 구버전 ERA

// 구버전 ERA 의 함수 형식
typedef void *(__stdcall *TLegacyHook)(void *Addr, THookHandler HandlerFunc, void **AppliedPatch, int MinCodeSize,
                                       THookType HookType);
typedef void *(__stdcall *TLegacySplice)(void *OrigFunc, void *HandlerFunc, int CallingConv, int NumArgs,
                                         int *CustomParam, void **AppliedPatch);
typedef bool(__stdcall *TLegacyWriteAtCode)(int Count, void *Src, void *Dst);
typedef char *(__stdcall *TLegacyTr)(const char *key, const char **params, int high);

TLegacyTr legacyTr = nullptr;

// 없는 기능을 대신할 "대역 함수" 자동 생성기
template <typename T> struct Stub
{
    static void __stdcall Fn()
    {
    }
};
template <typename R, typename... A> struct Stub<R(__stdcall *)(A...)>
{
    static R __stdcall Fn(A...)
    {
        return R();
    }
};
template <typename... A> struct Stub<void(__stdcall *)(A...)>
{
    static void __stdcall Fn(A...)
    {
    }
};
template <typename... A> struct Stub<const char *(__stdcall *)(A...)>
{
    static const char *__stdcall Fn(A...)
    {
        return "";
    }
};
template <typename... A> struct Stub<char *(__stdcall *)(A...)>
{
    static char *__stdcall Fn(A...)
    {
        static char empty[1] = {0};
        return empty;
    }
};

template <typename T> void FillIfMissing(T &ptr)
{
    if (!ptr)
    {
        ptr = reinterpret_cast<T>(&Stub<T>::Fn);
    }
}

// 번역 기능 대역: 구버전 tr 이 있으면 그것을 쓰고, 없으면 키 문자열을 그대로 돌려줌
const char *__stdcall Compat_trStatic(const char *key)
{
    if (legacyTr)
    {
        return legacyTr(key, nullptr, -1);
    }

    return key ? key : "";
}

char *__stdcall Compat_trTemp(const char *key, const char **params, int high)
{
    if (legacyTr)
    {
        return legacyTr(key, params, high);
    }

    static char buf[1024];
    lstrcpynA(buf, key ? key : "", sizeof(buf));

    return buf;
}

void Init()
{
    static bool done = false;

    if (done)
    {
        return;
    }

    done = true;
    isLegacyEra = (CreatePlugin == nullptr);

    if (hEra)
    {
        legacyTr = reinterpret_cast<TLegacyTr>(GetProcAddress(hEra, "tr"));
    }

    if (!trStatic)
    {
        trStatic = reinterpret_cast<decltype(trStatic)>(&Compat_trStatic);
    }

    if (!trTemp)
    {
        trTemp = reinterpret_cast<decltype(trTemp)>(&Compat_trTemp);
    }

    FillIfMissing(AllocErmFunc);
    FillIfMissing(ClearAllIniCache);
    FillIfMissing(ClearIniCache);
    FillIfMissing(DecorateInt);
    FillIfMissing(EmptyIniCache);
    FillIfMissing(ExecErmCmd);
    FillIfMissing(ExecPersistedErmCmd);
    FillIfMissing(ExtractErm);
    FillIfMissing(FastQuitToGameMenu);
    FillIfMissing(FatalError);
    FillIfMissing(FindNextObject);
    FillIfMissing(FireErmEvent);
    FillIfMissing(FireEvent);
    FillIfMissing(FormatQuantity);
    FillIfMissing(FreeAppliedPatch);
    FillIfMissing(GenerateDebugInfo);
    FillIfMissing(GetAppliedPatchSize);
    FillIfMissing(GetArgXVars);
    FillIfMissing(GetAssocVarIntValue);
    FillIfMissing(GetAssocVarStrValue);
    FillIfMissing(GetButtonID);
    FillIfMissing(GetCampaignFileName);
    FillIfMissing(GetCampaignMapInd);
    FillIfMissing(GetCodePage);
    FillIfMissing(GetEraRegistryIntValue);
    FillIfMissing(GetEraRegistryStrValue);
    FillIfMissing(GetGameState);
    FillIfMissing(GetLanguage);
    FillIfMissing(GetMapFileName);
    FillIfMissing(GetProcessGuid);
    FillIfMissing(GetRealAddr);
    FillIfMissing(GetRetXVars);
    FillIfMissing(GetTriggerReadableName);
    FillIfMissing(GetVersionNum);
    FillIfMissing(GlobalRedirectFile);
    FillIfMissing(Hash32);
    FillIfMissing(IsCampaign);
    FillIfMissing(IsCommanderId);
    FillIfMissing(IsPatchOverwritten);
    FillIfMissing(LoadImageAsPcx16);
    FillIfMissing(LogMemoryState);
    FillIfMissing(MemFree);
    FillIfMissing(MergeIniWithDefault);
    FillIfMissing(NameColor);
    FillIfMissing(NameTrigger);
    FillIfMissing(NotifyError);
    FillIfMissing(PatchExists);
    FillIfMissing(PcxPngExists);
    FillIfMissing(PersistErmCmd);
    FillIfMissing(PluginExists);
    FillIfMissing(RandomRangeWithFreeParam);
    FillIfMissing(ReadSavegameSection);
    FillIfMissing(ReadStrFromIni);
    FillIfMissing(RedirectFile);
    FillIfMissing(RedirectMemoryBlock);
    FillIfMissing(RegisterHandler);
    FillIfMissing(RegisterMemoryConsumer);
    FillIfMissing(ReloadErm);
    FillIfMissing(ReloadLanguageData);
    FillIfMissing(ReportPluginVersion);
    FillIfMissing(RestartCurrentProcess);
    FillIfMissing(RollbackAppliedPatch);
    FillIfMissing(SaveIni);
    FillIfMissing(SetAssocVarIntValue);
    FillIfMissing(SetAssocVarStrValue);
    FillIfMissing(SetCodePage);
    FillIfMissing(SetEraRegistryIntValue);
    FillIfMissing(SetEraRegistryStrValue);
    FillIfMissing(SetIsCommanderIdFunc);
    FillIfMissing(SetIsElixirOfLifeStackFunc);
    FillIfMissing(SetLanguage);
    FillIfMissing(SetMultiPurposeDlgHandler);
    FillIfMissing(SetRegenerationAbility);
    FillIfMissing(SetStdRegenerationEffect);
    FillIfMissing(ShowErmError);
    FillIfMissing(ShowMessage);
    FillIfMissing(ShowMultiPurposeDlg);
    FillIfMissing(SplitMix32);
    FillIfMissing(ToStaticStr);
    FillIfMissing(WriteLog);
    FillIfMissing(WriteSavegameSection);
    FillIfMissing(WriteStrToIni);
    FillIfMissing(GetEraVersion);
    FillIfMissing(_Hook);
    FillIfMissing(_Splice);
    FillIfMissing(_WriteAtCode);
    FillIfMissing(_tr);
}

} // namespace Compat

bool IsLegacyEra()
{
    Compat::Init();
    return Compat::isLegacyEra;
}

std::string GetModuleFileName(HINSTANCE hInstance)
{
    char buf[256];

    buf[0] = 0;
    size_t filePathLen = GetModuleFileNameA(hInstance, buf, sizeof(buf));

    if (filePathLen > 0)
    {
        int i = filePathLen - 1;

        while (i && buf[i] != '\\')
        {
            i--;
        }

        if (buf[i] == '\\')
        {
            i++;
        }

        return (char *)&buf[i];
    }

    return buf;
}

void ConnectEra(HINSTANCE PluginDllHandle, const char *PluginName)
{
    Compat::Init();

    std::string finalPluginName = GetModuleFileName(PluginDllHandle);

    if (PluginName && *PluginName)
    {
        finalPluginName = PluginName;
    }

    if (Compat::isLegacyEra)
    {
        // 구버전 ERA: 플러그인 등록 기능 자체가 없으므로 등록 단계를 건너뜀
        plugin = nullptr;
        return;
    }

    plugin = CreatePlugin(finalPluginName.c_str());

    if (!plugin)
    {
        FatalError((std::string("Duplicate registered plugin: ") + GetModuleFileName(PluginDllHandle)).c_str());
    }
}

void *Hook(void *Addr, THookHandler HandlerFunc, void **AppliedPatch, int MinCodeSize, THookType HookType)
{
    Compat::Init();

    if (Compat::isLegacyEra)
    {
        return reinterpret_cast<Compat::TLegacyHook>(_Hook)(Addr, HandlerFunc, AppliedPatch, MinCodeSize, HookType);
    }

    return _Hook(plugin, Addr, HandlerFunc, AppliedPatch, MinCodeSize, HookType);
}

void *Splice(void *OrigFunc, void *HandlerFunc, int CallingConv, int NumArgs, int *CustomParam, void **AppliedPatch)
{
    Compat::Init();

    if (Compat::isLegacyEra)
    {
        return reinterpret_cast<Compat::TLegacySplice>(_Splice)(OrigFunc, HandlerFunc, CallingConv, NumArgs,
                                                               CustomParam, AppliedPatch);
    }

    return _Splice(plugin, OrigFunc, HandlerFunc, CallingConv, NumArgs, CustomParam, AppliedPatch);
}

bool WriteAtCode(int Count, void *Src, void *Dst)
{
    Compat::Init();

    if (Compat::isLegacyEra)
    {
        return reinterpret_cast<Compat::TLegacyWriteAtCode>(_WriteAtCode)(Count, Src, Dst);
    }

    return _WriteAtCode(plugin, Count, Src, Dst);
}

static_str tr(const char *key)
{
    Compat::Init();
    return trStatic(key);
}

std::string tr(const char *key, const std::vector<std::string> &params)
{
    Compat::Init();

    constexpr int MAX_PARAMS = 64;

    const char *_params[MAX_PARAMS]{};
    const int numParams = (params.size() <= MAX_PARAMS ? params.size() : MAX_PARAMS) & ~1;

    for (int i = 0; i < numParams; i++)
    {
        _params[i] = params[i].c_str();
    }

    return trTemp(key, _params, numParams - 1);
}

std::string IntToStr(int value)
{
    char buf[16];
    sprintf_s(buf, sizeof(buf), "%d", value);

    return buf;
}

void SetPcharValue(char *Buf, const char *NewValue, int BufSize)
{
    if (BufSize < 0)
    {
        lstrcpyA(Buf, NewValue);
    }
    else if (BufSize > 0)
    {
        int NumBytesToCopy = lstrlenA(NewValue);

        if (NumBytesToCopy >= BufSize)
        {
            NumBytesToCopy = BufSize - 1;
        }

        memcpy(Buf, NewValue, NumBytesToCopy);
        Buf[NumBytesToCopy] = 0;
    }
}

} // namespace Era

// Replace basic C++ memory manager, but only for Visual Studio in multithreaded (/MT) mode.
// [수정] era.dll 에 메모리 관리 기능이 없는 구버전 ERA 에서는 기본 malloc/free 로 자동 대체.
#ifdef _MSC_VER
#ifdef _MT
namespace EraMemory
{

void *(__stdcall *_ClientMemAlloc)(volatile size_t *allocatedSize, size_t Size) = nullptr;
void(__stdcall *_ClientMemFree)(volatile size_t *allocatedSize, const void *Buf) = nullptr;
void *(__stdcall *_ClientMemRealloc)(volatile size_t *allocatedSize, const void *Buf, size_t NewSize) = nullptr;
size_t *(__stdcall *RegisterMemoryConsumer)(const char *ConsumerName) = nullptr;

HINSTANCE hEra = nullptr;
volatile size_t *allocatedMemorySize = nullptr;

HMODULE GetCurrentModuleHandle()
{
    HMODULE result = nullptr;

    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&GetCurrentModuleHandle), &result);

    return result;
}

void InitMemoryManager()
{
    if (!hEra)
    {
        char pluginLibraryPath[256];

        hEra = LoadLibraryA("era.dll");

        if (!hEra)
        {
            return;
        }

        _ClientMemAlloc = reinterpret_cast<decltype(_ClientMemAlloc)>(GetProcAddress(hEra, "_ClientMemAlloc"));
        _ClientMemFree = reinterpret_cast<decltype(_ClientMemFree)>(GetProcAddress(hEra, "_ClientMemFree"));
        _ClientMemRealloc = reinterpret_cast<decltype(_ClientMemRealloc)>(GetProcAddress(hEra, "_ClientMemRealloc"));
        RegisterMemoryConsumer =
            reinterpret_cast<decltype(RegisterMemoryConsumer)>(GetProcAddress(hEra, "RegisterMemoryConsumer"));

        if (!_ClientMemAlloc || !_ClientMemFree || !RegisterMemoryConsumer)
        {
            _ClientMemAlloc = nullptr;
            _ClientMemFree = nullptr;
            _ClientMemRealloc = nullptr;
            return;
        }

        pluginLibraryPath[0] = 0;
        size_t filePathLen = GetModuleFileNameA(GetCurrentModuleHandle(), pluginLibraryPath, sizeof(pluginLibraryPath));
        char *pluginLibraryName = pluginLibraryPath;

        if (filePathLen > 0)
        {
            int i = filePathLen - 1;

            while (i && pluginLibraryPath[i] != '\\')
            {
                i--;
            }

            if (pluginLibraryPath[i] == '\\')
            {
                i++;
            }

            pluginLibraryName = &pluginLibraryPath[i];
        }

        allocatedMemorySize = RegisterMemoryConsumer(pluginLibraryName);
    }
}

inline bool UseEraMemory()
{
    return _ClientMemAlloc != nullptr && _ClientMemFree != nullptr && allocatedMemorySize != nullptr;
}

} // namespace EraMemory

void *operator new(size_t size)
{
    EraMemory::InitMemoryManager();

    if (!EraMemory::UseEraMemory())
    {
        return std::malloc(size);
    }

    return EraMemory::_ClientMemAlloc(EraMemory::allocatedMemorySize, size);
}

void operator delete(void *ptr) noexcept
{
    EraMemory::InitMemoryManager();

    if (!EraMemory::UseEraMemory())
    {
        std::free(ptr);
        return;
    }

    EraMemory::_ClientMemFree(EraMemory::allocatedMemorySize, ptr);
}

void *operator new[](size_t size)
{
    EraMemory::InitMemoryManager();

    if (!EraMemory::UseEraMemory())
    {
        return std::malloc(size);
    }

    return EraMemory::_ClientMemAlloc(EraMemory::allocatedMemorySize, size);
}

void operator delete[](void *ptr) noexcept
{
    EraMemory::InitMemoryManager();

    if (!EraMemory::UseEraMemory())
    {
        std::free(ptr);
        return;
    }

    EraMemory::_ClientMemFree(EraMemory::allocatedMemorySize, ptr);
}
#endif
#endif
