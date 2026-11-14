#pragma once
#include <string>

// ERT 문구 읽기.
// 1) "Data/s/The Multiverse - 10. Hint.ert" (Interface_EnhancedCreatureInformation 과 같은 파일)
// 2) 못 찾은 번호가 있으면 Data/s/*.ert, Mods/*/Data/s/*.ert 를 차례로 찾아본다.
// 3) 그래도 없으면 코드에 넣어 둔 기본 문구를 쓴다.
namespace infopanel
{
namespace ert
{
enum eId : int
{
    LABEL_CLOCK = 202782,    // 현재 시간
    LABEL_PLAYTIME = 202783, // 플레이 시간
    BLOG_URL = 202784,       // https://heroesdeathstare.blogspot.com/
    HINT_CLOCK = 202785,     // 플레이어가 위치한 지역의 현재 시간
    HINT_PLAYTIME = 202786,  // 현재 맵을 플레이한 누적 시간
    HINT_BUTTON = 202787,    // 개발자의 블로그를 방문하여 더 많은 정보를 탐색합니다.
};

// 문구를 돌려준다. 반환값은 플러그인이 끝날 때까지 유효하다.
const char *Text(int id);
} // namespace ert
} // namespace infopanel
