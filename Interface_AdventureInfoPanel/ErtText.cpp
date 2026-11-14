#include "pch.h"
#include "ErtText.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>

namespace infopanel
{
namespace ert
{
namespace
{
constexpr const char *PRIMARY_FILES[] = {
    "Data\\s\\The Multiverse - 10. Hint.ert",
    "The Multiverse - 10. Hint.ert",
};

// 기본 문구 (CP949 바이트로 적어 둠 - 소스 파일 인코딩/컴파일러 설정과 상관없이 게임에서 깨지지 않도록)
struct Fallback
{
    int id;
    const char *text;
};
constexpr Fallback FALLBACKS[] = {
    {LABEL_CLOCK, "\xC7\xF6\xC0\xE7 \xBD\xC3\xB0\xA3"},                // 현재 시간
    {LABEL_PLAYTIME, "\xC7\xC3\xB7\xB9\xC0\xCC \xBD\xC3\xB0\xA3"},     // 플레이 시간
    {BLOG_URL, "https://heroesdeathstare.blogspot.com/"},
    {HINT_CLOCK, "\xC7\xC3\xB7\xB9\xC0\xCC\xBE\xEE\xB0\xA1 \xC0\xA7\xC4\xA1\xC7\xD1 \xC1\xF6\xBF\xAA\xC0\xC7 "
                 "\xC7\xF6\xC0\xE7 \xBD\xC3\xB0\xA3"},                   // 플레이어가 위치한 지역의 현재 시간
    {HINT_PLAYTIME, "\xC7\xF6\xC0\xE7 \xB8\xCA\xC0\xBB \xC7\xC3\xB7\xB9\xC0\xCC\xC7\xD1 \xB4\xA9\xC0\xFB "
                    "\xBD\xC3\xB0\xA3"},                                 // 현재 맵을 플레이한 누적 시간
    {HINT_BUTTON, "\xB0\xB3\xB9\xDF\xC0\xDA\xC0\xC7 \xBA\xED\xB7\xCE\xB1\xD7\xB8\xA6 \xB9\xE6\xB9\xAE\xC7\xCF"
                  "\xBF\xA9 \xB4\xF5 \xB8\xB9\xC0\xBA \xC1\xA4\xBA\xB8\xB8\xA6 \xC5\xBD\xBB\xF6\xC7\xD5"
                  "\xB4\xCF\xB4\xD9."},                                  // 개발자의 블로그를 방문하여 더 많은 정보를 탐색합니다.
};

std::map<int, std::string> g_texts;
bool g_primaryLoaded = false;
bool g_fullScanDone = false;

void Trim(std::string &s)
{
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
    {
        s.clear();
        return;
    }
    const size_t e = s.find_last_not_of(" \t\r\n");
    s = s.substr(b, e - b + 1);
}

// "번호<탭>문구" 한 줄씩. 이미 읽은 번호는 덮어쓰지 않는다 (앞에서 읽은 파일 우선).
bool ParseFile(const char *path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
        return false;

    std::string line;
    while (std::getline(file, line))
    {
        if (line.empty() || line[0] == '#' || line[0] == ';')
            continue;
        char *end = nullptr;
        const long id = std::strtol(line.c_str(), &end, 10);
        if (end == line.c_str() || id <= 0)
            continue;
        std::string text(end);
        Trim(text);
        g_texts.emplace(static_cast<int>(id), text);
    }
    return true;
}

void ScanFolder(const std::string &folder)
{
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((folder + "*.ert").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do
    {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            ParseFile((folder + fd.cFileName).c_str());
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

void ScanAllErtFiles()
{
    ScanFolder("Data\\s\\");

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA("Mods\\*", &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do
    {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.')
            ScanFolder(std::string("Mods\\") + fd.cFileName + "\\Data\\s\\");
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

const char *FallbackText(int id)
{
    for (const auto &f : FALLBACKS)
        if (f.id == id)
            return f.text;
    return "";
}
} // namespace

const char *Text(int id)
{
    if (!g_primaryLoaded)
    {
        g_primaryLoaded = true;
        for (const char *path : PRIMARY_FILES)
            if (ParseFile(path))
                break;
    }

    auto it = g_texts.find(id);
    if (it == g_texts.end() && !g_fullScanDone)
    {
        g_fullScanDone = true;
        ScanAllErtFiles();
        it = g_texts.find(id);
    }
    if (it != g_texts.end() && !it->second.empty())
        return it->second.c_str();
    return FallbackText(id);
}
} // namespace ert
} // namespace infopanel
