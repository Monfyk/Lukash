// KairoBoost — графічна версія (Dear ImGui + DirectX 11)
#include "core.h"
#include "fortnite.h"
#include "tweaks.h"
#include "dns.h"
#include "edition.h"
#include "games.h"

#include <commdlg.h>
#include "advisor.h"

#include <shellapi.h>

#include <d3d11.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// ---------------------------------------------------------------------------
// Значок у треї
// ---------------------------------------------------------------------------

static const UINT WM_TRAY = WM_APP + 1;
static const UINT WM_SHOW_EXISTING = WM_APP + 2;  // повторний запуск програми
static const UINT ID_TRAY_OPEN = 1001, ID_TRAY_EXIT = 1002;
static NOTIFYICONDATAW g_tray{};
static UINT g_taskbarCreated = 0;
static bool g_reallyQuit = false;

static void TrayAdd(HWND hwnd) {
    g_tray.cbSize = sizeof(g_tray);
    g_tray.hWnd = hwnd;
    g_tray.uID = 1;
    g_tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_tray.uCallbackMessage = WM_TRAY;
    g_tray.hIcon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON,
                                     GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    if (!g_tray.hIcon) g_tray.hIcon = LoadIconW(nullptr, (LPCWSTR)IDI_APPLICATION);
    wcsncpy(g_tray.szTip, L"KairoBoost", ARRAYSIZE(g_tray.szTip) - 1);
    Shell_NotifyIconW(NIM_ADD, &g_tray);
}

static void TrayRemove() { Shell_NotifyIconW(NIM_DELETE, &g_tray); }

static void TrayBalloon(const wchar_t* title, const wchar_t* text) {
    NOTIFYICONDATAW n = g_tray;
    n.uFlags = NIF_INFO;
    wcsncpy(n.szInfoTitle, title, ARRAYSIZE(n.szInfoTitle) - 1);
    wcsncpy(n.szInfo, text, ARRAYSIZE(n.szInfo) - 1);
    n.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

static void ShowFromTray(HWND hwnd) {
    ShowWindow(hwnd, SW_SHOW);
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    SetForegroundWindow(hwnd);
}

// Сховати вікно: у трей (якщо ввімкнено) або на панель задач
struct App;
static void HideWindow(HWND hwnd, bool toTray);

// ---------------------------------------------------------------------------
// Кольори та шрифти
// ---------------------------------------------------------------------------

static const ImVec4 kBg = ImVec4(0.055f, 0.063f, 0.086f, 1.0f);
static const ImVec4 kPanel = ImVec4(0.086f, 0.098f, 0.133f, 1.0f);
static const ImVec4 kPanel2 = ImVec4(0.118f, 0.133f, 0.180f, 1.0f);
static const ImVec4 kAccent = ImVec4(1.000f, 0.420f, 0.169f, 1.0f);   // помаранчевий
static const ImVec4 kAccent2 = ImVec4(0.169f, 0.784f, 1.000f, 1.0f);  // блакитний
static const ImVec4 kGood = ImVec4(0.290f, 0.871f, 0.502f, 1.0f);
static const ImVec4 kWarn = ImVec4(1.000f, 0.780f, 0.250f, 1.0f);
static const ImVec4 kBad = ImVec4(1.000f, 0.330f, 0.330f, 1.0f);
static const ImVec4 kMuted = ImVec4(0.560f, 0.600f, 0.680f, 1.0f);
static const ImVec4 kFortnite = ImVec4(0.659f, 0.443f, 1.000f, 1.0f);  // фіолетовий

static ImFont* g_fontBody = nullptr;
static ImFont* g_fontBold = nullptr;
static ImFont* g_fontTitle = nullptr;
static ImFont* g_fontBig = nullptr;
static float g_dpi = 1.0f;

static ImVec4 Fade(ImVec4 c, float a) { return ImVec4(c.x, c.y, c.z, a); }

static ImVec4 CategoryColor(tw::Category c) {
    switch (c) {
        case tw::Network: return kAccent2;
        case tw::System: return kAccent;
        case tw::Cpu: return kWarn;
        case tw::Gpu: return kGood;
        case tw::Input: return ImVec4(1.0f, 0.45f, 0.70f, 1);
        case tw::Disk: return ImVec4(0.60f, 0.75f, 0.95f, 1);
        default: return kFortnite;
    }
}

// ---------------------------------------------------------------------------
// Фонові задачі
// ---------------------------------------------------------------------------

struct Task {
    std::thread thread;
    std::atomic<bool> running{false};
    std::atomic<bool> cancel{false};

    template <class F>
    bool Start(F fn) {
        if (running) return false;
        if (thread.joinable()) thread.join();
        cancel = false;
        running = true;
        thread = std::thread([this, fn] {
            fn();
            running = false;
        });
        return true;
    }
    void Join() {
        cancel = true;
        if (thread.joinable()) thread.join();
    }
};

// ---------------------------------------------------------------------------
// Стан застосунку
// ---------------------------------------------------------------------------

struct LiveTarget {
    std::string target, label;
    std::vector<float> samples;  // < 0 — втрачений пакет
    lb::PingResult result;
    bool done = false;

    void Stats(float& mn, float& avg, float& mx, float& jit, float& loss) const {
        mn = 1e9f, avg = 0, mx = 0, jit = 0;
        int ok = 0, jn = 0;
        float prev = -1;
        for (float s : samples) {
            if (s < 0) continue;
            mn = std::min(mn, s);
            mx = std::max(mx, s);
            avg += s;
            if (prev >= 0) {
                jit += std::fabs(s - prev);
                ++jn;
            }
            prev = s;
            ++ok;
        }
        avg = ok ? avg / ok : 0;
        jit = jn ? jit / jn : 0;
        if (!ok) mn = 0;
        loss = samples.empty() ? 0 : 100.0f * (samples.size() - ok) / samples.size();
    }
    float Score() const {
        float mn, avg, mx, jit, loss;
        Stats(mn, avg, mx, jit, loss);
        if (avg <= 0) return 1e9f;
        return avg + 2 * jit + loss * 10;
    }
};

enum Page { PageHome, PageAdvisor, PageFortnite, PageTweaks, PageTimer, PageRam, PageNet, PageDns, PageGame, PageInfo, PageCount };

struct DnsRow {
    int provider = -1;  // -1 = поточний DNS від провайдера
    std::string name, servers, note;
    std::string ip;
    std::vector<float> samples;
    dns::Result res;
    bool done = false;
};

struct TweakState {
    bool on = false;
    std::string unavailable;
};

struct App {
    Page page = PageHome;
    bool elevated = false;
    DWORD build = 0;

    std::mutex logMutex;
    std::deque<std::string> log;
    bool logScroll = false;

    // Таймер
    float resMs = 0.5f;
    std::mutex sleepMutex;
    std::vector<float> sleepSamples;
    lb::SleepStats sleepStats;
    bool hasSleep = false;
    Task timerTask;

    // Мережа
    char targets[512] = "1.1.1.1 8.8.8.8 9.9.9.9";
    int pingCount = 30;
    std::mutex netMutex;
    std::vector<LiveTarget> live;
    bool liveIsFortnite = false;
    Task netTask;

    // Твіки
    std::vector<TweakState> tweakState;
    double tweakStateTime = -1e9;
    std::string pendingTweak;
    int tweakFilter = -1;  // -1 = усі
    Task tweakTask;
    bool confirmRevert = false;

    // Fortnite
    char fnDir[MAX_PATH] = "";
    std::string fnExe;
    bool fnRunning = false;
    bool onWifi = false;

    // Порадник
    Task advTask;
    std::mutex advMutex;
    adv::SystemInfo advInfo;
    std::vector<adv::Advice> advList;
    bool advReady = false, advStarted = false;

    // Повтор клавіш
    int kbDelay = 150, kbInterval = 10;
    char kbTest[256] = "";
    fn::Settings fnSettings;
    int fnFpsIdx = 0;
    bool fnVsync = false, fnPerf = false, fnFullscreen = false, fnLowGfx = false;
    bool fnUiLoaded = false;
    double fnTime = -1e9;

    // Режим гри
    char gameExe[256] = "FortniteClient-Win64-Shipping.exe";
    // Обрана гра (редакція для продажу)
    std::string gameId = "fortnite";
    char customExe[MAX_PATH] = "";
    char gamePath[MAX_PATH] = "";
    float gameRes = 0.5f;
    std::atomic<bool> gameStop{false};
    std::atomic<DWORD> gamePid{0};
    Task gameTask;

    // Очистка ОЗУ
    Task ramTask;
    bool ramAuto = true;
    int ramMode = 0;  // 0 = під час запуску гри, 1 = мало вільної пам'яті, 2 = за розкладом
    int ramFreePct = 20;
    int ramHoursIdx = 2;
    std::vector<std::string> ramExclude = {"FortniteClient-Win64-Shipping.exe", "EasyAntiCheat_EOS.exe",
                                           "audiodg.exe", "obs64.exe", "Discord.exe"};
    char ramNewExclude[128] = "";
    std::string ramLastText = "ще не виконувалась";
    ULONGLONG ramLastTick = 0;
    ULONGLONG ramCheckTick = 0;
    std::atomic<LONGLONG> ramLastFreed{0};
    std::atomic<bool> ramRequest{false};  // запит на очистку з потоку режиму гри

    bool minimizeOnGame = true;  // згортати вікно, коли гра запущена
    bool closeToTray = true;     // хрестик ховає програму в трей
    bool trayHintShown = false;

    // DNS
    std::mutex dnsMutex;
    std::vector<DnsRow> dnsRows;
    Task dnsTask;
    int dnsCurrent = -1;
    std::string dnsCurrentText;
    bool dnsChanged = false;
    double dnsInfoTime = -1e9;
    bool dnsTested = false;
    std::atomic<bool> dnsIntercepted{false};
    bool lowPower = false;       // економний режим зараз активний
};

static App* g_app = nullptr;
static const int kFpsOptions[] = {0, 60, 120, 144, 165, 240, 360};
static const char* kFpsNames[] = {"Без обмеження", "60", "120", "144", "165", "240", "360"};
static const int kHours[] = {1, 2, 4, 6, 12};
static const char* kHoursNames[] = {"Раз на 1 годину", "Раз на 2 години", "Раз на 4 години", "Раз на 6 годин",
                                    "Раз на 12 годин"};

static void AddLog(const std::string& s) {
    std::lock_guard<std::mutex> l(g_app->logMutex);
    SYSTEMTIME t;
    GetLocalTime(&t);
    char ts[16];
    snprintf(ts, sizeof(ts), "%02d:%02d:%02d  ", t.wHour, t.wMinute, t.wSecond);
    g_app->log.push_back(ts + s);
    while (g_app->log.size() > 500) g_app->log.pop_front();
    g_app->logScroll = true;
}

// ---------------------------------------------------------------------------
// Налаштування (%APPDATA%\KairoBoost\settings.ini)
// ---------------------------------------------------------------------------

static std::string SettingsFile() {
    std::string dir = lb::GetEnv("APPDATA") + "\\KairoBoost";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir + "\\settings.ini";
}

static void SaveSettings(const App& a) {
    std::string f = SettingsFile();
    auto put = [&](const char* k, const std::string& v) { WritePrivateProfileStringA("KairoBoost", k, v.c_str(), f.c_str()); };
    put("GameExe", a.gameExe);
    put("Game", a.gameId);
    put("CustomExe", a.customExe);
    put(("GamePath_" + a.gameId).c_str(), a.gamePath);
    put("GameRes", std::to_string(a.gameRes));
    put("FortniteDir", a.fnDir);
    put("RamAuto", a.ramAuto ? "1" : "0");
    put("RamMode", std::to_string(a.ramMode));
    put("RamFreePct", std::to_string(a.ramFreePct));
    put("RamHours", std::to_string(a.ramHoursIdx));
    std::string ex;
    for (auto& e : a.ramExclude) ex += (ex.empty() ? "" : ";") + e;
    put("RamExclude", ex);
    put("Targets", a.targets);
    put("MinimizeOnGame", a.minimizeOnGame ? "1" : "0");
    put("KeyDelay", std::to_string(a.kbDelay));
    put("KeyInterval", std::to_string(a.kbInterval));
    put("CloseToTray", a.closeToTray ? "1" : "0");
    put("TrayHintShown", a.trayHintShown ? "1" : "0");
}

static void LoadSettings(App& a) {
    std::string f = SettingsFile();
    if (!lb::FileExists(f)) return;
    char buf[2048];
    auto get = [&](const char* k) {
        GetPrivateProfileStringA("KairoBoost", k, "\x01", buf, sizeof(buf), f.c_str());
        return std::string(buf);
    };
    std::string v;
    if ((v = get("GameExe")) != "\x01" && !v.empty()) snprintf(a.gameExe, sizeof(a.gameExe), "%s", v.c_str());
    if ((v = get("Game")) != "\x01" && !v.empty()) a.gameId = v;
    if ((v = get("CustomExe")) != "\x01") snprintf(a.customExe, sizeof(a.customExe), "%s", v.c_str());
    if ((v = get(("GamePath_" + a.gameId).c_str())) != "\x01") snprintf(a.gamePath, sizeof(a.gamePath), "%s", v.c_str());
    if ((v = get("GameRes")) != "\x01") a.gameRes = std::clamp((float)atof(v.c_str()), 0.5f, 2.0f);
    if ((v = get("FortniteDir")) != "\x01") snprintf(a.fnDir, sizeof(a.fnDir), "%s", v.c_str());
    if ((v = get("RamAuto")) != "\x01") a.ramAuto = v == "1";
    if ((v = get("RamMode")) != "\x01") a.ramMode = std::clamp(atoi(v.c_str()), 0, 2);
    if ((v = get("RamFreePct")) != "\x01") a.ramFreePct = std::clamp(atoi(v.c_str()), 5, 50);
    if ((v = get("RamHours")) != "\x01") a.ramHoursIdx = std::clamp(atoi(v.c_str()), 0, 4);
    if ((v = get("RamExclude")) != "\x01") {
        a.ramExclude.clear();
        std::string cur;  // ділимо лише за ";" — в іменах exe бувають пробіли
        for (char c : v + ";") {
            if (c == ';') {
                if (!cur.empty()) a.ramExclude.push_back(cur);
                cur.clear();
            } else {
                cur += c;
            }
        }
    }
    if ((v = get("Targets")) != "\x01" && !v.empty()) snprintf(a.targets, sizeof(a.targets), "%s", v.c_str());
    if ((v = get("MinimizeOnGame")) != "\x01") a.minimizeOnGame = v == "1";
    if ((v = get("KeyDelay")) != "\x01") a.kbDelay = std::clamp(atoi(v.c_str()), 100, 1000);
    if ((v = get("KeyInterval")) != "\x01") a.kbInterval = std::clamp(atoi(v.c_str()), 5, 500);
    if ((v = get("CloseToTray")) != "\x01") a.closeToTray = v == "1";
    if ((v = get("TrayHintShown")) != "\x01") a.trayHintShown = v == "1";
}

// ---------------------------------------------------------------------------
// Дрібні віджети
// ---------------------------------------------------------------------------

static float S(float v) { return v * g_dpi; }

static bool AccentButton(const char* label, ImVec2 size, ImVec4 color = kAccent) {
    ImGui::PushStyleColor(ImGuiCol_Button, color);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(std::min(color.x + 0.08f, 1.f),
                                                          std::min(color.y + 0.08f, 1.f),
                                                          std::min(color.z + 0.08f, 1.f), 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Fade(color, 0.8f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.05f, 0.05f, 0.07f, 1));
    bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return r;
}

static void Dot(ImVec4 color) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = ImGui::GetTextLineHeight();
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + h * 0.4f, p.y + h * 0.5f), h * 0.28f,
                                                ImGui::GetColorU32(color));
    ImGui::Dummy(ImVec2(h * 0.9f, h));
    ImGui::SameLine();
}

