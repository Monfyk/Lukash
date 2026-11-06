// LukashBoost — графічна версія (Dear ImGui + DirectX 11)
#include "core.h"

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

static ImFont* g_fontBody = nullptr;
static ImFont* g_fontTitle = nullptr;
static ImFont* g_fontBig = nullptr;
static float g_dpi = 1.0f;

static ImVec4 Fade(ImVec4 c, float a) { return ImVec4(c.x, c.y, c.z, a); }

// ---------------------------------------------------------------------------
// Фонові задачі
// ---------------------------------------------------------------------------

struct Task {
    std::thread thread;
    std::atomic<bool> running{false};
    std::atomic<bool> cancel{false};

    template <class F>
    void Start(F fn) {
        if (running) return;
        if (thread.joinable()) thread.join();
        cancel = false;
        running = true;
        thread = std::thread([this, fn] {
            fn();
            running = false;
        });
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
    std::string target;
    std::vector<float> samples;  // < 0 — втрачений пакет
    lb::PingResult result;
    bool done = false;

    // Живі показники з уже отриманих відповідей
    void Stats(float& mn, float& avg, float& mx, float& jit, float& loss) const {
        mn = 1e9f, avg = 0, mx = 0, jit = 0;
        int ok = 0;
        float prev = -1;
        int jn = 0;
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

enum Page { PageHome, PageTimer, PageNet, PageTweaks, PageGame, PageInfo, PageCount };

struct App {
    Page page = PageHome;
    bool elevated = false;
    DWORD build = 0;

    // Лог
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
    char targets[512] = "1.1.1.1 8.8.8.8 9.9.9.9 google.com:443";
    int pingCount = 30;
    std::mutex netMutex;
    std::vector<LiveTarget> live;
    Task netTask;

    // Твіки
    lb::TweakStatus status;
    double statusTime = -1e9;
    char qosExe[128] = "cs2.exe";
    Task tweakTask;
    bool confirmRevert = false;

    // Режим гри
    char gameExe[128] = "cs2.exe";
    float gameRes = 0.5f;
    bool gameQos = false;
    bool gameTweaks = false;
    std::atomic<bool> gameStop{false};
    std::atomic<DWORD> gamePid{0};
    Task gameTask;
};

static App* g_app = nullptr;

static void AddLog(const std::string& s) {
    std::lock_guard<std::mutex> l(g_app->logMutex);
    g_app->log.push_back(s);
    while (g_app->log.size() > 500) g_app->log.pop_front();
    g_app->logScroll = true;
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

static bool BeginCard(const char* id, ImVec2 size) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kPanel);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(10));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(16), S(14)));
    bool r = ImGui::BeginChild(id, size, ImGuiChildFlags_Borders | (size.y == 0 ? ImGuiChildFlags_AutoResizeY : 0));
    return r;
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
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(value);
    ImGui::PopStyleColor();
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

// ---------------------------------------------------------------------------
// Дії
// ---------------------------------------------------------------------------

static void StartGameMode(App& a) {
    if (a.gameTask.running) return;
    std::string exe = a.gameExe;
    if (exe.empty()) return;
    ULONG res = (ULONG)std::lround(a.gameRes * 10000.0);
    bool qos = a.gameQos, tweaks = a.gameTweaks;
    a.gameStop = false;
    a.gameTask.Start([&a, exe, res, qos, tweaks] {
        if (tweaks) lb::ApplyNetworkTweaks();
        if (qos) lb::ApplyQosForGame(exe);
        lb::RunGameMode(exe, res, a.gameStop, &a.gamePid);
    });
}

static void StopGameMode(App& a) { a.gameStop = true; }

