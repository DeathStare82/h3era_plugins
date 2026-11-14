#pragma once

// 맵 플레이 누적 시간
//  - 새 게임/맵 시작/리스타트: 0 부터
//  - 저장: 그 순간까지의 누적 시간을 세이브 파일에 함께 기록 (ERA 세이브 섹션)
//  - 불러오기: 세이브에 기록된 값부터 다시 누적 (기록이 없는 예전 세이브는 0 부터)
//  - 게임 화면을 벗어나 메인 메뉴에 있는 동안은 세지 않는다
namespace infopanel
{
namespace playtime
{
void RegisterEvents();

// 현재까지의 누적 시간(분). 호출할 때마다 흐른 시간을 반영한다.
unsigned int TotalMinutes();
} // namespace playtime
} // namespace infopanel