static void Pill(const char* text, ImVec4 color) {
    ImVec2 ts = ImGui::CalcTextSize(text);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float px = S(8), py = S(2);
    ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + ts.x + px * 2, p.y + ts.y + py * 2),
                                              ImGui::GetColorU32(Fade(color, 0.16f)), S(6));
    ImGui::SetCursorScreenPos(ImVec2(p.x + px, p.y + py));
    ImGui::TextColored(color, "%s", text);
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(ts.x + px * 2, ts.y + py * 2));
}

// Перемикач у стилі iOS; повертає true при натисканні
static bool Toggle(const char* id, bool on, bool busy) {
    float h = S(24), w = S(46);
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
    bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec4 bg = on ? kGood : ImVec4(0.22f, 0.24f, 0.30f, 1);
    if (hovered) bg = Fade(bg, 0.85f);
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(bg), h * 0.5f);
    float r = h * 0.5f - S(3);
    float cx = on ? p.x + w - h * 0.5f : p.x + h * 0.5f;
    dl->AddCircleFilled(ImVec2(cx, p.y + h * 0.5f), r, ImGui::GetColorU32(ImVec4(1, 1, 1, busy ? 0.4f : 1)));
    return clicked;
}

static void Title(const char* text, const char* subtitle = nullptr) {
    ImGui::PushFont(g_fontTitle);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
    if (subtitle) {
        ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
        ImGui::TextWrapped("%s", subtitle);
        ImGui::PopStyleColor();
    }
    ImGui::Spacing();
}

static void SectionTitle(const char* text) {
    ImGui::PushFont(g_fontTitle);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
}

static bool BeginCard(const char* id, ImVec2 size) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kPanel);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(10));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(16), S(14)));
    return ImGui::BeginChild(id, size,
                             ImGuiChildFlags_Borders | (size.y == 0 ? ImGuiChildFlags_AutoResizeY : 0),
                             ImGuiWindowFlags_NoScrollbar);
}

static void EndCard() {
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

static void StatCard(const char* id, const char* label, const char* value, ImVec4 color, const char* hint,
                     float width) {
    BeginCard(id, ImVec2(width, S(118)));
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::PushFont(g_fontBig);
    ImGui::TextColored(color, "%s", value);
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextUnformatted(hint);
    ImGui::PopStyleColor();
    EndCard();
}

static void HelpText(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

static void InfoBox(const char* text, ImVec4 color = kAccent2) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Fade(color, 0.10f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(8));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(12), S(10)));
    ImGui::BeginChild(ImGui::GetID(text), ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

// ---------------------------------------------------------------------------
// Дії
// ---------------------------------------------------------------------------

static void RefreshTweaks(App& a, bool force = false) {
    if (a.tweakTask.running && a.tweakState.size() == tw::All().size()) return;
    if (a.lowPower && a.tweakState.size() == tw::All().size() && !force) return;  // під час гри не опитуємо реєстр і служби
    if (!force && ImGui::GetTime() - a.tweakStateTime < 2.0) return;
    const auto& all = tw::All();
    a.tweakState.resize(all.size());
    for (size_t i = 0; i < all.size(); ++i) {
        a.tweakState[i].unavailable = tw::Unavailable(all[i]);
        a.tweakState[i].on = a.tweakState[i].unavailable.empty() && tw::IsOn(all[i]);
    }
    a.tweakStateTime = ImGui::GetTime();
}

static void RunTweakJob(App& a, const std::string& pending, std::function<void()> job) {
    a.pendingTweak = pending;
    if (a.tweakTask.Start([&a, job] {
            job();
            a.tweakStateTime = -1e9;  // оновити стан одразу після завершення
        }))
        return;
    a.pendingTweak.clear();
}

static void StartRamClean(App& a, bool purgeStandby = true) {
    std::vector<std::string> ex = a.ramExclude;
    ex.push_back("KairoBoost.exe");
    ex.push_back("KairoBoostPro.exe");
    for (auto& e : game::CurrentExes()) ex.push_back(e);  // пам'ять гри і античитів не чіпаємо ніколи
    for (auto& e : game::AntiCheatProcesses()) ex.push_back(e);
    a.ramTask.Start([&a, ex, purgeStandby] {
        lb::CleanResult r = lb::CleanMemory(ex, purgeStandby);
        a.ramLastFreed = r.freedBytes;
    });
    SYSTEMTIME t;
    GetLocalTime(&t);
    char b[64];
    snprintf(b, sizeof(b), "%02d.%02d.%04d - %02d:%02d", t.wDay, t.wMonth, t.wYear, t.wHour, t.wMinute);
    a.ramLastText = b;
    a.ramLastTick = GetTickCount64();
}

// Автоматична очистка ОЗУ за умовами (викликається щокадру, навіть коли вікно згорнуте)
static void RamScheduler(App& a) {
    if (a.ramRequest && !a.ramTask.running) {
        a.ramRequest = false;
        lb::Log("Гру запущено — звільняю пам'ять фонових програм");
        StartRamClean(a, false);  // кеш файлів гри не чіпаємо, щоб не сповільнити завантаження
        return;
    }
    if (!a.ramAuto || a.ramTask.running || a.ramMode == 0) return;
    ULONGLONG now = GetTickCount64();
    if (now - a.ramCheckTick < 15000) return;
    a.ramCheckTick = now;
    if (a.ramMode == 1) {
        lb::MemInfo m = lb::GetMemInfo();
        int freePct = 100 - (int)m.load;
        bool cooldown = a.ramLastTick && now - a.ramLastTick < 5 * 60 * 1000ULL;
        if (freePct < a.ramFreePct && !cooldown) {
            lb::Log("Вільної пам'яті %d%% — запускаю автоматичну очистку", freePct);
            StartRamClean(a);
        }
    } else if (a.ramMode == 2) {
        ULONGLONG interval = (ULONGLONG)kHours[a.ramHoursIdx] * 3600 * 1000;
        if (!a.ramLastTick) a.ramLastTick = now;  // перший відлік — від запуску програми
        else if (now - a.ramLastTick >= interval) StartRamClean(a);
    }
}

static void StartGameMode(App& a) {
    if (a.gameTask.running) return;
    std::string exe = a.gameExe;
    if (exe.empty()) return;
    ULONG res = (ULONG)std::lround(a.gameRes * 10000.0);
    a.gameStop = false;
    SaveSettings(a);
    a.gameTask.Start([&a, exe, res] {
        lb::RunGameMode(exe, res, a.gameStop, &a.gamePid, [&a](DWORD) {
            if (a.ramAuto && a.ramMode == 0) a.ramRequest = true;
        });
    });
}

static void StopGameMode(App& a) { a.gameStop = true; }

static void StartPingList(App& a, const std::vector<std::pair<std::string, std::string>>& list, bool fortnite) {
    if (list.empty() || a.netTask.running) return;
    {
        std::lock_guard<std::mutex> l(a.netMutex);
        a.live.clear();
        for (auto& t : list) {
            LiveTarget lt;
            lt.target = t.first;
            lt.label = t.second;
            a.live.push_back(lt);
        }
        a.liveIsFortnite = fortnite;
    }
    int count = a.pingCount;
    a.netTask.Start([&a, count] {
        size_t n;
        std::vector<std::string> targets;
        {
            std::lock_guard<std::mutex> l(a.netMutex);
            n = a.live.size();
            for (auto& lt : a.live) targets.push_back(lt.target);
        }
        lb::Log("Тест мережі: %d серверів по %d запитів", (int)n, count);
        std::vector<std::thread> workers;
        for (size_t i = 0; i < n; ++i) {
            workers.emplace_back([&a, i, t = targets[i], count] {
                lb::PingResult r = lb::Ping(
                    t, count,
                    [&a, i](int, double rtt) {
                        std::lock_guard<std::mutex> l(a.netMutex);
                        a.live[i].samples.push_back((float)rtt);
                    },
                    &a.netTask.cancel);
                std::lock_guard<std::mutex> l(a.netMutex);
                a.live[i].result = r;
                a.live[i].done = true;
            });
        }
        for (auto& w : workers) w.join();
        std::lock_guard<std::mutex> l(a.netMutex);
        int best = -1;
        for (int i = 0; i < (int)a.live.size(); ++i) {
            lb::LogPing(a.live[i].result);
            if (a.live[i].result.received && (best < 0 || a.live[i].result.Score() < a.live[best].result.Score()))
                best = i;
        }
        if (best >= 0)
            lb::Log("Найкращий: %s (%.0f мс)", a.live[best].label.empty() ? a.live[best].target.c_str()
                                                                           : a.live[best].label.c_str(),
                    a.live[best].result.avg);
    });
}

static void StartPing(App& a) {
    std::vector<std::pair<std::string, std::string>> list;
    for (auto& t : lb::Split(a.targets)) list.push_back({t, ""});
    StartPingList(a, list, false);
}

static void StartFortnitePing(App& a) {
    std::vector<std::pair<std::string, std::string>> list;
    for (auto& r : fn::Regions()) list.push_back({r.host, std::string(r.name) + " (" + r.code + ")"});
    StartPingList(a, list, true);
}

static void RunBenchmark(App& a) {
    a.timerTask.Start([&a] {
        std::vector<float> samples;
        lb::SleepStats s = lb::MeasureSleep1(300, &samples);
        {
            std::lock_guard<std::mutex> l(a.sleepMutex);
            a.sleepSamples = samples;
            a.sleepStats = s;
            a.hasSleep = true;
        }
        lb::Log("Sleep(1): сер %.3f мс, мін %.3f, макс %.3f", s.avg, s.min, s.max);
    });
}

// ---------------------------------------------------------------------------
// Картка твіка
// ---------------------------------------------------------------------------

static void TweakCard(App& a, size_t idx, float width) {
    const tw::Tweak& t = tw::All()[idx];
    const TweakState& st = a.tweakState[idx];
    bool busy = a.tweakTask.running;
    bool pending = busy && a.pendingTweak == t.id;
    bool unavailable = !st.unavailable.empty();

    ImGui::PushID(t.id);
    BeginCard(t.id, ImVec2(width, S(215)));
    if (unavailable) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.55f);

    Pill(t.category == tw::Input ? "Ввід" : tw::CategoryName(t.category), CategoryColor(t.category));
    if (t.reboot || t.gameRestart) {
        ImGui::SameLine();
        Pill(t.reboot ? "рестарт ПК" : "перезапуск гри", kMuted);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Почне діяти %s", tw::EffectText(t));
    } else {
        ImGui::SameLine();
        Pill("одразу", Fade(kGood, 0.9f));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Діє одразу після ввімкнення");
    }
    ImGui::SameLine(ImGui::GetContentRegionMax().x - S(46));
    ImGui::BeginDisabled(busy || unavailable);
    if (Toggle("##t", st.on, pending)) {
        bool turnOn = !st.on;
        if (std::string(t.id) == "keyboard") tw::SetKeyRepeat(a.kbDelay, a.kbInterval);
        size_t i = idx;
        RunTweakJob(a, t.id, [i, turnOn] {
            const tw::Tweak& tk = tw::All()[i];
            if (turnOn) tw::Apply(tk); else tw::Revert(tk);
        });
    }
    ImGui::EndDisabled();

    ImGui::Dummy(ImVec2(0, S(4)));
    ImGui::PushFont(g_fontBold);
    ImGui::TextWrapped("%s", t.name);
    ImGui::PopFont();
    HelpText(t.desc);

    // Нижній рядок — притиснутий до низу картки
    float bottomY = ImGui::GetWindowHeight() - S(14) - ImGui::GetTextLineHeight();
    if (ImGui::GetCursorPosY() < bottomY) ImGui::SetCursorPosY(bottomY);
    if (unavailable) {
        ImGui::TextColored(kWarn, "%s", st.unavailable.c_str());
    } else if (pending) {
        ImGui::TextColored(kAccent2, "Застосовую...");
    } else {
        ImGui::TextColored(ImVec4(0.85f, 0.87f, 0.92f, 1), "Коли не варто?");
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(S(320));
            ImGui::TextUnformatted(t.caution);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
        if (t.recommended) {
            const char* rec = "рекомендовано";
            ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(rec).x);
            ImGui::TextColored(kGood, "%s", rec);
        }
    }
    if (unavailable) ImGui::PopStyleVar();
    EndCard();
    ImGui::PopID();
}