static void StartPing(App& a) {
    auto list = lb::Split(a.targets);
    if (list.empty() || a.netTask.running) return;
    {
        std::lock_guard<std::mutex> l(a.netMutex);
        a.live.clear();
        for (auto& t : list) {
            LiveTarget lt;
            lt.target = t;
            a.live.push_back(lt);
        }
    }
    int count = a.pingCount;
    a.netTask.Start([&a, list, count] {
        lb::Log("Тест мережі: %d серверів по %d запитів", (int)list.size(), count);
        std::vector<std::thread> workers;
        for (size_t i = 0; i < list.size(); ++i) {
            workers.emplace_back([&a, i, t = list[i], count] {
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
        std::vector<lb::PingResult> res;
        {
            std::lock_guard<std::mutex> l(a.netMutex);
            for (auto& lt : a.live) res.push_back(lt.result);
        }
        std::sort(res.begin(), res.end(),
                  [](const lb::PingResult& x, const lb::PingResult& y) { return x.Score() < y.Score(); });
        for (auto& r : res) lb::LogPing(r);
        if (!res.empty() && res[0].received) lb::Log("Найкращий сервер: %s", res[0].target.c_str());
    });
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
// Сторінки
// ---------------------------------------------------------------------------

static void PageHomeUi(App& a) {
    Title("Головна", "Timer Resolution + мережева оптимізація в одній програмі.");

    ULONG coarsest = 0, finest = 0, current = 0;
    lb::TimerQuery(coarsest, finest, current);
    char v1[64], v2[64], v3[64], v4[64];
    snprintf(v1, sizeof(v1), "%.3f мс", current / 10000.0);
    {
        std::lock_guard<std::mutex> l(a.sleepMutex);
        if (a.hasSleep)
            snprintf(v2, sizeof(v2), "%.2f мс", a.sleepStats.avg);
        else
            snprintf(v2, sizeof(v2), "--");
    }
    bool gameOn = a.gameTask.running;
    DWORD pid = a.gamePid;
    snprintf(v3, sizeof(v3), "%s", gameOn ? (pid ? "У ГРІ" : "ЧЕКАЮ") : "ВИМК");
    std::string best = "--";
    {
        std::lock_guard<std::mutex> l(a.netMutex);
        float bs = 1e9f;
        for (auto& lt : a.live) {
            float mn, avg, mx, jit, loss;
            lt.Stats(mn, avg, mx, jit, loss);
            if (avg > 0 && lt.Score() < bs) {
                bs = lt.Score();
                snprintf(v4, sizeof(v4), "%.0f мс", avg);
                best = v4;
            }
        }
    }

    float avail = ImGui::GetContentRegionAvail().x;
    float sp = ImGui::GetStyle().ItemSpacing.x;
    float w = (avail - sp * 3) / 4;
    ImVec4 resColor = current <= 5100 ? kGood : (current <= 10000 ? kWarn : kBad);
    StatCard("c1", "Роздільність таймера", v1, resColor, current <= 5100 ? "Максимальна точність" : "Можна краще", w);
    ImGui::SameLine();
    StatCard("c2", "Sleep(1)", v2, kAccent2, a.hasSleep ? "Останній тест" : "Тест ще не запускали", w);
    ImGui::SameLine();
    StatCard("c3", "Режим гри", v3, gameOn ? kGood : kMuted, gameOn ? a.gameExe : "Не запущено", w);
    ImGui::SameLine();
    StatCard("c4", "Найкращий пінг", best.c_str(), kAccent, best == "--" ? "Запусти тест мережі" : "З останнього тесту",
             w);

    ImGui::Spacing();
    BeginCard("quick", ImVec2(0, 0));
    ImGui::PushFont(g_fontTitle);
    ImGui::TextUnformatted("Швидкий старт");
    ImGui::PopFont();
    HelpText("Введи exe гри і натисни БУСТ. Програма дочекається запуску гри, дасть їй високий пріоритет, "
             "вимкне енергозбереження для неї і триматиме точний таймер, поки гра працює.");
    ImGui::Spacing();
    ImGui::SetNextItemWidth(S(260));
    ImGui::BeginDisabled(gameOn);
    ImGui::InputTextWithHint("##qexe", "наприклад cs2.exe", a.gameExe, sizeof(a.gameExe));
    ImGui::SameLine();
    ImGui::Checkbox("+ QoS пріоритет", &a.gameQos);
    ImGui::SameLine();
    ImGui::Checkbox("+ мережеві твіки", &a.gameTweaks);
    ImGui::EndDisabled();
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
        ImGui::Text("LukashBoost тримає: %.4f мс", held / 10000.0);
    else
        ImGui::TextUnformatted("LukashBoost зараз нічого не тримає");
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
    HelpText("Авто-підбір шукає значення, при якому Sleep(1) найближчий до 1 мс (зазвичай 0.50-0.51 мс).");
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
    BeginCard("tglobal", ImVec2(0, 0));
    bool gOn = lb::GlobalTimerEnabled();
    Dot(gOn ? kGood : kMuted);
    ImGui::TextUnformatted(gOn ? "Глобальний таймер увімкнено" : "Глобальний таймер вимкнено");
    HelpText("На Windows 11 роздільність таймера за замовчуванням діє лише на процес, який її задав. "
             "Цей ключ реєстру робить її глобальною, щоб вона діяла і на гру. Потрібне перезавантаження.");
    ImGui::BeginDisabled(gOn || !lb::GlobalTimerSupported());
    if (ImGui::Button("Увімкнути глобальний таймер")) lb::EnableGlobalTimerResolution();
    ImGui::EndDisabled();
    if (!lb::GlobalTimerSupported()) {
        ImGui::SameLine();
        ImGui::TextColored(kMuted, "(недоступно на збірці %lu)", a.build);
    }
    EndCard();
}

static void PageNetUi(App& a) {
    Title("Тест мережі",
          "Пінг, джитер і втрати до кількох серверів одночасно. Вкажи адресу (ICMP-пінг) або адресу:порт "
          "(TCP-пінг — для серверів, що не відповідають на звичайний пінг).");
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
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextUnformatted("Швидкий вибір:");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::BeginDisabled(running);
    if (ImGui::SmallButton("DNS-сервери")) snprintf(a.targets, sizeof(a.targets), "1.1.1.1 8.8.8.8 9.9.9.9");
    ImGui::SameLine();
    if (ImGui::SmallButton("Steam")) snprintf(a.targets, sizeof(a.targets), "store.steampowered.com:443 steamcommunity.com:443");
    ImGui::SameLine();
    if (ImGui::SmallButton("Discord")) snprintf(a.targets, sizeof(a.targets), "discord.com:443 gateway.discord.gg:443");
    ImGui::EndDisabled();
    EndCard();

    ImGui::Spacing();
    std::lock_guard<std::mutex> l(a.netMutex);
    if (a.live.empty()) {
        HelpText("Результати з'являться тут. Порада: перевір IP серверів гри в різних регіонах і грай там, "
                 "де найменші пінг, джитер і втрати.");
        return;
    }
    int bestIdx = -1;
    float bestScore = 1e9f;
    for (int i = 0; i < (int)a.live.size(); ++i)
        if (a.live[i].Score() < bestScore) {
            bestScore = a.live[i].Score();
            bestIdx = i;
        }

    BeginCard("ntable", ImVec2(0, 0));
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
            ImGui::TextUnformatted(lt.target.c_str());
            bool unresolved = lt.done && !lt.result.resolved;
            if (avg <= 0) {
                // Немає жодної відповіді — показуємо прочерки замість нулів
                const char* why = unresolved ? "адресу не знайдено" : (lt.samples.empty() ? "очікую..." : "немає відповіді");
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
                             ImVec2(-1, S(34)));
            ImGui::PopStyleColor(2);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    HelpText("Пінг у мс. Зелена крапка — найкращий сервер (пінг + 2×джитер + штраф за втрати).");
    EndCard();
}

static void TweakRow(bool on, const char* name, const char* desc) {
    Dot(on ? kGood : kMuted);
    ImGui::TextUnformatted(name);
    ImGui::SameLine();
    ImGui::TextColored(on ? kGood : kMuted, on ? " — увімкнено" : " — не застосовано");
    ImGui::Indent(ImGui::GetTextLineHeight() * 0.9f + ImGui::GetStyle().ItemSpacing.x);
    HelpText(desc);
    ImGui::Unindent(ImGui::GetTextLineHeight() * 0.9f + ImGui::GetStyle().ItemSpacing.x);
    ImGui::Spacing();
}

static void PageTweaksUi(App& a) {
    Title("Твіки системи",
          "Усі зміни реєстру зберігаються в резервну копію — кнопка «Відкотити все» повертає оригінальні значення.");
    if (ImGui::GetTime() - a.statusTime > 1.5 && !a.tweakTask.running) {
        a.status = lb::GetTweakStatus();
        a.statusTime = ImGui::GetTime();
    }
    const lb::TweakStatus& s = a.status;
    bool busy = a.tweakTask.running;

    BeginCard("tw", ImVec2(0, 0));
    char nagle[128];
    snprintf(nagle, sizeof(nagle), "Вимкнення алгоритму Нейгла (%d з %d інтерфейсів)", s.nagleOff, s.ifaces);
    TweakRow(s.ifaces && s.nagleOff == s.ifaces, nagle,
             "TcpAckFrequency=1, TCPNoDelay=1 — TCP-пакети відправляються одразу, без накопичення.");
    TweakRow(s.throttlingOff, "Network throttling вимкнено",
             "NetworkThrottlingIndex — Windows не обмежує обробку мережевих пакетів під час мультимедіа.");
    TweakRow(s.sysResponsiveness <= 10, "SystemResponsiveness = 10",
             "Менше ресурсів CPU резервується для фонових задач.");
    TweakRow(s.mmcss, "MMCSS-пріоритети для ігор", "Вищий пріоритет CPU/GPU для ігрових потоків.");
    TweakRow(s.globalTimer, "Глобальний Timer Resolution (Windows 11)",
             "Роздільність таймера діє на всі процеси, зокрема на гру.");
    ImGui::BeginDisabled(busy);
    if (AccentButton("Застосувати мережеві твіки", ImVec2(S(280), S(40))))
        a.tweakTask.Start([] { lb::ApplyNetworkTweaks(); });
    ImGui::SameLine();
    if (ImGui::Button("Очистити DNS-кеш", ImVec2(S(200), S(40)))) a.tweakTask.Start([] { lb::FlushDns(); });
    ImGui::SameLine();
    if (AccentButton("Відкотити все", ImVec2(S(180), S(40)), kBad)) a.confirmRevert = true;
    ImGui::EndDisabled();
    EndCard();

    ImGui::Spacing();
    BeginCard("qos", ImVec2(0, 0));
    ImGui::PushFont(g_fontTitle);
    ImGui::TextUnformatted("QoS-пріоритет для гри (DSCP 46)");
    ImGui::PopFont();
    HelpText("Пакети гри позначаються як пріоритетні. Wi-Fi (WMM) і роутери з QoS пропускають їх першими — "
             "помітно, коли хтось у мережі качає або дивиться відео.");
    ImGui::BeginDisabled(busy);
    ImGui::SetNextItemWidth(S(260));
    ImGui::InputTextWithHint("##qosexe", "cs2.exe", a.qosExe, sizeof(a.qosExe));
    ImGui::SameLine();
    if (ImGui::Button("Додати QoS")) {
        std::string exe = a.qosExe;
        a.tweakTask.Start([exe] { lb::ApplyQosForGame(exe); });
    }
    ImGui::EndDisabled();
    if (s.qosGames.empty()) {
        HelpText("Політик поки немає.");
    } else {
        for (auto& g : s.qosGames) {
            Dot(kGood);
            ImGui::TextUnformatted(g.c_str());
        }
    }
    if (busy) ImGui::TextColored(kAccent2, "Працюю...");
    EndCard();

    if (a.confirmRevert) {
        ImGui::OpenPopup("Відкотити зміни?");
        a.confirmRevert = false;
    }
    if (ImGui::BeginPopupModal("Відкотити зміни?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Усі зміни LukashBoost у реєстрі буде повернуто до оригінальних значень,\n"
                               "QoS-політики видалено.");
        ImGui::Spacing();
        if (AccentButton("Так, відкотити", ImVec2(S(170), 0), kBad)) {
            a.tweakTask.Start([] { lb::RevertAll(); });
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Скасувати", ImVec2(S(170), 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

static void PageGameUi(App& a) {
    Title("Режим гри",
          "Програма чекає на запуск гри, ставить їй High-пріоритет, вимикає для неї EcoQoS і тримає Timer "
          "Resolution, поки гра працює. Можна згорнути вікно — усе працює у фоні.");
    bool on = a.gameTask.running;
    DWORD pid = a.gamePid;

    BeginCard("gcfg", ImVec2(0, 0));
    ImGui::BeginDisabled(on);
    ImGui::TextUnformatted("exe гри");
    ImGui::SetNextItemWidth(S(320));
    ImGui::InputTextWithHint("##gexe", "cs2.exe / valorant.exe / FortniteClient-Win64-Shipping.exe", a.gameExe,
                             sizeof(a.gameExe));
    ImGui::TextUnformatted("Timer Resolution");
    ImGui::SetNextItemWidth(S(320));
    ImGui::SliderFloat("##gres", &a.gameRes, 0.5f, 2.0f, "%.3f мс");
    ImGui::Checkbox("Додати QoS-пріоритет (DSCP 46) для цієї гри", &a.gameQos);
    ImGui::Checkbox("Застосувати мережеві твіки перед стартом", &a.gameTweaks);
    ImGui::EndDisabled();
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
        ImGui::Text("PID %lu  ·  пріоритет High  ·  таймер %.3f мс", pid, cur / 10000.0);
    }
    EndCard();
}

static void PageInfoUi(App&) {
    Title("Як це працює");
    BeginCard("info", ImVec2(0, 0));
    ImGui::PushFont(g_fontTitle);
    ImGui::TextColored(kAccent, "Чесно про ExitLag");
    ImGui::PopFont();
    ImGui::TextWrapped(
        "ExitLag і подібні сервіси зменшують пінг, пускаючи трафік гри через свої сервери по всьому світу "
        "(кращий маршрут + дублювання пакетів кількома шляхами). Без такої мережі серверів жодна програма на "
        "твоєму ПК не змінить маршрут пакетів в інтернеті. Програми, що обіцяють «-50 пінгу» без серверів, "
        "обманюють.");
    ImGui::Spacing();
    ImGui::PushFont(g_fontTitle);
    ImGui::TextColored(kAccent2, "Що реально робить LukashBoost");
    ImGui::PopFont();
    const char* items[] = {
        "Timer Resolution — точніші таймери Windows: рівніший frametime, менша затримка вводу.",
        "Вимкнення Нейгла — TCP-пакети йдуть одразу (для ігор на TCP).",
        "Network throttling — Windows не обмежує обробку пакетів.",
        "QoS DSCP 46 — пріоритет пакетів гри у твоїй мережі (Wi-Fi, роутер).",
        "High-пріоритет і вимкнення EcoQoS для гри.",
        "Тест пінгу/джитеру/втрат — щоб вибрати найкращий сервер або регіон гри.",
    };
    for (auto* it : items) {
        Dot(kGood);
        ImGui::TextWrapped("%s", it);
    }
    ImGui::Spacing();
    ImGui::PushFont(g_fontTitle);
    ImGui::TextColored(kWarn, "Обмеження");
    ImGui::PopFont();
    ImGui::TextWrapped(
        "На Windows 10 версії 2004+ роздільність таймера діє лише на процес, що її задав (на Windows 11 це "
        "виправляє «глобальний таймер»). QoS працює тільки в межах твоєї мережі. Нейгл впливає лише на TCP — "
        "більшість шутерів працюють на UDP.");
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
    ImGui::TextColored(kAccent, "LUKASH");
    ImGui::SameLine(0, S(6));
    ImGui::TextUnformatted("BOOST");
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextUnformatted("timer + net optimizer");
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, S(14)));

    const char* names[PageCount] = {"Головна", "Timer Resolution", "Тест мережі", "Твіки", "Режим гри",
                                    "Як це працює"};
    for (int i = 0; i < PageCount; ++i) {
        bool sel = a.page == i;
        ImGui::PushStyleColor(ImGuiCol_Button, sel ? Fade(kAccent, 0.18f) : ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Fade(kAccent, sel ? 0.25f : 0.10f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, Fade(kAccent, 0.30f));
        ImGui::PushStyleColor(ImGuiCol_Text, sel ? kAccent : ImVec4(0.85f, 0.87f, 0.92f, 1));
        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(14), S(10)));
        if (ImGui::Button(names[i], ImVec2(-1, 0))) a.page = (Page)i;
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(4);
    }

    // Статус внизу панелі
    float bottom = ImGui::GetWindowHeight() - S(18) - ImGui::GetTextLineHeightWithSpacing() * 3;
    if (ImGui::GetCursorPosY() < bottom) ImGui::SetCursorPosY(bottom);
    Dot(a.elevated ? kGood : kBad);
    ImGui::TextColored(a.elevated ? kMuted : kBad, a.elevated ? "Адміністратор" : "Без прав адміна");
    Dot(a.gameTask.running ? kGood : kMuted);
    ImGui::TextColored(kMuted, a.gameTask.running ? "Буст активний" : "Буст вимкнено");
    ImGui::TextColored(kMuted, "Windows build %lu", a.build);
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    ImGui::SameLine(0, 0);

    // --- Основна область ---
    ImGui::BeginGroup();
    float logH = S(150);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(24), S(20)));
    ImGui::BeginChild("main", ImVec2(0, -logH), ImGuiChildFlags_AlwaysUseWindowPadding);
    if (!a.elevated) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Fade(kBad, 0.15f));
        ImGui::BeginChild("warn", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::TextColored(kBad, "Програма запущена без прав адміністратора — твіки реєстру і режим гри не спрацюють.");
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }
    switch (a.page) {
        case PageHome: PageHomeUi(a); break;
        case PageTimer: PageTimerUi(a); break;
        case PageNet: PageNetUi(a); break;
        case PageTweaks: PageTweaksUi(a); break;
        case PageGame: PageGameUi(a); break;
        default: PageInfoUi(a); break;
    }
    ImGui::EndChild();

    // --- Лог ---
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.040f, 0.045f, 0.062f, 1));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(16), S(10)));
    ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::TextColored(kMuted, "ЖУРНАЛ");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - S(70));
    if (ImGui::SmallButton("Очистити")) {
        std::lock_guard<std::mutex> l(a.logMutex);
        a.log.clear();
    }
    ImGui::BeginChild("logtext", ImVec2(0, 0));
    {
        std::lock_guard<std::mutex> l(a.logMutex);
        for (auto& line : a.log) {
            ImVec4 c = ImVec4(0.80f, 0.83f, 0.88f, 1);
            if (line.rfind("[OK]", 0) == 0) c = kGood;
            else if (line.rfind("[!]", 0) == 0 || line.rfind("[ПОМИЛКА]", 0) == 0) c = kBad;
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
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int) {
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
        MessageBoxW(nullptr, L"Не вдалося завантажити функції таймера з ntdll.dll", L"LukashBoost", MB_ICONERROR);
        return 1;
    }
    app.elevated = lb::IsElevated();
    app.build = lb::WindowsBuild();
    lb::Log("LukashBoost запущено. Windows build %lu%s", app.build,
            app.elevated ? "" : " — [!] без прав адміністратора");

    WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, WndProc, 0, 0, inst, LoadIconW(inst, MAKEINTRESOURCEW(1)), nullptr,
                   nullptr, nullptr, L"LukashBoostWnd", nullptr};
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, (LPCWSTR)IDI_APPLICATION);
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    RegisterClassExW(&wc);
    int w = (int)(1180 * g_dpi), h = (int)(780 * g_dpi);
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2, y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"LukashBoost — Timer Resolution + Net", WS_OVERLAPPEDWINDOW,
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
        MessageBoxW(nullptr, L"Не вдалося ініціалізувати DirectX 11", L"LukashBoost", MB_ICONERROR);
        return 1;
    }
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

    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;
        // Згорнуте/закрите вікно — не малюємо, щоб не навантажувати ПК під час гри
        if (IsIconic(hwnd) || (g_occluded && g_swapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED)) {
            Sleep(50);
            continue;
        }
        g_occluded = false;
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
        g_occluded = g_swapChain->Present(1, 0) == DXGI_STATUS_OCCLUDED;
    }

    // Зупиняємо фонові задачі
    app.gameStop = true;
    app.gameTask.Join();
    app.netTask.Join();
    app.timerTask.Join();
    app.tweakTask.Join();
    lb::SetLogSink(nullptr);
    lb::Shutdown();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDevice();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}
