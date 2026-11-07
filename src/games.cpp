#include "games.h"

#include "core.h"
#include "edition.h"
#include "fortnite.h"

#include <tlhelp32.h>

#include <cstdio>
#include <map>
#include <mutex>

namespace game {

static bool ToLowerExt(const std::string& n) { return lb::ToLower(n.substr(n.size() - 4)) == ".exe"; }

const std::vector<Profile>& Profiles() {
    static const std::vector<Profile> p = {
        {"fortnite", "Fortnite", {"FortniteClient-Win64-Shipping.exe"},
         "У грі: NVIDIA Reflex «Увімк. + прискорення», режим продуктивності, повноекранний режим, VSync вимкнено. "
         "Регіон підбору гравців — з найменшим пінгом."},
        {"cs2", "Counter-Strike 2", {"cs2.exe"},
         "У грі: NVIDIA Reflex «Увімкнено + прискорення», вертикальна синхронізація вимкнена, режим «Повний екран». "
         "У Steam можна додати параметр запуску -fullscreen.",
         730, "game\\bin\\win64\\cs2.exe"},
        {"valorant", "VALORANT", {"VALORANT-Win64-Shipping.exe"},
         "У грі: NVIDIA Reflex «Увімк. + прискорення», режим «Повноекранний», обмеження FPS вимкнено або вище частоти "
         "монітора. Vanguard (античит) KairoBoost не чіпає.",
         0, nullptr, "Riot Games\\VALORANT\\live\\ShooterGame\\Binaries\\Win64\\VALORANT-Win64-Shipping.exe"},
        {"lol", "League of Legends", {"League of Legends.exe"},
         "У грі: обмеження кадрів — без обмеження або вище частоти монітора, вертикальна синхронізація вимкнена.",
         0, nullptr, "Riot Games\\League of Legends\\Game\\League of Legends.exe"},
        {"apex", "Apex Legends", {"r5apex.exe", "r5apex_dx12.exe"},
         "У грі: NVIDIA Reflex «Увімк. + прискорення», вертикальна синхронізація вимкнена, режим «Повноекранний».",
         1172470, "r5apex.exe"},
        {"cod", "Call of Duty / Warzone", {"cod.exe"},
         "У грі: NVIDIA Reflex «Увімк. + прискорення», режим відображення «Повноекранний ексклюзивний», вертикальна "
         "синхронізація вимкнена. Шлях до гри KairoBoost визначить, коли запустиш її."},
        {"dota2", "Dota 2", {"dota2.exe"},
         "У грі: NVIDIA Reflex увімкнено, вертикальна синхронізація вимкнена, режим «Повний екран».", 570,
         "game\\bin\\win64\\dota2.exe"},
        {"ow2", "Overwatch 2", {"Overwatch.exe"},
         "У грі: NVIDIA Reflex «Увімк. + прискорення», режим «Повноекранний», вертикальна синхронізація вимкнена, "
         "зменшення затримки буфера увімкнено.",
         2357570, "Overwatch.exe", "Overwatch\\_retail_\\Overwatch.exe"},
        {"rl", "Rocket League", {"RocketLeague.exe"},
         "У грі: режим «Повноекранний», вертикальна синхронізація вимкнена, обмеження FPS — вище частоти монітора.",
         252950, "Binaries\\Win64\\RocketLeague.exe"},
        {"r6", "Rainbow Six Siege", {"RainbowSix.exe", "RainbowSix_Vulkan.exe", "RainbowSix_BE.exe"},
         "У грі: NVIDIA Reflex «Увімк. + прискорення», режим «Повноекранний», вертикальна синхронізація вимкнена.",
         359550, "RainbowSix.exe"},
        {"pubg", "PUBG: Battlegrounds", {"TslGame.exe"},
         "У грі: NVIDIA Reflex увімкнено, режим «Повноекранний», вертикальна синхронізація вимкнена.", 578080,
         "TslGame\\Binaries\\Win64\\TslGame.exe"},
        {"gta5", "GTA V", {"GTA5.exe", "GTA5_Enhanced.exe"},
         "У грі: вертикальна синхронізація вимкнена, режим «Повноекранний».", 271590, "GTA5.exe"},
        {"marvel", "Marvel Rivals", {"Marvel-Win64-Shipping.exe"},
         "У грі: NVIDIA Reflex «Увімк. + прискорення», вертикальна синхронізація вимкнена, режим «Повноекранний».",
         2767030, "MarvelGame\\Marvel\\Binaries\\Win64\\Marvel-Win64-Shipping.exe"},
        {"tarkov", "Escape from Tarkov", {"EscapeFromTarkov.exe"},
         "У грі: режим «Повноекранний», вертикальна синхронізація вимкнена."},
        {"roblox", "Roblox", {"RobloxPlayerBeta.exe"},
         "У грі: обмеження FPS — максимальне (налаштування → Maximum Frame Rate)."},
        {"minecraft", "Minecraft (Java)", {"javaw.exe"},
         "У грі: VSync вимкнено, максимальна частота кадрів — без обмеження. Твіки діють на всі Java-програми."},
        {"custom", "Своя гра", {},
         "Вкажи exe своєї гри — KairoBoost застосує до неї пріоритет, відеокарту, QoS і режим гри."},
    };
    return p;
}

static std::mutex g_mutex;
static std::string g_current = "fortnite";
static std::string g_customExe;
static std::map<std::string, std::string> g_manual;
static std::map<std::string, std::pair<ULONGLONG, std::string>> g_cache;  // id -> (час, шлях)

const Profile& Current() {
    std::lock_guard<std::mutex> l(g_mutex);
    for (auto& p : Profiles())
        if (g_current == p.id) return p;
    return Profiles()[0];
}

bool Select(const std::string& id) {
#if !KB_IS_SALE
    (void)id;
    return true;  // особиста редакція — завжди Fortnite
#else
    for (auto& p : Profiles())
        if (id == p.id) {
            std::lock_guard<std::mutex> l(g_mutex);
            g_current = id;
            return true;
        }
    return false;
#endif
}

bool IsFortnite() { return std::string(Current().id) == "fortnite"; }

std::string DisplayName() {
    const Profile& p = Current();
    if (std::string(p.id) != "custom") return p.name;
    auto ex = CurrentExes();
    if (ex.empty()) return "гра";
    std::string n = ex[0];
    if (n.size() > 4 && ToLowerExt(n)) n = n.substr(0, n.size() - 4);
    return n;
}

void SetCustomExe(const std::string& exeName) {
    std::lock_guard<std::mutex> l(g_mutex);
    g_customExe = exeName;
    g_cache.clear();
}

std::vector<std::string> CurrentExes() {
    const Profile& p = Current();
    if (std::string(p.id) == "custom") {
        std::lock_guard<std::mutex> l(g_mutex);
        std::string e = g_customExe;
        size_t s = e.find_last_of("\\/");
        if (s != std::string::npos) e = e.substr(s + 1);
        return e.empty() ? std::vector<std::string>{} : std::vector<std::string>{e};
    }
    return p.exes;
}

std::string ExeList() {
    std::string r;
    for (auto& e : CurrentExes()) r += (r.empty() ? "" : ";") + e;
    return r;
}

void SetManualPath(const std::string& path) {
    std::lock_guard<std::mutex> l(g_mutex);
    g_manual[g_current] = path;
    g_cache.erase(g_current);
}

std::string ManualPath() {
    std::lock_guard<std::mutex> l(g_mutex);
    auto it = g_manual.find(g_current);
    return it == g_manual.end() ? "" : it->second;
}

// ---------------------------------------------------------------------------
// Пошук гри
// ---------------------------------------------------------------------------

static std::string ReadText(const std::string& path) {
    std::string out;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return out;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return out;
}

// Значення "key" "value" з файлів Steam (VDF/ACF)
static std::vector<std::string> VdfValues(const std::string& text, const std::string& key) {
    std::vector<std::string> out;
    std::string pat = "\"" + key + "\"";
    size_t pos = 0;
    while ((pos = text.find(pat, pos)) != std::string::npos) {
        pos += pat.size();
        size_t a = text.find('"', pos);
        if (a == std::string::npos) break;
        std::string v;
        size_t i = a + 1;
        for (; i < text.size() && text[i] != '"'; ++i) {
            if (text[i] == '\\' && i + 1 < text.size()) ++i;
            v += text[i];
        }
        out.push_back(v);
        pos = i;
    }
    return out;
}

static std::string FindSteam(unsigned appId, const char* rel) {
    std::string steam;
    if (!lb::RegGetString(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath", steam) || steam.empty())
        return "";
    for (auto& c : steam)
        if (c == '/') c = '\\';
    std::vector<std::string> libs = {steam};
    for (auto& l : VdfValues(ReadText(steam + "\\steamapps\\libraryfolders.vdf"), "path")) libs.push_back(l);
    for (auto& lib : libs) {
        std::string acf = ReadText(lib + "\\steamapps\\appmanifest_" + std::to_string(appId) + ".acf");
        auto dirs = VdfValues(acf, "installdir");
        if (dirs.empty()) continue;
        std::string exe = lib + "\\steamapps\\common\\" + dirs[0] + "\\" + rel;
        if (lb::FileExists(exe)) return exe;
    }
    return "";
}

static std::string FindFixed(const char* rel) {
    std::vector<std::string> roots;
    for (const char* env : {"ProgramFiles", "ProgramFiles(x86)"}) {
        std::string v = lb::GetEnv(env);
        if (!v.empty()) roots.push_back(v);
    }
    for (char d = 'C'; d <= 'F'; ++d) roots.push_back(std::string(1, d) + ":");
    for (auto& r : roots) {
        std::string exe = r + "\\" + rel;
        if (lb::FileExists(exe)) return exe;
    }
    return "";
}

static std::string FindRoblox() {
    std::string base = lb::GetEnv("LOCALAPPDATA") + "\\Roblox\\Versions";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((base + "\\*").c_str(), &fd);
    std::string found;
    if (h == INVALID_HANDLE_VALUE) return found;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
        std::string exe = base + "\\" + fd.cFileName + "\\RobloxPlayerBeta.exe";
        if (lb::FileExists(exe)) found = exe;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return found;
}

// Шлях запущеного процесу гри (працює і для ігор з античитом — потрібен лише дозвіл на читання шляху)
static std::string RunningPath(const std::vector<std::string>& exes) {
    if (exes.empty()) return "";
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return "";
    PROCESSENTRY32 pe{};
    pe.dwSize = sizeof(pe);
    std::string path;
    if (Process32First(snap, &pe)) {
        do {
            for (auto& e : exes)
                if (lb::ToLower(pe.szExeFile) == lb::ToLower(e)) {
                    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
                    if (h) {
                        char buf[MAX_PATH * 2];
                        DWORD len = sizeof(buf);
                        if (QueryFullProcessImageNameA(h, 0, buf, &len)) path.assign(buf, len);
                        CloseHandle(h);
                    }
                }
        } while (path.empty() && Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return path;
}

bool IsRunning() { return lb::FindProcess(ExeList()) != 0; }

std::string CurrentPath() {
    const Profile& p = Current();
    std::string id = p.id;
    std::string manual = ManualPath();
    if (!manual.empty() && lb::FileExists(manual)) return manual;
    if (id == "fortnite") return fn::ExePath();
    {
        std::lock_guard<std::mutex> l(g_mutex);
        auto it = g_cache.find(id);
        if (it != g_cache.end() && GetTickCount64() - it->second.first < 10000) return it->second.second;
    }
    std::string path;
    if (p.steamAppId) path = FindSteam(p.steamAppId, p.steamRel);
    if (path.empty() && p.fixedRel) path = FindFixed(p.fixedRel);
    if (path.empty() && id == "roblox") path = FindRoblox();
    if (path.empty() && id == "custom") {
        std::lock_guard<std::mutex> l(g_mutex);
        if (lb::FileExists(g_customExe)) path = g_customExe;
    }
    if (path.empty()) path = RunningPath(CurrentExes());
    std::lock_guard<std::mutex> l(g_mutex);
    g_cache[id] = {GetTickCount64(), path};
    return path;
}

const std::vector<std::string>& AntiCheatProcesses() {
    static const std::vector<std::string> v = {"EasyAntiCheat.exe",  "EasyAntiCheat_EOS.exe", "BEService.exe",
                                               "BEService_x64.exe",  "vgc.exe",               "vgtray.exe",
                                               "FACEIT.exe",         "faceitservice.exe",     "GameGuard.des",
                                               "EAAntiCheat.GameService.exe"};
    return v;
}

}  // namespace game