static void TweakGrid(App& a, std::function<bool(const tw::Tweak&)> filter) {
    RefreshTweaks(a);
    const auto& all = tw::All();
    float avail = ImGui::GetContentRegionAvail().x;
    float sp = ImGui::GetStyle().ItemSpacing.x;
    int cols = avail > S(860) ? 3 : 2;
    float w = (avail - sp * (cols - 1)) / cols;
    int col = 0;
    for (size_t i = 0; i < all.size(); ++i) {
        if (!filter(all[i])) continue;
        if (col) ImGui::SameLine();
        TweakCard(a, i, w);
        col = (col + 1) % cols;
    }
}

// ---------------------------------------------------------------------------
// Таблиця пінгу
// ---------------------------------------------------------------------------

static int DrawPingTable(App& a) {
    std::lock_guard<std::mutex> l(a.netMutex);
    int bestIdx = -1;
    float bestScore = 1e9f;
    for (int i = 0; i < (int)a.live.size(); ++i)
        if (a.live[i].Score() < bestScore) {
            bestScore = a.live[i].Score();
            bestIdx = i;
        }
    if (ImGui::BeginTable("res", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Сервер", 0, 3.0f);
        ImGui::TableSetupColumn("Мін", 0, 1.0f);
        ImGui::TableSetupColumn("Сер", 0, 1.0f);
        ImGui::TableSetupColumn("Макс", 0, 1.0f);
        ImGui::TableSetupColumn("Джитер", 0, 1.0f);
        ImGui::TableSetupColumn("Втрати", 0, 1.0f);
        ImGui::TableSetupColumn("Графік", 0, 3.0f);
        ImGui::TableHeadersRow();
        for (int i = 0; i < (int)a.live.size(); ++i) {
            auto& lt = a.live[i];
            float mn, avg, mx, jit, loss;
            lt.Stats(mn, avg, mx, jit, loss);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            bool best = i == bestIdx && avg > 0;
            if (best) Dot(kGood);
            ImGui::TextUnformatted(lt.label.empty() ? lt.target.c_str() : lt.label.c_str());
            if (!lt.label.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", lt.target.c_str());
            bool unresolved = lt.done && !lt.result.resolved;
            if (avg <= 0) {
                const char* why = unresolved ? "адресу не знайдено"
                                             : (lt.samples.empty() ? "очікую..." : "немає відповіді");
                for (int c = 0; c < 4; ++c) {
                    ImGui::TableNextColumn();
                    ImGui::TextColored(kMuted, "--");
                }
                ImGui::TableNextColumn();
                if (lt.samples.empty() && !unresolved)
                    ImGui::TextColored(kMuted, "--");
                else
                    ImGui::TextColored(kBad, "100%%");
                ImGui::TableNextColumn();
                ImGui::TextColored(unresolved || !lt.samples.empty() ? kBad : kMuted, "%s", why);
                continue;
            }
            ImVec4 pc = avg < 40 ? kGood : (avg < 90 ? kWarn : kBad);
            ImGui::TableNextColumn();
            ImGui::Text("%.1f", mn);
            ImGui::TableNextColumn();
            ImGui::TextColored(pc, "%.1f", avg);
            ImGui::TableNextColumn();
            ImGui::Text("%.1f", mx);
            ImGui::TableNextColumn();
            ImGui::TextColored(jit < 5 ? kGood : (jit < 15 ? kWarn : kBad), "%.1f", jit);
            ImGui::TableNextColumn();
            ImGui::TextColored(loss < 0.5f ? kGood : (loss < 3 ? kWarn : kBad), "%.0f%%", loss);
            ImGui::TableNextColumn();
            std::vector<float> plot;
            for (float s : lt.samples) plot.push_back(s < 0 ? 0 : s);
            ImGui::PushID(i);
            ImGui::PushStyleColor(ImGuiCol_PlotLines, best ? kGood : kAccent2);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, kPanel2);
            ImGui::PlotLines("##p", plot.data(), (int)plot.size(), 0, nullptr, 0.0f, std::max(10.0f, mx * 1.2f),
                             ImVec2(-1, S(30)));
            ImGui::PopStyleColor(2);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    float mn, avg, mx, jit, loss;
    if (bestIdx >= 0) {
        a.live[bestIdx].Stats(mn, avg, mx, jit, loss);
        if (avg <= 0) bestIdx = -1;
    }
    return bestIdx;
}

static std::string BestPingText(App& a) {
    std::lock_guard<std::mutex> l(a.netMutex);
    float bs = 1e9f;
    std::string best = "--";
    for (auto& lt : a.live) {
        float mn, avg, mx, jit, loss;
        lt.Stats(mn, avg, mx, jit, loss);
        if (avg > 0 && lt.Score() < bs) {
            bs = lt.Score();
            char b[32];
            snprintf(b, sizeof(b), "%.0f мс", avg);
            best = b;
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Сторінки
// ---------------------------------------------------------------------------

static void PageHomeUi(App& a) {
    Title("Головна", KB_ED("Timer Resolution, оптимізація Windows і мережі для Fortnite — в одній програмі.",
                           "Timer Resolution, оптимізація Windows і мережі для ігор — в одній програмі."));

    ULONG coarsest = 0, finest = 0, current = 0;
    lb::TimerQuery(coarsest, finest, current);
    lb::MemInfo mem = lb::GetMemInfo();
    char v1[64], v2[64], v3[64];
    snprintf(v1, sizeof(v1), "%.3f мс", current / 10000.0);
    snprintf(v2, sizeof(v2), "%lu%%", mem.load);
    bool gameOn = a.gameTask.running;
    DWORD pid = a.gamePid;
    snprintf(v3, sizeof(v3), "%s", gameOn ? (pid ? "У ГРІ" : "ЧЕКАЮ") : "ВИМК");
    std::string best = BestPingText(a);

    float avail = ImGui::GetContentRegionAvail().x;
    float sp = ImGui::GetStyle().ItemSpacing.x;
    float w = (avail - sp * 3) / 4;
    ImVec4 resColor = current <= 5100 ? kGood : (current <= 10000 ? kWarn : kBad);
    StatCard("c1", "Роздільність таймера", v1, resColor, current <= 5100 ? "Максимальна точність" : "Стандартна", w);
    ImGui::SameLine();
    char memHint[64];
    snprintf(memHint, sizeof(memHint), "Вільно %.1f з %.1f ГБ", mem.avail / 1073741824.0, mem.total / 1073741824.0);
    StatCard("c2", "Оперативна пам'ять", v2, mem.load < 70 ? kAccent2 : (mem.load < 85 ? kWarn : kBad), memHint, w);
    ImGui::SameLine();
    StatCard("c3", "Буст", v3, gameOn ? kGood : kMuted, gameOn ? a.gameExe : "Не запущено", w);
    ImGui::SameLine();
    StatCard("c4", "Найкращий пінг", best.c_str(), kAccent, best == "--" ? "Перевір сервери" : "Останній тест", w);

    ImGui::Spacing();
    BeginCard("quick", ImVec2(0, 0));
    SectionTitle("Буст для гри");
    HelpText("Програма дочекається запуску гри, триматиме точний таймер, дасть грі високий пріоритет і за "
             "потреби очистить ОЗУ. Вікно можна згорнути.");
    ImGui::Spacing();
    ImGui::SetNextItemWidth(S(340));
    ImGui::BeginDisabled(gameOn);
    ImGui::InputTextWithHint("##qexe", "FortniteClient-Win64-Shipping.exe", a.gameExe, sizeof(a.gameExe));
    ImGui::SameLine();
#if KB_IS_SALE
    if (ImGui::Button(game::DisplayName().c_str())) snprintf(a.gameExe, sizeof(a.gameExe), "%s", game::ExeList().c_str());
#else
    if (ImGui::Button("Fortnite")) snprintf(a.gameExe, sizeof(a.gameExe), "%s", fn::kExe);
#endif
    ImGui::EndDisabled();
    if (ImGui::Checkbox("Згортати KairoBoost, коли гра запущена", &a.minimizeOnGame)) SaveSettings(a);
    ImGui::SameLine(0, S(24));
    if (ImGui::Checkbox("Хрестик ховає програму в трей", &a.closeToTray)) SaveSettings(a);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Закриття вікна не вимикає KairoBoost — він працює біля годинника.\n"
                          "Повністю вийти: правою кнопкою по значку в треї → Вихід.");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Згорнуте вікно не малюється — програма майже не використовує CPU і відеокарту.");
    ImGui::Spacing();
    if (!gameOn) {
        if (AccentButton("БУСТ", ImVec2(S(220), S(48)))) StartGameMode(a);
    } else {
        if (AccentButton("ЗУПИНИТИ", ImVec2(S(220), S(48)), kBad)) StopGameMode(a);
        ImGui::SameLine();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (S(48) - ImGui::GetTextLineHeight()) * 0.5f);
        if (pid)
            ImGui::TextColored(kGood, "  Гра запущена (PID %lu) — буст активний", pid);
        else
            ImGui::TextColored(kWarn, "  Чекаю на запуск %s...", a.gameExe);
    }
    EndCard();

    ImGui::Spacing();
    BeginCard("oneclick", ImVec2(0, 0));
    SectionTitle("Оптимізація в один клік");
    RefreshTweaks(a);
    int total = 0, on = 0;
    const auto& all = tw::All();
    for (size_t i = 0; i < all.size(); ++i) {
        if (!all[i].recommended || !a.tweakState[i].unavailable.empty()) continue;
        ++total;
        if (a.tweakState[i].on) ++on;
    }
    ImGui::Text("Рекомендовані твіки: увімкнено %d з %d", on, total);
    float frac = total ? (float)on / total : 0;
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, on == total ? kGood : kAccent);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, kPanel2);
    ImGui::ProgressBar(frac, ImVec2(-1, S(8)), "");
    ImGui::PopStyleColor(2);
    HelpText(KB_ED("Вмикає лише безпечні твіки з позначкою «рекомендовано» (зокрема для Fortnite). Решта — на сторінці ",
                   "Вмикає лише безпечні твіки з позначкою «рекомендовано» (зокрема для обраної гри). Решта — на сторінці ")
             "«Твіки». Усе можна вимкнути назад.");
    if (ImGui::Button("Порадник для мого ПК", ImVec2(S(220), S(40)))) a.page = PageAdvisor;
    ImGui::SameLine();
    ImGui::BeginDisabled(a.tweakTask.running || on == total);
    if (AccentButton(on == total ? "Усе вже оптимізовано" : "Оптимізувати", ImVec2(S(220), S(40))))
        RunTweakJob(a, "*", [] { tw::ApplyRecommended(true); });
    ImGui::EndDisabled();
    if (a.tweakTask.running) {
        ImGui::SameLine();
        ImGui::TextColored(kAccent2, "Застосовую...");
    }
    EndCard();
}

// ---------------------------------------------------------------------------
// Порадник
// ---------------------------------------------------------------------------

static void StartAnalysis(App& a) {
    a.advStarted = true;
    a.advTask.Start([&a] {
        adv::SystemInfo info = adv::Collect();
        std::vector<adv::Advice> list = adv::Analyze(info);
        int crit = 0;
        for (auto& x : list) crit += x.level == adv::Critical;
        {
            std::lock_guard<std::mutex> l(a.advMutex);
            a.advInfo = info;
            a.advList = list;
            a.advReady = true;
        }
        lb::Log("Порадник: %d порад, з них найважливіших — %d", (int)list.size(), crit);
    });
}

static void CopyToClipboard(const std::string& utf8) {
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, n * sizeof(wchar_t));
    if (!mem) return;
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, (wchar_t*)GlobalLock(mem), n);
    GlobalUnlock(mem);
    if (OpenClipboard(nullptr)) {
        EmptyClipboard();
        SetClipboardData(CF_UNICODETEXT, mem);
        CloseClipboard();
    } else {
        GlobalFree(mem);
    }
}

static ImVec4 LevelColor(adv::Level l) {
    switch (l) {
        case adv::Critical: return kBad;
        case adv::Recommended: return kAccent;
        case adv::Optional: return kAccent2;
        case adv::Good: return kGood;
        default: return kMuted;
    }
}

static Page PageByName(const std::string& n) {
    if (n == "fortnite") return PageFortnite;
    if (n == "ram") return PageRam;
    if (n == "dns") return PageDns;
    if (n == "net") return PageNet;
    if (n == "timer") return PageTimer;
    return PageHome;
}

// Виконати дію поради (у фоні), потім перевірити ПК знову
static void RunAdvice(App& a, const adv::Advice& x) {
    if (x.action == adv::OpenPage) {
        a.page = PageByName(x.page);
        return;
    }
    adv::Advice copy = x;
    RunTweakJob(a, x.tweakId.empty() ? "advice" : x.tweakId, [&a, copy] {
        if (copy.action == adv::EnableTweak || copy.action == adv::DisableTweak) {
            if (const tw::Tweak* t = tw::Find(copy.tweakId)) {
                if (copy.action == adv::EnableTweak) tw::Apply(*t); else tw::Revert(*t);
            }
        } else if (copy.action == adv::FortniteMaxFps) {
            fn::Settings cur = fn::ReadSettings();
            adv::SystemInfo info;
            {
                std::lock_guard<std::mutex> l(a.advMutex);
                info = a.advInfo;
            }
            bool weak = info.physicalCores <= 4 || !info.HasDiscrete();
            int fps = cur.fpsLimit && info.refreshMax && cur.fpsLimit < (int)info.refreshMax ? 0 : cur.fpsLimit;
            fn::WriteSettings(fps, false, cur.performanceMode || weak, true, cur.lowGraphics);
        }
    });
    a.advReady = false;
    a.advStarted = false;  // після завершення дії — новий аналіз
    a.fnTime = -1e9;
}

