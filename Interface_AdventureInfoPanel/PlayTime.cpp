#include "pch.h"
#include "PlayTime.h"
#include "InfoPanel.h"

namespace infopanel
{
namespace playtime
{
namespace
{
constexpr const char *SAVE_SECTION = "DeathStare.AdventureInfoPanel.PlayTime";
constexpr UINT32 SAVE_MAGIC = 0x50495344; // "DSIP"
constexpr UINT32 SAVE_VERSION = 1;

#pragma pack(push, 1)
struct SaveRecord
{
    UINT32 magic;
    UINT32 version;
    UINT64 totalMs;
};
#pragma pack(pop)

struct State
{
    UINT64 totalMs = 0;      // 지금까지 누적된 시간
    DWORD lastTick = 0;      // 마지막으로 누적에 반영한 GetTickCount()
    bool running = false;    // 게임 화면 안에 있는 동안만 true
    bool loadPending = false; // 세이브를 읽었고 아직 OnGameEnter 가 오지 않음
    UINT64 loadedMs = 0;
} g;

// 흐른 시간을 누적값에 더한다. (DWORD 뺄셈이라 GetTickCount 가 한 바퀴 돌아도 안전)
void Fold()
{
    if (!g.running)
        return;
    const DWORD now = GetTickCount();
    g.totalMs += static_cast<DWORD>(now - g.lastTick);
    g.lastTick = now;
}

void Start(UINT64 totalMs)
{
    g.totalMs = totalMs;
    g.lastTick = GetTickCount();
    g.running = true;
    MarkDirty();
}

// 새 게임 / 맵 시작 / 리스타트: ERM 명령(!#)이 실행된 직후. 불러오기에서는 오지 않는다.
void __stdcall OnAfterErmInstructions(Era::TEvent *)
{
    g.loadPending = false;
    Start(0);
}

void __stdcall OnSavegameWrite(Era::TEvent *)
{
    Fold();
    SaveRecord rec{SAVE_MAGIC, SAVE_VERSION, g.totalMs};
    Era::WriteSavegameSection(sizeof(rec), &rec, SAVE_SECTION);
}

void __stdcall OnSavegameRead(Era::TEvent *)
{
    SaveRecord rec{};
    const int read = Era::ReadSavegameSection(sizeof(rec), &rec, SAVE_SECTION);
    const bool valid = read == static_cast<int>(sizeof(rec)) && rec.magic == SAVE_MAGIC;
    g.loadedMs = valid ? rec.totalMs : 0; // 이 플러그인 이전의 세이브는 0 부터
    g.loadPending = true;
}

// 새 게임이든 불러오기든 모험 지도에 들어가기 직전에 온다.
// 바로 전에 세이브를 읽었다면 그 값부터, 아니면(새 게임/리스타트) 0 부터.
void __stdcall OnGameEnter(Era::TEvent *)
{
    const UINT64 start = g.loadPending ? g.loadedMs : 0;
    g.loadPending = false;
    Start(start);
}

void __stdcall OnGameLeave(Era::TEvent *)
{
    Fold();
    g.running = false;
    g.loadPending = false;
}
} // namespace

void RegisterEvents()
{
    Era::RegisterHandler(OnAfterErmInstructions, "OnAfterErmInstructions");
    Era::RegisterHandler(OnSavegameWrite, "OnSavegameWrite");
    Era::RegisterHandler(OnSavegameRead, "OnSavegameRead");
    Era::RegisterHandler(OnGameEnter, "OnGameEnter");
    Era::RegisterHandler(OnGameLeave, "OnGameLeave");
}

unsigned int TotalMinutes()
{
    Fold();
    return static_cast<unsigned int>(g.totalMs / 60000ULL);
}
} // namespace playtime
} // namespace infopanel