static void PageAdvisorUi(App& a) {
    Title("Порадник", "Аналізує саме твій ПК і пояснює, що варто ввімкнути, а що ні. Працює без інтернету.");
    if (!a.advStarted && !a.tweakTask.running) StartAnalysis(a);

    std::lock_guard<std::mutex> l(a.advMutex);
    if (!a.advReady) {
        BeginCard("advwait", ImVec2(0, 0));
        ImGui::TextColored(kAccent2, a.tweakTask.running ? "Застосовую..." : "Аналізую ПК...");
        EndCard();
        return;
    }

    // --- Твій ПК ---
    BeginCard("advpc", ImVec2(0, 0));
    SectionTitle("Твій ПК");
    std::string sum = a.advInfo.Summary();
    size_t pos = 0;
    while (pos < sum.size()) {
        size_t e = sum.find('\n', pos);
        std::string line = sum.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
        if (!line.empty()) {
            size_t c = line.find(": ");
            if (c != std::string::npos) {
                ImGui::TextColored(kMuted, "%s", line.substr(0, c + 1).c_str());
                ImGui::SameLine();
                ImGui::TextUnformatted(line.substr(c + 2).c_str());
            } else {
                ImGui::TextUnformatted(line.c_str());
            }
        }
        if (e == std::string::npos) break;
        pos = e + 1;
    }
    ImGui::Spacing();
    ImGui::BeginDisabled(a.advTask.running || a.tweakTask.running);
    if (ImGui::Button("Перевірити знову", ImVec2(S(180), S(34)))) {
        a.advReady = false;
        a.advStarted = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Скопіювати звіт", ImVec2(S(180), S(34)))) {
        std::string rep = "KairoBoost — звіт про ПК\n\n" + sum + "\nПоради:\n";
        for (auto& x : a.advList) rep += std::string("[") + adv::LevelName(x.level) + "] " + x.title + " — " + x.text + "\n";
        CopyToClipboard(rep);
        lb::Log("[OK] Звіт скопійовано в буфер обміну");
    }
    ImGui::SameLine();
    int todo = 0;
    for (auto& x : a.advList)
        if ((x.level == adv::Critical || x.level == adv::Recommended) &&
            (x.action == adv::EnableTweak || x.action == adv::FortniteMaxFps))
            ++todo;
    ImGui::BeginDisabled(todo == 0);
    char btn[64];
    snprintf(btn, sizeof(btn), todo ? "Застосувати важливе (%d)" : "Усе важливе застосовано", todo);
    if (AccentButton(btn, ImVec2(S(260), S(34)))) {
        std::vector<adv::Advice> list;
        for (auto& x : a.advList)
            if ((x.level == adv::Critical || x.level == adv::Recommended) &&
                (x.action == adv::EnableTweak || x.action == adv::FortniteMaxFps))
                list.push_back(x);
        RunTweakJob(a, "advice", [list] {
            for (auto& x : list) {
                if (x.action == adv::EnableTweak) {
                    if (const tw::Tweak* t = tw::Find(x.tweakId)) tw::Apply(*t);
                } else {
                    fn::Settings cur = fn::ReadSettings();
                    fn::WriteSettings(cur.fpsLimit, false, true, true, cur.lowGraphics);
                }
            }
        });
        a.advReady = false;
        a.advStarted = false;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Вмикає всі поради «Найважливіше» і «Рекомендовано», які можна застосувати автоматично.");
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    EndCard();

    // --- Поради за групами ---
    for (int lvl = adv::Critical; lvl <= adv::NotForYou; ++lvl) {
        bool any = false;
        for (auto& x : a.advList) any |= x.level == lvl;
        if (!any) continue;
        ImGui::Spacing();
        ImGui::PushID(lvl);
        BeginCard("lvl", ImVec2(0, 0));
        ImGui::PushFont(g_fontTitle);
        ImGui::TextColored(LevelColor((adv::Level)lvl), "%s", adv::LevelName((adv::Level)lvl));
        ImGui::PopFont();
        int k = 0;
        for (size_t i = 0; i < a.advList.size(); ++i) {
            const adv::Advice& x = a.advList[i];
            if (x.level != lvl) continue;
            ImGui::PushID((int)i);
            if (k++) ImGui::Separator();
            float right = ImGui::GetContentRegionMax().x;
            bool hasBtn = x.action != adv::None && lvl != adv::Good && lvl != adv::NotForYou;
            ImGui::BeginGroup();
            Dot(LevelColor((adv::Level)lvl));
            ImGui::PushFont(g_fontBold);
            ImGui::TextUnformatted(x.title.c_str());
            ImGui::PopFont();
            ImGui::PushTextWrapPos(hasBtn ? right - S(170) : right);
            ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
            ImGui::TextWrapped("%s", x.text.c_str());
            ImGui::PopStyleColor();
            ImGui::PopTextWrapPos();
            ImGui::EndGroup();
            if (hasBtn) {
                ImGui::SameLine(right - S(150));
                const char* label = x.action == adv::EnableTweak    ? "Увімкнути"
                                    : x.action == adv::DisableTweak ? "Вимкнути"
                                    : x.action == adv::OpenPage     ? "Відкрити"
                                                                    : "Налаштувати";
                ImGui::BeginDisabled(a.tweakTask.running);
                bool click = lvl == adv::Critical ? AccentButton(label, ImVec2(S(150), S(34)), kBad)
                                                  : ImGui::Button(label, ImVec2(S(150), S(34)));
                ImGui::EndDisabled();
                if (click) {
                    adv::Advice copy = x;
                    ImGui::PopID();
                    EndCard();
                    ImGui::PopID();
                    // Мютекс тримаємо — RunAdvice його не чіпає синхронно
                    RunAdvice(a, copy);
                    return;
                }
            }
            ImGui::PopID();
        }
        EndCard();
        ImGui::PopID();
    }
}

// Вибір гри (редакція для продажу): застосувати до модуля game і оновити залежні частини
static void ApplyGame(App& a, const std::string& id, bool loadPath) {
    if (!game::Select(id)) return;
    a.gameId = id;
    game::SetCustomExe(a.customExe);
    if (loadPath) {
        // шлях для цієї гри — з налаштувань
        std::string f = lb::GetEnv("APPDATA") + "\\KairoBoost\\settings.ini";
        char buf[MAX_PATH] = "";
        GetPrivateProfileStringA("KairoBoost", ("GamePath_" + id).c_str(), "", buf, sizeof(buf), f.c_str());
        snprintf(a.gamePath, sizeof(a.gamePath), "%s", buf);
    }
    game::SetManualPath(a.gamePath);
    std::string list = game::ExeList();
    if (!list.empty()) snprintf(a.gameExe, sizeof(a.gameExe), "%s", list.c_str());
    a.tweakStateTime = -1e9;
    a.fnTime = -1e9;
    a.advStarted = false;
    a.advReady = false;
}

static std::string BrowseExe(HWND owner) {
    char file[MAX_PATH] = "";
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = "Програми (*.exe)\0*.exe\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = sizeof(file);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameA(&ofn) ? std::string(file) : std::string();
}

static void GamePickerCard(App& a) {
    BeginCard("gamepick", ImVec2(0, 0));
    SectionTitle("Гра");
    const auto& ps = game::Profiles();
    int cur = 0;
    for (int i = 0; i < (int)ps.size(); ++i)
        if (a.gameId == ps[i].id) cur = i;
    ImGui::SetNextItemWidth(S(320));
    if (ImGui::BeginCombo("##game", ps[cur].name)) {
        for (int i = 0; i < (int)ps.size(); ++i)
            if (ImGui::Selectable(ps[i].name, i == cur)) {
                ApplyGame(a, ps[i].id, true);
                SaveSettings(a);
            }
        ImGui::EndCombo();
    }
    if (a.gameId == "custom") {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(S(300));
        if (ImGui::InputTextWithHint("##cexe", "game.exe або повний шлях", a.customExe, sizeof(a.customExe)))
            game::SetCustomExe(a.customExe);
    }
    ImGui::Spacing();
    if (game::CurrentExes().empty()) {
        Dot(kWarn);
        ImGui::TextColored(kWarn, "Вкажи exe своєї гри (кнопка «Огляд…» або введи ім'я, напр. game.exe)");
    } else if (!a.fnExe.empty()) {
        Dot(kGood);
        ImGui::TextColored(kGood, "Гру знайдено");
        ImGui::SameLine();
        if (a.fnRunning) Pill("гра запущена", kAccent2);
        HelpText(a.fnExe.c_str());
    } else {
        Dot(kWarn);
        ImGui::TextColored(kWarn, "Шлях до гри не знайдено автоматично");
        HelpText("Запусти гру — KairoBoost знайде її сам, або вкажи exe кнопкою «Огляд…». Без шляху працюють пріоритет, "
                 "QoS і режим гри; для вибору відеокарти потрібен шлях.");
    }
    ImGui::SetNextItemWidth(S(420));
    ImGui::InputTextWithHint("##gpath", "Шлях до exe гри (необов'язково)", a.gamePath, sizeof(a.gamePath));
    ImGui::SameLine();
    if (ImGui::Button("Огляд…")) {
        std::string f = BrowseExe(GetActiveWindow());
        if (!f.empty()) {
            snprintf(a.gamePath, sizeof(a.gamePath), "%s", f.c_str());
            if (a.gameId == "custom") snprintf(a.customExe, sizeof(a.customExe), "%s", f.c_str());
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Зберегти")) {
        if (a.gameId == "custom") game::SetCustomExe(a.customExe);
        game::SetManualPath(a.gamePath);
        ApplyGame(a, a.gameId, false);
        SaveSettings(a);
    }
    EndCard();
}

static void PageFortniteUi(App& a) {
#if KB_IS_SALE
    Title("Гра", "Налаштування Windows і самої гри під обрану гру.");
#else
    Title("Fortnite", "Налаштування Windows і самої гри саме під Fortnite.");
#endif
    if (a.onWifi) {
        InfoBox("Ти граєш через Wi-Fi. Найбільший виграш у пінгу, джитері та втратах дає кабель (Ethernet): "
                "Wi-Fi додає кілька-десятки мілісекунд і стрибки пінгу, які не прибере жодна програма.", kWarn);
        ImGui::Spacing();
    }
    if (ImGui::GetTime() - a.fnTime > (a.lowPower ? 30.0 : 2.0)) {
        a.fnExe = game::CurrentPath();
        a.fnRunning = game::IsRunning();
        a.onWifi = dns::OnWifi();
        a.fnSettings = fn::ReadSettings();
        if (!a.fnUiLoaded && a.fnSettings.found) {
            a.fnVsync = a.fnSettings.vsync;
            a.fnPerf = a.fnSettings.performanceMode;
            a.fnFullscreen = a.fnSettings.fullscreen;
            a.fnLowGfx = a.fnSettings.lowGraphics;
            a.fnFpsIdx = 0;
            for (int i = 0; i < 7; ++i)
                if (kFpsOptions[i] == a.fnSettings.fpsLimit) a.fnFpsIdx = i;
            a.fnUiLoaded = true;
        }
        a.fnTime = ImGui::GetTime();
    }

    // --- Гра ---
#if KB_IS_SALE
    GamePickerCard(a);  // у версії для продажу шлях до гри задається тут (і для Fortnite теж)
#else
    BeginCard("fngame", ImVec2(0, 0));
    if (!a.fnExe.empty()) {
        Dot(kGood);
        ImGui::TextColored(kGood, "Fortnite знайдено");
        ImGui::SameLine();
        if (a.fnRunning) Pill("гра запущена", kAccent2);
        HelpText(a.fnExe.c_str());
    } else {
        Dot(kWarn);
        ImGui::TextColored(kWarn, "Fortnite не знайдено автоматично");
        HelpText("Вкажи папку гри (наприклад C:\\Program Files\\Epic Games\\Fortnite).");
    }
    ImGui::SetNextItemWidth(S(420));
    ImGui::InputTextWithHint("##fndir", "Папка Fortnite (необов'язково)", a.fnDir, sizeof(a.fnDir));
    ImGui::SameLine();
    if (ImGui::Button("Зберегти")) {
        fn::SetManualDir(a.fnDir);
        SaveSettings(a);
        a.fnTime = -1e9;
        a.tweakStateTime = -1e9;
    }
    EndCard();
#endif

    // --- Твіки Fortnite ---
    ImGui::Spacing();
    SectionTitle((std::string("Твіки для ") + game::DisplayName()).c_str());
    TweakGrid(a, [](const tw::Tweak& t) { return t.category == tw::Fortnite; });

    if (!game::IsFortnite()) {
        // Для інших ігор — що налаштувати в самій грі
        ImGui::Spacing();
        BeginCard("gametips", ImVec2(0, 0));
        SectionTitle("Налаштування в самій грі");
        ImGui::TextWrapped("%s", game::Current().tip);
        ImGui::Spacing();
        HelpText("Кабель замість Wi-Fi, частота опитування миші 1000 Гц+, свіжий драйвер відеокарти — "
                 "дають ефект у будь-якій грі.");
        EndCard();
        return;
    }

    // --- Налаштування гри ---
    ImGui::Spacing();
    BeginCard("fnset", ImVec2(0, 0));
    SectionTitle("Налаштування гри");
    if (!a.fnSettings.found) {
        HelpText("Файл налаштувань не знайдено. Запусти Fortnite хоча б раз і закрий гру.");
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
        ImGui::Text("Зараз у файлі: FPS %s · VSync %s · %s", a.fnSettings.fpsLimit ? std::to_string(a.fnSettings.fpsLimit).c_str() : "без обмеження",
                    a.fnSettings.vsync ? "увімк" : "вимк",
                    a.fnSettings.performanceMode ? "режим продуктивності" : (a.fnSettings.rhi.empty() ? "DirectX" : a.fnSettings.rhi.c_str()));
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::BeginDisabled(a.fnRunning);
        ImGui::TextUnformatted("Обмеження FPS");
        ImGui::SameLine(S(200));
        ImGui::SetNextItemWidth(S(180));
        ImGui::Combo("##fps", &a.fnFpsIdx, kFpsNames, 7);
        ImGui::Checkbox("Вертикальна синхронізація (VSync)", &a.fnVsync);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Вимкни для мінімальної затримки вводу.");
        ImGui::Checkbox("Режим продуктивності (Performance Mode)", &a.fnPerf);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Найбільший приріст FPS, особливо на слабких ПК і в будівництві.");
        ImGui::Checkbox("Повноекранний режим (найменша затримка вводу)", &a.fnFullscreen);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("«Вікно без рамки» додає затримку через композитор Windows. Повноекранний режим — ні.");
        ImGui::Checkbox("Мінімальна графіка: тіні, ефекти, постобробка, згладжування", &a.fnLowGfx);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Дальність огляду і текстури не змінюються — вони важливі для гри і майже не впливають на FPS.");
        HelpText("Для максимального FPS і мінімальної затримки: режим продуктивності, повноекранний режим, VSync "
                 "вимкнено, мінімальна графіка, FPS — без обмеження або трохи вище частоти монітора.");
        if (ImGui::Button("Обрати все для макс. FPS", ImVec2(S(240), S(32)))) {
            a.fnVsync = false;
            a.fnPerf = true;
            a.fnFullscreen = true;
            a.fnLowGfx = true;
            a.fnFpsIdx = 0;
        }
        if (AccentButton("Записати налаштування", ImVec2(S(240), S(38)))) {
            int fps = kFpsOptions[a.fnFpsIdx];
            bool vs = a.fnVsync, perf = a.fnPerf, full = a.fnFullscreen, low = a.fnLowGfx;
            RunTweakJob(a, "fn_settings", [fps, vs, perf, full, low] { fn::WriteSettings(fps, vs, perf, full, low); });
            a.fnTime = -1e9;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!fn::HasSettingsBackup());
        if (ImGui::Button("Повернути як було", ImVec2(S(200), S(38)))) {
            fn::RestoreSettingsBackup();
            a.fnUiLoaded = false;
            a.fnTime = -1e9;
        }
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        if (a.fnRunning) ImGui::TextColored(kWarn, "Закрий Fortnite, щоб змінити налаштування (гра перезаписує файл при виході).");
    }
    EndCard();

    // --- Сервери ---
    ImGui::Spacing();
    BeginCard("fnsrv", ImVec2(0, 0));
    SectionTitle("Сервери Fortnite");
    HelpText("Пінг до дата-центрів Epic Games. Обери найкращий регіон у грі: Налаштування → Гра → "
             "Регіон підбору гравців (Matchmaking Region).");
    bool running = a.netTask.running;
    if (!running) {
        if (AccentButton("Перевірити регіони", ImVec2(S(220), S(38)))) StartFortnitePing(a);
    } else {
        if (AccentButton("Стоп", ImVec2(S(220), S(38)), kBad)) a.netTask.cancel = true;
    }
    bool isFn;
    {
        std::lock_guard<std::mutex> l(a.netMutex);
        isFn = a.liveIsFortnite && !a.live.empty();
    }
    if (isFn) {
        ImGui::Spacing();
        int best = DrawPingTable(a);
        if (best >= 0) {
            std::lock_guard<std::mutex> l(a.netMutex);
            std::string msg = "Найкращий регіон: " + a.live[best].label;
            InfoBox(msg.c_str(), kGood);
        }
    }
    EndCard();

    // --- Обслуговування ---
    ImGui::Spacing();
    BeginCard("fnmaint", ImVec2(0, 0));
    SectionTitle("Обслуговування");
    ImGui::BeginDisabled(a.fnRunning || a.tweakTask.running);
    if (ImGui::Button("Очистити логи і звіти про збої", ImVec2(S(300), S(36))))
        RunTweakJob(a, "fn_logs", [] { fn::CleanLogs(); });
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Буст для Fortnite", ImVec2(S(220), S(36)))) {
        snprintf(a.gameExe, sizeof(a.gameExe), "%s", fn::kExe);
        StartGameMode(a);
        a.page = PageHome;
    }
    HelpText("Логи і звіти про збої з часом займають місце; гра створить нові за потреби.");
    EndCard();

    // --- Що ще зробити ---
    ImGui::Spacing();
    BeginCard("fntips", ImVec2(0, 0));
    SectionTitle("Що ще дає найбільший ефект");
    HelpText("Цього програма змінити не може — тільки ти в грі або в налаштуваннях заліза:");
    const char* tips[] = {
        "У грі: Відео → NVIDIA Reflex Low Latency → «Увімк. + прискорення» (на картах NVIDIA) — мінус 10-30 мс "
        "затримки вводу.",
        "У грі: Відео → Режим відображення — «Повноекранний» (або «Вікно без рамки» з твіком «Оптимізації для "
        "віконних ігор»).",
        "Кабель замість Wi-Fi — найбільший виграш у пінгу і стабільності.",
        "Миша: частота опитування 1000 Гц або вище (у програмі виробника миші).",
        "Драйвер відеокарти — останньої версії; у панелі NVIDIA/AMD: режим живлення «Максимальна продуктивність».",
        "Регіон підбору гравців — з найменшим пінгом (кнопка «Перевірити регіони» вище).",
    };
    for (auto* t : tips) {
        Dot(kAccent2);
        ImGui::TextWrapped("%s", t);
    }
    EndCard();
}

// Налаштування повтору клавіш (FilterKeys) — повзунки
static void KeyRepeatPanel(App& a) {
    const tw::Tweak* kt = tw::Find("keyboard");
    if (!kt) return;
    size_t idx = (size_t)(kt - &tw::All()[0]);
    bool on = idx < a.tweakState.size() && a.tweakState[idx].on;
    bool busy = a.tweakTask.running;

    BeginCard("kbpanel", ImVec2(0, 0));
    SectionTitle("Повтор клавіш");
    ImGui::SameLine();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(4));
    if (on) {
        int d, i;
        if (tw::ReadSystemKeyRepeat(d, i))
            ImGui::TextColored(kGood, "  зараз: %d мс / %d мс", d, i);
    } else {
        ImGui::TextColored(kMuted, "  зараз: стандартні налаштування Windows");
    }
    HelpText("Як FilterKeysSetter: «Фільтрація введення» Windows з власними інтервалами (натискання не "
             "фільтруються). Затисни клавішу в полі нижче, щоб перевірити.");
    ImGui::Spacing();

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Затримка перед повтором");
    ImGui::SameLine(S(260));
    ImGui::SetNextItemWidth(S(300));
    ImGui::SliderInt("##kbdelay", &a.kbDelay, 100, 1000, "%d мс");
    if (a.kbDelay < 150) {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(kWarn, "може дублювати символи");
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Інтервал повтору");
    ImGui::SameLine(S(260));
    ImGui::SetNextItemWidth(S(300));
    char fmt[64];
    snprintf(fmt, sizeof(fmt), "%%d мс  (~%d разів/с)", 1000 / std::max(1, a.kbInterval));
    ImGui::SliderInt("##kbint", &a.kbInterval, 5, 100, fmt);
    if (a.kbInterval < 16)
        HelpText("Інтервал менше 16 мс може обмежуватися системним таймером Windows (крок 15.6 мс) — найточніше "
                 "працює, поки KairoBoost тримає Timer Resolution (режим гри або сторінка Timer Resolution).");
    ImGui::TextColored(kMuted, "Пресети:");
    struct Preset { const char* name; int d, i; };
    static const Preset presets[] = {{"Швидко", 250, 20}, {"Дуже швидко", 200, 15}, {"Максимум", 150, 10}, {"Екстрим", 100, 5}};
    for (auto& p : presets) {
        ImGui::SameLine();
        if (ImGui::SmallButton(p.name)) {
            a.kbDelay = p.d;
            a.kbInterval = p.i;
        }
    }
    ImGui::Spacing();
    ImGui::BeginDisabled(busy);
    if (AccentButton(on ? "Застосувати значення" : "Увімкнути з цими значеннями", ImVec2(S(280), S(38)))) {
        tw::SetKeyRepeat(a.kbDelay, a.kbInterval);
        SaveSettings(a);
        RunTweakJob(a, "keyboard", [idx] { tw::Apply(tw::All()[idx]); });
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!on);
    if (ImGui::Button("Стандарт Windows", ImVec2(S(200), S(38))))
        RunTweakJob(a, "keyboard", [idx] { tw::Revert(tw::All()[idx]); });
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    ImGui::Spacing();
    ImGui::InputTextMultiline("##kbtest", a.kbTest, sizeof(a.kbTest), ImVec2(-1, S(56)));
    if (!a.kbTest[0] && !ImGui::IsItemActive()) {
        ImVec2 r = ImGui::GetItemRectMin();
        ImGui::GetWindowDrawList()->AddText(ImVec2(r.x + S(10), r.y + S(8)), ImGui::GetColorU32(kMuted),
                                            "Тестове поле: клацни і затисни будь-яку клавішу");
    }
    EndCard();
}

static void PageTweaksUi(App& a) {
    Title("Твіки системи",
          "Кожен твік вмикається окремо. Вимкнення повертає саме ті значення, що були до KairoBoost.");
    // Фільтри
    const char* allLabel = "Усі";
    auto chip = [&](const char* label, int value) {
        bool sel = a.tweakFilter == value;
        ImGui::PushStyleColor(ImGuiCol_Button, sel ? Fade(kAccent, 0.25f) : kPanel);
        ImGui::PushStyleColor(ImGuiCol_Text, sel ? kAccent : ImVec4(0.85f, 0.87f, 0.92f, 1));
        if (ImGui::Button(label)) a.tweakFilter = value;
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
    };
    chip(allLabel, -1);
    for (int c = 0; c < tw::Fortnite; ++c) chip(tw::CategoryName((tw::Category)c), c);
    ImGui::NewLine();
    ImGui::Spacing();

    ImGui::BeginDisabled(a.tweakTask.running);
    if (AccentButton("Увімкнути рекомендовані", ImVec2(S(240), S(36))))
        RunTweakJob(a, "*", [] { tw::ApplyRecommended(false); });
    ImGui::SameLine();
    if (AccentButton("Відкотити все", ImVec2(S(170), S(36)), kBad)) a.confirmRevert = true;
    ImGui::SameLine();
    if (ImGui::Button("Очистити DNS-кеш", ImVec2(S(180), S(36)))) RunTweakJob(a, "dns", [] { lb::FlushDns(); });
    ImGui::EndDisabled();
    ImGui::Spacing();

    int f = a.tweakFilter;
    TweakGrid(a, [f](const tw::Tweak& t) { return t.category != tw::Fortnite && (f < 0 || t.category == f); });
    if (f < 0 || f == tw::Input) {
        ImGui::Spacing();
        KeyRepeatPanel(a);
    }

    if (a.confirmRevert) {
        ImGui::OpenPopup("Відкотити все?");
        a.confirmRevert = false;
    }
    if (ImGui::BeginPopupModal("Відкотити все?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Усі зміни KairoBoost (твіки, QoS, налаштування Fortnite) буде повернуто\n"
                               "до оригінальних значень.");
        ImGui::Spacing();
        if (AccentButton("Так, відкотити", ImVec2(S(170), 0), kBad)) {
            RunTweakJob(a, "*", [] { tw::RevertEverything(); });
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Скасувати", ImVec2(S(170), 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

static void PageTimerUi(App& a) {
    Title("Timer Resolution",
          "Зменшує крок системного таймера Windows (стандартно 15.6 мс). Точніший таймер — рівніший frametime "
          "і менша затримка вводу.");
    bool busy = a.gameTask.running || a.timerTask.running;

    ULONG coarsest = 0, finest = 0, current = 0;
    lb::TimerQuery(coarsest, finest, current);
    ULONG held = lb::TimerHeld();

    BeginCard("tstate", ImVec2(0, 0));
    ImGui::PushFont(g_fontBig);
    ImGui::TextColored(current <= 5100 ? kGood : kAccent, "%.4f мс", current / 10000.0);
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::Text("Діапазон: %.4f ... %.4f мс", finest / 10000.0, coarsest / 10000.0);
    if (held)
        ImGui::Text("KairoBoost тримає: %.4f мс", held / 10000.0);
    else
        ImGui::TextUnformatted("KairoBoost зараз нічого не тримає");
    ImGui::PopStyleColor();
    EndCard();

    ImGui::Spacing();
    BeginCard("tctl", ImVec2(0, 0));
    if (a.gameTask.running) ImGui::TextColored(kWarn, "Таймером зараз керує режим гри.");
    ImGui::BeginDisabled(busy);
    ImGui::SetNextItemWidth(S(320));
    ImGui::SliderFloat("##res", &a.resMs, 0.5f, 2.0f, "%.3f мс");
    ImGui::SameLine();
    if (AccentButton("Встановити", ImVec2(S(130), 0))) {
        ULONG actual = 0;
        if (lb::TimerHold((ULONG)std::lround(a.resMs * 10000.0), &actual))
            lb::Log("Timer Resolution: %.4f мс", actual / 10000.0);
        else
            lb::Log("[!] Не вдалося встановити Timer Resolution");
    }
    ImGui::SameLine();
    if (ImGui::Button("Відпустити", ImVec2(S(130), 0))) {
        lb::TimerRelease();
        lb::Log("Timer Resolution відпущено");
    }
    ImGui::Spacing();
    if (ImGui::Button("Авто-підбір", ImVec2(S(200), S(36)))) {
        a.timerTask.Start([&a] {
            ULONG best = lb::AutoTuneResolution(&a.timerTask.cancel);
            a.resMs = best / 10000.0f;
            a.gameRes = a.resMs;
        });
    }
    ImGui::SameLine();
    if (ImGui::Button("Тест Sleep(1)", ImVec2(S(200), S(36)))) RunBenchmark(a);
    ImGui::EndDisabled();
    if (a.timerTask.running) {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(kAccent2, "Працюю...");
    }
    HelpText("Авто-підбір шукає значення, при якому Sleep(1) найближчий до 1 мс (зазвичай 0.50-0.51 мс), і "
             "використовує його в режимі гри.");
    EndCard();

    ImGui::Spacing();
    BeginCard("tplot", ImVec2(0, 0));
    ImGui::TextUnformatted("Тривалість Sleep(1), мс");
    {
        std::lock_guard<std::mutex> l(a.sleepMutex);
        if (a.hasSleep) {
            ImGui::SameLine();
            ImGui::TextColored(kAccent2, "  сер %.3f   мін %.3f   макс %.3f", a.sleepStats.avg, a.sleepStats.min,
                               a.sleepStats.max);
            float top = std::max(2.0f, (float)a.sleepStats.max * 1.1f);
            ImGui::PushStyleColor(ImGuiCol_PlotLines, kAccent2);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, kPanel2);
            ImGui::PlotLines("##sleep", a.sleepSamples.data(), (int)a.sleepSamples.size(), 0, nullptr, 0.0f, top,
                             ImVec2(-1, S(110)));
            ImGui::PopStyleColor(2);
        } else {
            HelpText("Натисни «Тест Sleep(1)», щоб побачити реальну точність таймера. Без програми зазвичай "
                     "~15.6 мс або ~2 мс, з 0.5 мс — близько 1 мс.");
        }
    }
    EndCard();

    ImGui::Spacing();
    SectionTitle("Пов'язані твіки");
    TweakGrid(a, [](const tw::Tweak& t) { return std::string(t.id) == "globaltimer"; });
}

static void PageRamUi(App& a) {
    Title("Очистка ОЗУ",
          "Звільняє оперативну пам'ять: скидає неактивні сторінки програм і очищає кеш standby, який іноді "
          "не віддається грі й спричиняє фризи.");
    lb::MemInfo m = lb::GetMemInfo();
    double used = (m.total - m.avail) / 1073741824.0, freeG = m.avail / 1073741824.0, total = m.total / 1073741824.0;

    BeginCard("ram", ImVec2(0, 0));
    SectionTitle("Пам'ять");
    ImGui::SameLine();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(4));
    ImGui::TextColored(kMuted, "  Остання очистка: %s", a.ramLastText.c_str());
    ImGui::SameLine(ImGui::GetContentRegionMax().x - S(150));
    ImGui::BeginDisabled(a.ramTask.running);
    if (AccentButton(a.ramTask.running ? "Очищаю..." : "Очистити", ImVec2(S(150), S(38)), kBad)) StartRamClean(a);
    ImGui::EndDisabled();
    ImGui::Spacing();

    // Смуга використання
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, h = S(18);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(kPanel2), S(5));
    float frac = m.total ? (float)((m.total - m.avail) / (double)m.total) : 0;
    ImVec4 bar = m.load < 70 ? kAccent2 : (m.load < 85 ? kWarn : kBad);
    dl->AddRectFilled(p, ImVec2(p.x + w * frac, p.y + h), ImGui::GetColorU32(bar), S(5));
    ImGui::Dummy(ImVec2(w, h));
    Dot(bar);
    ImGui::Text("Використано %.1f ГБ", used);
    ImGui::SameLine(0, S(24));
    Dot(kPanel2);
    ImGui::Text("Вільно %.1f ГБ", freeG);
    ImGui::SameLine(ImGui::GetContentRegionMax().x - S(110));
    ImGui::TextColored(kMuted, "%.1f ГБ всього", total);
    LONGLONG freed = a.ramLastFreed;
    if (freed > 0) ImGui::TextColored(kGood, "Минулого разу звільнено %.0f МБ", freed / 1048576.0);
    EndCard();

    ImGui::Spacing();
    float avail = ImGui::GetContentRegionAvail().x, sp = ImGui::GetStyle().ItemSpacing.x;
    float half = (avail - sp) / 2;

    BeginCard("ramauto", ImVec2(half, S(320)));
    SectionTitle("Розклад");
    ImGui::SameLine(ImGui::GetContentRegionMax().x - S(46));
    if (Toggle("##ramauto", a.ramAuto, false)) {
        a.ramAuto = !a.ramAuto;
        SaveSettings(a);
    }
    ImGui::TextColored(kMuted, "Автоматична очистка");
    ImGui::Spacing();
    ImGui::BeginDisabled(!a.ramAuto);
    ImGui::TextUnformatted("Запускати, коли:");
    bool changed = false;
    changed |= ImGui::RadioButton("запускається гра (режим гри / БУСТ)", &a.ramMode, 0);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Звільняє пам'ять фонових програм. Кеш файлів гри не чіпається,\n"
                          "щоб не сповільнити завантаження матчу.");
    changed |= ImGui::RadioButton("вільної пам'яті менше ніж", &a.ramMode, 1);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(S(110));
    changed |= ImGui::SliderInt("##pct", &a.ramFreePct, 5, 50, "%d%%");
    changed |= ImGui::RadioButton("за розкладом", &a.ramMode, 2);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(S(180));
    changed |= ImGui::Combo("##hours", &a.ramHoursIdx, kHoursNames, 5);
    ImGui::EndDisabled();
    if (changed) SaveSettings(a);
    ImGui::Spacing();
    InfoBox("Очистка швидка, але після неї ПК може на мить сповільнитися. Пам'ять гри не чіпаємо.");
    EndCard();

    ImGui::SameLine();
    BeginCard("ramex", ImVec2(half, S(320)));
    SectionTitle("Винятки");
    HelpText("Пам'ять цих програм не очищається.");
    ImGui::SetNextItemWidth(-S(110));
    bool enter = ImGui::InputTextWithHint("##newex", "program.exe", a.ramNewExclude, sizeof(a.ramNewExclude),
                                          ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button("Додати", ImVec2(-1, 0)) || enter) && a.ramNewExclude[0]) {
        a.ramExclude.push_back(a.ramNewExclude);
        a.ramNewExclude[0] = 0;
        SaveSettings(a);
    }
    ImGui::BeginChild("exlist", ImVec2(0, 0));
    for (size_t i = 0; i < a.ramExclude.size(); ++i) {
        ImGui::PushID((int)i);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(a.ramExclude[i].c_str());
        ImGui::SameLine(ImGui::GetContentRegionMax().x - S(30));
        if (ImGui::SmallButton("x")) {
            a.ramExclude.erase(a.ramExclude.begin() + i);
            SaveSettings(a);
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    EndCard();
}

static void PageNetUi(App& a) {
    Title("Тест мережі",
          "Пінг, джитер і втрати до кількох серверів одночасно. Адреса — ICMP-пінг, адреса:порт — TCP-пінг "
          "(для серверів, що не відповідають на звичайний пінг).");
    bool running = a.netTask.running;
    BeginCard("nctl", ImVec2(0, 0));
    ImGui::BeginDisabled(running);
    ImGui::SetNextItemWidth(-S(330));
    ImGui::InputTextWithHint("##targets", "1.1.1.1 8.8.8.8 server.com:443", a.targets, sizeof(a.targets));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(S(160));
    ImGui::SliderInt("##count", &a.pingCount, 5, 200, "%d запитів");
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (!running) {
        if (AccentButton("Старт", ImVec2(-1, 0))) StartPing(a);
    } else {
        if (AccentButton("Стоп", ImVec2(-1, 0), kBad)) a.netTask.cancel = true;
    }
    ImGui::TextColored(kMuted, "Швидкий вибір:");
    ImGui::SameLine();
    ImGui::BeginDisabled(running);
    if (ImGui::SmallButton("Сервери Fortnite")) StartFortnitePing(a);
    ImGui::SameLine();
    if (ImGui::SmallButton("DNS")) snprintf(a.targets, sizeof(a.targets), "1.1.1.1 8.8.8.8 9.9.9.9");
    ImGui::SameLine();
    if (ImGui::SmallButton("Epic Games")) snprintf(a.targets, sizeof(a.targets), "epicgames.com:443 fortnite.com:443");
    ImGui::SameLine();
    if (ImGui::SmallButton("Discord")) snprintf(a.targets, sizeof(a.targets), "discord.com:443 gateway.discord.gg:443");
    ImGui::EndDisabled();
    EndCard();

    ImGui::Spacing();
    bool empty;
    {
        std::lock_guard<std::mutex> l(a.netMutex);
        empty = a.live.empty();
    }
    if (empty) {
        HelpText("Результати з'являться тут.");
        return;
    }
    BeginCard("ntable", ImVec2(0, 0));
    DrawPingTable(a);
    HelpText("Пінг у мс. Зелена крапка — найкращий сервер (пінг + 2×джитер + штраф за втрати).");
    EndCard();
}

// ---------------------------------------------------------------------------
// DNS
// ---------------------------------------------------------------------------

static void RefreshDnsInfo(App& a, bool force = false) {
    if (!force && ImGui::GetTime() - a.dnsInfoTime < 3.0) return;
    a.dnsCurrent = dns::CurrentProvider();
    auto cur = dns::CurrentServers();
    a.dnsCurrentText.clear();
    for (auto& c : cur) a.dnsCurrentText += (a.dnsCurrentText.empty() ? "" : " / ") + c;
    a.dnsChanged = dns::IsChanged();
    a.dnsInfoTime = ImGui::GetTime();
}

static void StartDnsTest(App& a) {
    if (a.dnsTask.running) return;
    RefreshDnsInfo(a, true);
    {
        std::lock_guard<std::mutex> l(a.dnsMutex);
        a.dnsRows.clear();
        const auto& ps = dns::Providers();
        for (int i = 0; i < (int)ps.size(); ++i) {
            DnsRow r;
            r.provider = i;
            r.name = ps[i].name;
            r.note = ps[i].note;
            r.servers = std::string(ps[i].v4[0]) + " / " + ps[i].v4[1];
            r.ip = ps[i].v4[0];
            a.dnsRows.push_back(r);
        }
        // DNS від провайдера, якщо зараз стоїть не один із відомих
        auto cur = dns::CurrentServers();
        if (a.dnsCurrent < 0 && !cur.empty()) {
            DnsRow r;
            r.name = "Провайдер / роутер";
            r.note = "DNS, який зараз видає твоя мережа";
            r.servers = a.dnsCurrentText;
            r.ip = cur[0];
            a.dnsRows.push_back(r);
        }
    }
    a.dnsTested = true;
    a.dnsTask.Start([&a] {
        size_t n;
        std::vector<std::string> ips;
        {
            std::lock_guard<std::mutex> l(a.dnsMutex);
            n = a.dnsRows.size();
            for (auto& r : a.dnsRows) ips.push_back(r.ip);
        }
        lb::Log("Тест DNS: %d серверів", (int)n);
        a.dnsIntercepted = dns::InterceptionDetected();
        if (a.dnsIntercepted) lb::Log("[!] DNS-запити перехоплюються мережею — результати тесту недостовірні");
        std::vector<std::thread> w;
        for (size_t i = 0; i < n; ++i)
            w.emplace_back([&a, i, ip = ips[i]] {
                dns::Result r = dns::Measure(ip, 16, &a.dnsTask.cancel, [&a, i](double ms) {
                    std::lock_guard<std::mutex> l(a.dnsMutex);
                    a.dnsRows[i].samples.push_back((float)ms);
                });
                std::lock_guard<std::mutex> l(a.dnsMutex);
                a.dnsRows[i].res = r;
                a.dnsRows[i].done = true;
            });
        for (auto& t : w) t.join();
        std::lock_guard<std::mutex> l(a.dnsMutex);
        int best = -1;
        for (int i = 0; i < (int)a.dnsRows.size(); ++i)
            if (a.dnsRows[i].res.avg >= 0 && (best < 0 || a.dnsRows[i].res.avg < a.dnsRows[best].res.avg)) best = i;
        if (best >= 0) lb::Log("Найшвидший DNS: %s (%.0f мс)", a.dnsRows[best].name.c_str(), a.dnsRows[best].res.avg);
    });
}

static double RowAvg(const DnsRow& r) {
    if (r.done) return r.res.avg;
    // Живе значення під час тесту
    double sum = 0;
    int ok = 0;
    for (float s : r.samples)
        if (s >= 0) sum += s, ++ok;
    return ok ? sum / ok : -1;
}

static void PageDnsUi(App& a) {
    Title("DNS", "Вибір найшвидшого DNS-сервера. Програма надсилає справжні DNS-запити і міряє час відповіді.");
    RefreshDnsInfo(a);
    if (!a.dnsTested && !a.dnsTask.running) StartDnsTest(a);

    BeginCard("dnscur", ImVec2(0, 0));
    ImGui::TextColored(kMuted, "Зараз:");
    ImGui::SameLine();
    const auto& ps = dns::Providers();
    if (a.dnsCurrent >= 0) {
        ImGui::PushFont(g_fontBold);
        ImGui::TextUnformatted(ps[a.dnsCurrent].name);
        ImGui::PopFont();
        ImGui::SameLine();
    }
    ImGui::TextColored(kMuted, "%s", a.dnsCurrentText.empty() ? "немає активного підключення" : a.dnsCurrentText.c_str());
    ImGui::Spacing();
    ImGui::BeginDisabled(a.dnsTask.running);
    if (AccentButton(a.dnsTask.running ? "Перевіряю..." : "Перевірити ще раз", ImVec2(S(200), S(36)))) StartDnsTest(a);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(a.tweakTask.running);
    if (ImGui::Button(a.dnsChanged ? "Повернути як було" : "Автоматично (від провайдера)", ImVec2(S(260), S(36))))
        RunTweakJob(a, "dns", [&a] {
            dns::Restore();
            a.dnsInfoTime = -1e9;
        });
    ImGui::EndDisabled();
    HelpText("DNS впливає на швидкість відкриття сайтів, входу в лаунчер і підключення до матчу, але не на пінг "
             "у самій грі.");
    EndCard();
    ImGui::Spacing();
    if (a.dnsIntercepted) {
        InfoBox("Твоя мережа перехоплює DNS-запити (так роблять деякі провайдери, роутери або антивіруси): відповідь "
                "приходить навіть з адреси, де DNS-сервера немає. Через це цифри нижче недостовірні, а зміна DNS у "
                "Windows може нічого не дати. Перевір налаштування роутера/антивіруса або спитай провайдера.",
                kWarn);
        ImGui::Spacing();
    }

    // Сортування: найшвидші зверху (після завершення тесту)
    std::lock_guard<std::mutex> l(a.dnsMutex);
    std::vector<int> order(a.dnsRows.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = (int)i;
    bool allDone = !a.dnsTask.running;
    if (allDone)
        std::stable_sort(order.begin(), order.end(), [&](int x, int y) {
            double ax = a.dnsRows[x].res.avg, ay = a.dnsRows[y].res.avg;
            if (ax < 0) return false;
            if (ay < 0) return true;
            return ax < ay;
        });
    int fastest = -1;
    for (int i : order)
        if (RowAvg(a.dnsRows[i]) >= 0 && (fastest < 0 || RowAvg(a.dnsRows[i]) < RowAvg(a.dnsRows[fastest]))) fastest = i;
    if (a.dnsIntercepted) fastest = -1;  // при перехопленні "найшвидший" був би вигадкою

    BeginCard("dnslist", ImVec2(0, 0));
    int rank = 0;
    for (int i : order) {
        const DnsRow& r = a.dnsRows[i];
        ++rank;
        ImGui::PushID(i);
        if (rank > 1) ImGui::Separator();
        float rowY = ImGui::GetCursorPosY();
        // Номер
        ImVec2 p = ImGui::GetCursorScreenPos();
        float box = S(34);
        ImGui::GetWindowDrawList()->AddRect(p, ImVec2(p.x + box, p.y + box), ImGui::GetColorU32(ImVec4(1, 1, 1, 0.12f)),
                                            S(8));
        char num[8];
        snprintf(num, sizeof(num), "%d", rank);
        ImVec2 ts = ImGui::CalcTextSize(num);
        ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + (box - ts.x) / 2, p.y + (box - ts.y) / 2),
                                            ImGui::GetColorU32(ImVec4(0.85f, 0.87f, 0.92f, 1)), num);
        ImGui::Dummy(ImVec2(box, box));
        ImGui::SameLine(0, S(14));
        // Назва, адреси, примітка
        ImGui::BeginGroup();
        ImGui::PushFont(g_fontBold);
        ImGui::TextUnformatted(r.name.c_str());
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::TextColored(kMuted, "%s", r.servers.c_str());
        bool isCurrent = (r.provider >= 0 && r.provider == a.dnsCurrent) || (r.provider < 0 && a.dnsCurrent < 0);
        if (isCurrent) {
            ImGui::SameLine();
            Pill("поточний", ImVec4(0.85f, 0.87f, 0.92f, 1));
        }
        if (i == fastest && allDone) {
            ImGui::SameLine();
            Pill("найшвидший", kGood);
        }
        ImGui::TextColored(kMuted, "%s", r.note.c_str());
        ImGui::EndGroup();

        // Відгук і кнопка — праворуч
        float right = ImGui::GetContentRegionMax().x;
        ImGui::SameLine(right - S(330));
        ImGui::SetCursorPosY(rowY + S(6));
        double avg = RowAvg(r);
        ImGui::TextColored(kMuted, "Середній відгук");
        ImGui::SameLine();
        if (avg >= 0) {
            ImVec4 c = avg < 20 ? kGood : (avg < 60 ? kWarn : kBad);
            Dot(c);
            ImGui::PushFont(g_fontBold);
            ImGui::Text("%.0f мс", avg);
            ImGui::PopFont();
        } else if (r.done) {
            Dot(kBad);
            ImGui::TextColored(kBad, "немає відповіді");
        } else {
            ImGui::TextColored(kMuted, "...");
        }
        if (r.provider >= 0) {
            ImGui::SameLine(right - S(120));
            ImGui::SetCursorPosY(rowY);
            ImGui::BeginDisabled(a.tweakTask.running || isCurrent);
            bool fast = i == fastest && allDone;
            int pi = r.provider;
            bool clicked = fast ? AccentButton(isCurrent ? "Вибрано" : "Найшвидший", ImVec2(S(120), S(36)), kBad)
                                : ImGui::Button(isCurrent ? "Вибрано" : "Вибрати", ImVec2(S(120), S(36)));
            if (clicked)
                RunTweakJob(a, "dns", [&a, pi] {
                    dns::Apply(dns::Providers()[pi]);
                    a.dnsInfoTime = -1e9;
                });
            ImGui::EndDisabled();
        }
        ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), rowY + S(46)));
        ImGui::PopID();
    }
    EndCard();
    HelpText("Порада: різниця в кілька мілісекунд майже непомітна. Обирай стабільний сервер без втрат; "
             "AdGuard блокує рекламу, Quad9 і Comodo — шкідливі сайти.");
}

static void PageGameUi(App& a) {
    Title("Режим гри",
          "Чекає на запуск гри, тримає Timer Resolution, ставить High-пріоритет і вимикає EcoQoS (якщо гра без "
          "захисту античитом), за потреби очищає ОЗУ.");
    bool on = a.gameTask.running;
    DWORD pid = a.gamePid;
    BeginCard("gcfg", ImVec2(0, 0));
    ImGui::BeginDisabled(on);
    ImGui::TextUnformatted("exe гри");
    ImGui::SetNextItemWidth(S(360));
    ImGui::InputTextWithHint("##gexe", "FortniteClient-Win64-Shipping.exe", a.gameExe, sizeof(a.gameExe));
    ImGui::SameLine();
#if KB_IS_SALE
    if (ImGui::Button(game::DisplayName().c_str())) snprintf(a.gameExe, sizeof(a.gameExe), "%s", game::ExeList().c_str());
#else
    if (ImGui::Button("Fortnite")) snprintf(a.gameExe, sizeof(a.gameExe), "%s", fn::kExe);
#endif
    ImGui::TextUnformatted("Timer Resolution");
    ImGui::SetNextItemWidth(S(360));
    ImGui::SliderFloat("##gres", &a.gameRes, 0.5f, 2.0f, "%.3f мс");
    ImGui::EndDisabled();
    if (ImGui::Checkbox("Згортати KairoBoost, коли гра запущена", &a.minimizeOnGame)) SaveSettings(a);
    ImGui::Spacing();
    if (!on) {
        if (AccentButton("Запустити режим гри", ImVec2(S(260), S(48)))) StartGameMode(a);
    } else {
        if (AccentButton("Зупинити", ImVec2(S(260), S(48)), kBad)) StopGameMode(a);
    }
    EndCard();

    ImGui::Spacing();
    BeginCard("gstate", ImVec2(0, 0));
    ImGui::PushFont(g_fontTitle);
    if (!on) {
        Dot(kMuted);
        ImGui::TextColored(kMuted, "Вимкнено");
    } else if (!pid) {
        Dot(kWarn);
        ImGui::TextColored(kWarn, "Чекаю на запуск %s", a.gameExe);
    } else {
        Dot(kGood);
        ImGui::TextColored(kGood, "Гра запущена — буст активний");
    }
    ImGui::PopFont();
    if (on && pid) {
        ULONG c = 0, f = 0, cur = 0;
        lb::TimerQuery(c, f, cur);
        ImGui::Text("PID %lu  ·  таймер %.3f мс", pid, cur / 10000.0);
    }
    ImGui::Spacing();
    InfoBox("Fortnite захищений EasyAntiCheat, тому змінити йому пріоритет «на льоту» не можна. Для цього увімкни "
            "твік «Високий пріоритет CPU для Fortnite» на сторінці Fortnite — Windows сама запускатиме гру з "
            "високим пріоритетом.");
    EndCard();
}

static void PageInfoUi(App&) {
    Title("Як це працює");
    BeginCard("info", ImVec2(0, 0));
    ImGui::PushFont(g_fontTitle);
    ImGui::TextColored(kAccent, "Чесно про пінг");
    ImGui::PopFont();
    ImGui::TextWrapped(
        KB_ED("Сервіси на кшталт ExitLag зменшують пінг,", "Ігрові VPN-сервіси зменшують пінг,") " пускаючи трафік гри через свої сервери по всьому світу "
        "(кращий маршрут + дублювання пакетів). Без такої мережі серверів жодна програма на твоєму ПК не змінить "
        "маршрут пакетів в інтернеті. Тому KairoBoost не обіцяє «-50 пінгу», а робить те, що реально можна "
        "зробити локально: стабільніший FPS, менша затримка вводу, пріоритет трафіку гри у твоїй мережі і вибір "
        "найкращого регіону.");
    ImGui::Spacing();
    ImGui::PushFont(g_fontTitle);
    ImGui::TextColored(kAccent2, "Що реально робить KairoBoost");
    ImGui::PopFont();
    const char* items[] = {
        "Timer Resolution — точніші таймери Windows: рівніший frametime, менша затримка вводу.",
        "Твіки Windows — пріоритети для ігор, схема живлення, енергозбереження мережі, ігрова панель тощо. "
        "Кожен вмикається окремо і відкочується назад.",
        KB_ED("Fortnite — високий пріоритет через Windows (сумісно з EasyAntiCheat), дискретна відеокарта, налаштування "
              "FPS/VSync/режиму продуктивності, пінг до регіонів Epic.",
              "Будь-яка гра — високий пріоритет через Windows (сумісно з античитами), дискретна відеокарта, QoS, режим "
              "гри; для Fortnite ще й налаштування FPS/VSync/режиму продуктивності і пінг до регіонів Epic."),
        "QoS DSCP 46 — пріоритет пакетів гри у твоїй мережі (Wi-Fi, роутер).",
        "Очистка ОЗУ — вручну, при запуску гри, за нестачі пам'яті або за розкладом.",
    };
    for (auto* it : items) {
        Dot(kGood);
        ImGui::TextWrapped("%s", it);
    }
    ImGui::Spacing();
    ImGui::PushFont(g_fontTitle);
    ImGui::TextColored(kWarn, "Безпека");
    ImGui::PopFont();
    ImGui::TextWrapped(
        "KairoBoost не втручається в пам'ять чи файли гри (для Fortnite — лише стандартний файл налаштувань) "
        "— тільки налаштування Windows. Усі зміни реєстру зберігаються в резервну копію; кнопка «Відкотити все» на "
        "сторінці «Твіки» повертає все як було.");
    EndCard();
}

// ---------------------------------------------------------------------------
// Головний кадр
// ---------------------------------------------------------------------------

static void DrawUi(App& a) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar();

    // --- Бокова панель ---
    float sideW = S(220);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kPanel);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(14), S(18)));
    ImGui::BeginChild("side", ImVec2(sideW, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PushFont(g_fontTitle);
    ImGui::TextColored(kAccent, "KAIRO");
    ImGui::SameLine(0, S(6));
    ImGui::TextUnformatted("BOOST");
#if KB_IS_SALE
    ImGui::SameLine(0, S(6));
    ImGui::TextColored(kFortnite, "PRO");
#endif
    ImGui::PopFont();
    ImGui::TextColored(kMuted, KB_ED("оптимізатор для Fortnite", "оптимізатор для ігор"));
    ImGui::Dummy(ImVec2(0, S(14)));

    const char* names[PageCount] = {"Головна",     "Порадник",    KB_ED("Fortnite", "Гра"),    "Твіки",     "Timer Resolution",
                                    "Очистка ОЗУ", "Тест мережі", "DNS",       "Режим гри",
                                    "Як це працює"};
    for (int i = 0; i < PageCount; ++i) {
        bool sel = a.page == i;
        ImVec4 acc = i == PageFortnite ? kFortnite : kAccent;
        ImGui::PushStyleColor(ImGuiCol_Button, sel ? Fade(acc, 0.18f) : ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Fade(acc, sel ? 0.25f : 0.10f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, Fade(acc, 0.30f));
        ImGui::PushStyleColor(ImGuiCol_Text, sel ? acc : ImVec4(0.85f, 0.87f, 0.92f, 1));
        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(14), S(9)));
        if (ImGui::Button(names[i], ImVec2(-1, 0))) a.page = (Page)i;
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(4);
    }

    float bottom = ImGui::GetWindowHeight() - S(18) - ImGui::GetTextLineHeightWithSpacing() * 4;
    if (ImGui::GetCursorPosY() < bottom) ImGui::SetCursorPosY(bottom);
    Dot(a.elevated ? kGood : kBad);
    ImGui::TextColored(a.elevated ? kMuted : kBad, a.elevated ? "Адміністратор" : "Без прав адміна");
    Dot(a.gameTask.running ? kGood : kMuted);
    ImGui::TextColored(kMuted, a.gameTask.running ? "Буст активний" : "Буст вимкнено");
    Dot(a.lowPower ? kAccent2 : kMuted);
    ImGui::TextColored(kMuted, a.lowPower ? "Економний режим" : "Звичайний режим");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Під час гри KairoBoost знижує свій пріоритет, працює в режимі ефективності Windows\n"
                          "і рідко перемальовує вікно, щоб не забирати ресурси в гри.");
    ImGui::TextColored(kMuted, "Windows build %lu", a.build);
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    ImGui::SameLine(0, 0);

    // --- Основна область ---
    ImGui::BeginGroup();
    float logH = S(130);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(24), S(20)));
    ImGui::BeginChild("main", ImVec2(0, -logH), ImGuiChildFlags_AlwaysUseWindowPadding);
    if (!a.elevated) {
        InfoBox("Програма запущена без прав адміністратора — твіки, QoS і очистка ОЗУ не спрацюють.", kBad);
        ImGui::Spacing();
    }
    switch (a.page) {
        case PageHome: PageHomeUi(a); break;
        case PageAdvisor: PageAdvisorUi(a); break;
        case PageFortnite: PageFortniteUi(a); break;
        case PageTweaks: PageTweaksUi(a); break;
        case PageTimer: PageTimerUi(a); break;
        case PageRam: PageRamUi(a); break;
        case PageNet: PageNetUi(a); break;
        case PageDns: PageDnsUi(a); break;
        case PageGame: PageGameUi(a); break;
        default: PageInfoUi(a); break;
    }
    ImGui::EndChild();

    // --- Журнал ---
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.040f, 0.045f, 0.062f, 1));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(16), S(10)));
    ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::TextColored(kMuted, "ЖУРНАЛ");
    ImGui::SameLine(ImGui::GetContentRegionMax().x - S(80));
    if (ImGui::SmallButton("Очистити")) {
        std::lock_guard<std::mutex> l(a.logMutex);
        a.log.clear();
    }
    ImGui::BeginChild("logtext", ImVec2(0, 0));
    {
        std::lock_guard<std::mutex> l(a.logMutex);
        for (auto& line : a.log) {
            ImVec4 c = ImVec4(0.80f, 0.83f, 0.88f, 1);
            if (line.find("[OK]") != std::string::npos) c = kGood;
            else if (line.find("[!]") != std::string::npos || line.find("[ПОМИЛКА]") != std::string::npos) c = kBad;
            ImGui::PushStyleColor(ImGuiCol_Text, c);
            ImGui::TextUnformatted(line.c_str());
            ImGui::PopStyleColor();
        }
        if (a.logScroll) {
            ImGui::SetScrollHereY(1.0f);
            a.logScroll = false;
        }
    }
    ImGui::EndChild();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    ImGui::EndGroup();

    ImGui::End();
}

static void SetupStyle() {
    ImGuiStyle& st = ImGui::GetStyle();
    ImGui::StyleColorsDark(&st);
    st.WindowRounding = 0;
    st.ChildRounding = 10;
    st.FrameRounding = 7;
    st.GrabRounding = 7;
    st.PopupRounding = 8;
    st.ScrollbarRounding = 8;
    st.TabRounding = 7;
    st.FramePadding = ImVec2(10, 7);
    st.ItemSpacing = ImVec2(10, 9);
    st.WindowBorderSize = 0;
    st.ChildBorderSize = 1;
    st.FrameBorderSize = 0;
    st.GrabMinSize = 14;
    ImVec4* c = st.Colors;
    c[ImGuiCol_WindowBg] = kBg;
    c[ImGuiCol_ChildBg] = kBg;
    c[ImGuiCol_PopupBg] = kPanel;
    c[ImGuiCol_Border] = ImVec4(1, 1, 1, 0.06f);
    c[ImGuiCol_FrameBg] = kPanel2;
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.16f, 0.18f, 0.24f, 1);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.19f, 0.21f, 0.28f, 1);
    c[ImGuiCol_Button] = kPanel2;
    c[ImGuiCol_ButtonHovered] = ImVec4(0.17f, 0.19f, 0.26f, 1);
    c[ImGuiCol_ButtonActive] = ImVec4(0.21f, 0.23f, 0.31f, 1);
    c[ImGuiCol_SliderGrab] = kAccent;
    c[ImGuiCol_SliderGrabActive] = kAccent;
    c[ImGuiCol_CheckMark] = kAccent;
    c[ImGuiCol_Header] = Fade(kAccent, 0.2f);
    c[ImGuiCol_HeaderHovered] = Fade(kAccent, 0.3f);
    c[ImGuiCol_TableHeaderBg] = kPanel2;
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.025f);
    c[ImGuiCol_TableBorderLight] = ImVec4(1, 1, 1, 0.04f);
    c[ImGuiCol_TextSelectedBg] = Fade(kAccent, 0.35f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.6f);
    c[ImGuiCol_PlotLines] = kAccent2;
    c[ImGuiCol_TitleBg] = kPanel2;
    c[ImGuiCol_TitleBgActive] = kPanel2;
    c[ImGuiCol_PlotLinesHovered] = kAccent;
    st.ScaleAllSizes(g_dpi);
}

static void LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    static const ImWchar ranges[] = {0x0020, 0x00FF, 0x0400, 0x052F, 0x2000, 0x206F, 0x2190, 0x21FF, 0};
    char win[MAX_PATH];
    GetWindowsDirectoryA(win, MAX_PATH);
    std::string dir = std::string(win) + "\\Fonts\\";
    // Перший знайдений шрифт з кирилицею; вбудований шрифт ImGui — крайній випадок (без кирилиці)
    auto load = [&](std::initializer_list<const char*> files, float size) -> ImFont* {
        for (const char* f : files) {
            std::string p = dir + f;
            if (GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES)
                return io.Fonts->AddFontFromFileTTF(p.c_str(), size * g_dpi, nullptr, ranges);
        }
        ImFontConfig cfg;
        cfg.SizePixels = size * g_dpi;
        return io.Fonts->AddFontDefault(&cfg);
    };
    g_fontBody = load({"segoeui.ttf", "tahoma.ttf", "arial.ttf", "verdana.ttf"}, 17);
    g_fontBold = load({"seguisb.ttf", "segoeuib.ttf", "tahomabd.ttf", "arialbd.ttf", "arial.ttf"}, 17);
    g_fontTitle = load({"seguisb.ttf", "segoeuib.ttf", "tahomabd.ttf", "arialbd.ttf", "arial.ttf"}, 22);
    g_fontBig = load({"segoeuib.ttf", "tahomabd.ttf", "arialbd.ttf", "arial.ttf"}, 34);
    io.FontDefault = g_fontBody;
}

// ---------------------------------------------------------------------------
// DirectX 11 + Win32 (на основі офіційного прикладу Dear ImGui)
// ---------------------------------------------------------------------------

static ID3D11Device* g_device = nullptr;
static ID3D11DeviceContext* g_context = nullptr;
static IDXGISwapChain* g_swapChain = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static bool g_occluded = false;
static UINT g_resizeW = 0, g_resizeH = 0;

static void CreateRenderTarget() {
    ID3D11Texture2D* back = nullptr;
    g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) {
        g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}

static void CleanupRenderTarget() {
    if (g_rtv) {
        g_rtv->Release();
        g_rtv = nullptr;
    }
}

static bool CreateDevice(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL fl;
    const D3D_FEATURE_LEVEL levels[2] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                               D3D11_SDK_VERSION, &sd, &g_swapChain, &g_device, &fl, &g_context);
    if (hr == DXGI_ERROR_UNSUPPORTED)
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                           &sd, &g_swapChain, &g_device, &fl, &g_context);
    if (hr != S_OK) return false;
    CreateRenderTarget();
    return true;
}

static void CleanupDevice() {
    CleanupRenderTarget();
    if (g_swapChain) g_swapChain->Release();
    if (g_context) g_context->Release();
    if (g_device) g_device->Release();
    g_swapChain = nullptr;
    g_context = nullptr;
    g_device = nullptr;
}

static LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;
    switch (msg) {
        case WM_SIZE:
            if (wp == SIZE_MINIMIZED) return 0;
            g_resizeW = LOWORD(lp);
            g_resizeH = HIWORD(lp);
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mm = (MINMAXINFO*)lp;
            mm->ptMinTrackSize.x = (LONG)(960 * g_dpi);
            mm->ptMinTrackSize.y = (LONG)(640 * g_dpi);
            return 0;
        }
        case WM_SYSCOMMAND:
            if ((wp & 0xfff0) == SC_KEYMENU) return 0;
            break;
        case WM_CLOSE:
            if (!g_reallyQuit && g_app && g_app->closeToTray) {
                HideWindow(hwnd, true);
                if (!g_app->trayHintShown) {
                    TrayBalloon(L"KairoBoost працює у треї",
                                L"Буст і таймер продовжують працювати. Вийти: правою кнопкою по значку → Вихід.");
                    g_app->trayHintShown = true;
                    SaveSettings(*g_app);
                }
                return 0;
            }
            DestroyWindow(hwnd);
            return 0;
        case WM_SHOW_EXISTING:
            ShowFromTray(hwnd);
            return 0;
        case WM_TRAY:
            if (LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_LBUTTONDBLCLK) {
                ShowFromTray(hwnd);
            } else if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) {
                HMENU m = CreatePopupMenu();
                AppendMenuW(m, MF_STRING, ID_TRAY_OPEN, L"Відкрити KairoBoost");
                AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(m, MF_STRING, ID_TRAY_EXIT, L"Вихід");
                SetMenuDefaultItem(m, ID_TRAY_OPEN, FALSE);
                POINT pt;
                GetCursorPos(&pt);
                SetForegroundWindow(hwnd);  // інакше меню не закривається кліком повз нього
                UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
                DestroyMenu(m);
                if (cmd == ID_TRAY_OPEN) ShowFromTray(hwnd);
                if (cmd == ID_TRAY_EXIT) {
                    g_reallyQuit = true;
                    DestroyWindow(hwnd);
                }
            }
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    if (g_taskbarCreated && msg == g_taskbarCreated) {  // Провідник перезапустився — повертаємо значок
        TrayAdd(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void HideWindow(HWND hwnd, bool toTray) {
    if (toTray)
        ShowWindow(hwnd, SW_HIDE);
    else
        ShowWindow(hwnd, SW_MINIMIZE);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int) {
    // Лише одна копія: повторний запуск показує вже відкрите вікно (зокрема з трею)
    HANDLE single = CreateMutexW(nullptr, TRUE, L"KairoBoost_SingleInstance");
    if (single && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(L"KairoBoostWnd", nullptr);
        if (other) {
            AllowSetForegroundWindow(ASFW_ANY);
            PostMessageW(other, WM_SHOW_EXISTING, 0, 0);
        }
        return 0;
    }
    ImGui_ImplWin32_EnableDpiAwareness();
    {
        POINT pt{0, 0};
        g_dpi = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY));
        if (g_dpi <= 0) g_dpi = 1.0f;
    }

    App app;
    g_app = &app;
    lb::SetLogSink([](const std::string& s) { AddLog(s); });
    if (!lb::Init()) {
        MessageBoxW(nullptr, L"Не вдалося завантажити функції таймера з ntdll.dll", L"KairoBoost", MB_ICONERROR);
        return 1;
    }
    app.elevated = lb::IsElevated();
    app.build = lb::WindowsBuild();
    LoadSettings(app);
#if KB_IS_SALE
    ApplyGame(app, app.gameId, false);
#endif
    fn::SetManualDir(app.fnDir);
    {
        int d, i;
        if (tw::ReadSystemKeyRepeat(d, i)) app.kbDelay = d, app.kbInterval = i;  // що реально стоїть у системі
        tw::SetKeyRepeat(app.kbDelay, app.kbInterval);
    }
    app.resMs = app.gameRes;
    lb::Log("KairoBoost запущено. Windows build %lu%s", app.build,
            app.elevated ? "" : " — [!] без прав адміністратора");

    WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, WndProc, 0, 0, inst, LoadIconW(inst, MAKEINTRESOURCEW(1)), nullptr,
                   nullptr, nullptr, L"KairoBoostWnd", nullptr};
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, (LPCWSTR)IDI_APPLICATION);
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    RegisterClassExW(&wc);
    int w = (int)(1180 * g_dpi), h = (int)(780 * g_dpi);
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2, y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"KairoBoost", WS_OVERLAPPEDWINDOW,
                              std::max(0, x), std::max(0, y), w, h, nullptr, nullptr, inst, nullptr);

    // Темний заголовок вікна (Windows 10 20H1+ / 11)
    {
        typedef HRESULT(WINAPI * DwmSet_t)(HWND, DWORD, LPCVOID, DWORD);
        HMODULE dwm = LoadLibraryA("dwmapi.dll");
        if (dwm) {
            auto fn = (DwmSet_t)(void*)GetProcAddress(dwm, "DwmSetWindowAttribute");
            BOOL dark = TRUE;
            if (fn) fn(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
        }
    }

    if (!CreateDevice(hwnd)) {
        CleanupDevice();
        MessageBoxW(nullptr, L"Не вдалося ініціалізувати DirectX 11", L"KairoBoost", MB_ICONERROR);
        return 1;
    }
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    TrayAdd(hwnd);
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    SetupStyle();
    LoadFonts();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    // Цикл малювання економний: кадр малюється лише коли є ввід або раз на певний час.
    //  - вікно активне:          до 60 к/с під час взаємодії, ~15 к/с у спокої
    //  - вікно у фоні:           ~4 к/с (під час гри — 1 к/с)
    //  - згорнуте / перекрите:   не малюється взагалі, лише фонові перевірки раз на секунду
    bool done = false;
    DWORD prevPid = 0;
    int busyFrames = 3;  // кілька кадрів після вводу — щоб анімації ImGui встигли завершитися
    while (!done) {
        bool minimized = IsIconic(hwnd) || !IsWindowVisible(hwnd);
        bool focused = GetForegroundWindow() == hwnd;
        DWORD pid = app.gamePid;
        bool inGame = pid != 0;

        // Гра щойно запустилася — згортаємо вікно
        if (inGame && !prevPid && app.minimizeOnGame && !minimized) {
            HideWindow(hwnd, app.closeToTray);
            minimized = true;
        }
        prevPid = pid;

        bool wantLow = minimized || (inGame && !focused);
        if (wantLow != app.lowPower) {
            app.lowPower = wantLow;
            lb::SetSelfLowPower(wantLow);
        }

        DWORD timeout;
        if (minimized || g_occluded) timeout = 1000;
        else if (busyFrames > 0) timeout = 0;
        else if (!focused) timeout = inGame ? 1000 : 250;
        else timeout = 66;
        if (timeout && MsgWaitForMultipleObjectsEx(0, nullptr, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE) == WAIT_OBJECT_0)
            busyFrames = 3;
        else if (busyFrames > 0)
            --busyFrames;

        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;
        RamScheduler(app);

        if (IsIconic(hwnd) || !IsWindowVisible(hwnd)) continue;
        if (g_occluded) {
            if (g_swapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) continue;
            g_occluded = false;
        }
        if (g_resizeW && g_resizeH) {
            CleanupRenderTarget();
            g_swapChain->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
            g_resizeW = g_resizeH = 0;
            CreateRenderTarget();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawUi(app);
        ImGui::Render();
        const float clear[4] = {kBg.x, kBg.y, kBg.z, 1.0f};
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_context->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        // Без очікування vsync: частоту кадрів і так обмежує цикл вище
        g_occluded = g_swapChain->Present(busyFrames > 0 ? 1 : 0, 0) == DXGI_STATUS_OCCLUDED;
    }

    // Зупиняємо фонові задачі
    SaveSettings(app);
    app.gameStop = true;
    app.gameTask.Join();
    app.netTask.Join();
    app.timerTask.Join();
    app.tweakTask.Join();
    app.ramTask.Join();
    app.dnsTask.Join();
    app.advTask.Join();
    lb::SetLogSink(nullptr);
    lb::Shutdown();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDevice();
    TrayRemove();
    if (IsWindow(hwnd)) DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}
