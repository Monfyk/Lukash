// KairoBoost — консольна версія (меню + командний рядок)
#include "core.h"
#include "advisor.h"
#include "dns.h"
#include "fortnite.h"
#include "tweaks.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace lb;

static std::atomic<bool> g_stop{false};

static BOOL WINAPI CtrlHandler(DWORD) {
    g_stop = true;
    return TRUE;
}

static std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n\"");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n\"");
    return s.substr(a, b - a + 1);
}

static std::string ReadLine(const char* prompt) {
    printf("%s", prompt);
    fflush(stdout);
    char buf[1024];
    if (!fgets(buf, sizeof(buf), stdin)) return "";
    return Trim(buf);
}

static void Pause() { ReadLine("\nНатисни Enter, щоб продовжити..."); }

static ULONG ParseResolutionMs(const std::string& s, ULONG def) {
    double ms = s.empty() ? 0 : atof(s.c_str());
    return ms > 0 ? (ULONG)std::lround(ms * 10000.0) : def;
}

static std::vector<std::string> DefaultExclude() {
    return {fn::kExe, "EasyAntiCheat_EOS.exe", "audiodg.exe", "obs64.exe", "Discord.exe", "KairoBoost-cli.exe"};
}

static void PrintTimerStatus() {
    ULONG coarsest, finest, current;
    if (!TimerQuery(coarsest, finest, current)) return;
    printf("  Timer Resolution: %.4f мс (діапазон %.4f ... %.4f мс)", current / 10000.0, finest / 10000.0,
           coarsest / 10000.0);
    if (TimerHeld()) printf(", KairoBoost тримає %.4f мс", TimerHeld() / 10000.0);
    printf("\n");
}

static void PrintTweaks() {
    const auto& all = tw::All();
    for (size_t i = 0; i < all.size(); ++i) {
        const auto& t = all[i];
        std::string why = tw::Unavailable(t);
        printf("  %2zu) [%s] %-12s %s%s\n", i + 1, !why.empty() ? "--" : (tw::IsOn(t) ? "ON" : "  "),
               tw::CategoryName(t.category), t.name, t.recommended ? "  *" : "");
    }
    printf("  (* — рекомендовано)\n");
}

static void RunGame(const std::string& exe, ULONG res, bool cleanRam) {
    printf("  Ctrl+C — зупинити режим гри\n");
    g_stop = false;
    SetConsoleCtrlHandler(CtrlHandler, TRUE);
    RunGameMode(exe, res, g_stop, nullptr, [cleanRam](DWORD) {
        if (cleanRam) CleanMemory(DefaultExclude(), false);
    });
    SetConsoleCtrlHandler(CtrlHandler, FALSE);
}

static void Compare(const std::vector<std::string>& targets, int count) {
    if (targets.size() == 1) {
        PingResult r = Ping(targets[0], count, [](int i, double rtt) {
            if (rtt >= 0) printf("    #%-3d %7.2f мс\n", i + 1, rtt);
            else printf("    #%-3d втрачено\n", i + 1);
        });
        LogPing(r);
        return;
    }
    std::vector<PingResult> res;
    for (auto& t : targets) {
        printf("  Перевіряю %s...\n", t.c_str());
        res.push_back(Ping(t, count));
    }
    std::sort(res.begin(), res.end(), [](const PingResult& a, const PingResult& b) { return a.Score() < b.Score(); });
    printf("\n  Результати (найкращий — зверху):\n");
    for (auto& r : res) LogPing(r);
}

static void FortnitePing(int count) {
    std::vector<std::string> hosts;
    for (auto& r : fn::Regions()) hosts.push_back(r.host);
    Compare(hosts, count);
}

static void DnsTest() {
    const auto& ps = dns::Providers();
    std::vector<std::pair<double, int>> res;
    printf("  Перевіряю DNS-сервери...\n");
    for (int i = 0; i < (int)ps.size(); ++i) {
        dns::Result r = dns::Measure(ps[i].v4[0], 12);
        res.push_back({r.avg < 0 ? 1e9 : r.avg, i});
    }
    std::sort(res.begin(), res.end());
    int cur = dns::CurrentProvider();
    for (size_t k = 0; k < res.size(); ++k) {
        const auto& p = ps[res[k].second];
        if (res[k].first >= 1e9)
            printf("  %zu) %-14s %-32s немає відповіді%s\n", k + 1, p.name, p.v4[0], res[k].second == cur ? "  [поточний]" : "");
        else
            printf("  %zu) %-14s %-16s %6.0f мс%s\n", k + 1, p.name, p.v4[0], res[k].first,
                   res[k].second == cur ? "  [поточний]" : "");
    }
}

static void DnsMenu() {
    printf("\n=== DNS ===\n");
    DnsTest();
    const auto& ps = dns::Providers();
    printf("\n  Встановити: ");
    for (size_t i = 0; i < ps.size(); ++i) printf("%zu=%s  ", i + 1, ps[i].name);
    std::string c = ReadLine("\n  Номер провайдера, a — автоматично/як було, Enter — назад: ");
    if (c == "a") {
        dns::Restore();
    } else {
        int n = atoi(c.c_str());
        if (n > 0 && n <= (int)ps.size()) dns::Apply(ps[n - 1]);
    }
}

static void Advisor() {
    printf("\n=== Порадник ===\n  Аналізую ПК...\n\n");
    adv::SystemInfo info = adv::Collect();
    printf("%s\n", info.Summary().c_str());
    int lastLevel = -1;
    for (auto& x : adv::Analyze(info)) {
        if (x.level != lastLevel) {
            printf("\n-- %s --\n", adv::LevelName(x.level));
            lastLevel = x.level;
        }
        printf("  * %s%s\n    %s\n", x.title.c_str(),
               x.action == adv::EnableTweak ? ("  [твік: " + x.tweakId + "]").c_str() : "", x.text.c_str());
    }
}

static void TweaksMenu() {
    for (;;) {
        printf("\n=== Твіки ===\n");
        PrintTweaks();
        std::string c = ReadLine("\n  Номер — увімкнути/вимкнути, r — рекомендовані, x — відкотити все, 0 — назад: ");
        if (c == "r") {
            tw::ApplyRecommended(true);
        } else if (c == "x") {
            if (ReadLine("  Точно? (y/n): ") == "y") tw::RevertEverything();
        } else {
            int n = atoi(c.c_str());
            const auto& all = tw::All();
            if (n <= 0 || n > (int)all.size()) return;
            const auto& t = all[n - 1];
            if (tw::IsOn(t)) tw::Revert(t); else tw::Apply(t);
        }
    }
}

static void MainMenu() {
    for (;;) {
        printf("\n=== KairoBoost ===\n");
        if (!IsElevated()) printf("  [!] Без прав адміністратора твіки не спрацюють.\n");
        PrintTimerStatus();
        printf("  1) Твіки\n  2) Timer Resolution: авто-підбір\n  3) Тест Sleep(1)\n  4) Тест мережі\n"
               "  5) Пінг до серверів Fortnite\n  6) Очистка ОЗУ\n  7) Режим гри\n  8) Очистити DNS-кеш\n  9) Вибір DNS\n  0) Вихід\n");
        std::string c = ReadLine("> ");
        if (c == "1") {
            TweaksMenu();
        } else if (c == "2") {
            AutoTuneResolution();
            Pause();
        } else if (c == "3") {
            SleepStats s = MeasureSleep1(300);
            printf("  Sleep(1): сер %.3f мс, мін %.3f, макс %.3f\n", s.avg, s.min, s.max);
            Pause();
        } else if (c == "4") {
            auto t = Split(ReadLine("  Сервери (напр. 1.1.1.1 8.8.8.8 google.com:443): "));
            int n = atoi(ReadLine("  Кількість запитів [20]: ").c_str());
            if (!t.empty()) Compare(t, n > 0 ? n : 20);
            Pause();
        } else if (c == "5") {
            FortnitePing(20);
            Pause();
        } else if (c == "6") {
            CleanMemory(DefaultExclude());
            Pause();
        } else if (c == "7") {
            std::string exe = ReadLine("  exe гри [FortniteClient-Win64-Shipping.exe]: ");
            if (exe.empty()) exe = fn::kExe;
            RunGame(exe, ParseResolutionMs(ReadLine("  Timer Resolution в мс [0.5]: "), 5000), true);
            Pause();
        } else if (c == "8") {
            FlushDns();
            Pause();
        } else if (c == "a") {
            Advisor();
            Pause();
        } else if (c == "9") {
            DnsMenu();
            Pause();
        } else if (c == "0") {
            return;
        }
    }
}

static void Usage() {
    printf("KairoBoost-cli.exe                               меню\n"
           "  --res 0.5                                      тримати Timer Resolution\n"
           "  --game [exe] [--res 0.5] [--clean-ram]          режим гри (типово Fortnite)\n"
           "  --ping host[:port] [...] [--count 20]          тест / порівняння серверів\n"
           "  --fortnite-ping                                пінг до регіонів Fortnite\n"
           "  --apply-recommended | --revert-all | --status\n"
           "  --clean-ram                                    очистити ОЗУ\n"
           "  --dns-test | --dns Cloudflare | --dns-auto     тест / вибір DNS\n"
           "  --advisor                                      порадник для твого ПК\n");
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    if (!Init()) {
        printf("Не вдалося завантажити функції таймера з ntdll.dll\n");
        return 1;
    }
    if (argc <= 1) {
        MainMenu();
        Shutdown();
        return 0;
    }
    std::string game;
    bool gameMode = false, cleanRam = false, apply = false, revert = false, status = false, fnPing = false;
    bool dnsTest = false, dnsAuto = false, advisor = false;
    std::string dnsSet;
    ULONG res = 0;
    int count = 20;
    std::vector<std::string> pings;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--res" && i + 1 < argc) res = ParseResolutionMs(argv[++i], 5000);
        else if (a == "--game") {
            gameMode = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') game = argv[++i];
        } else if (a == "--clean-ram") cleanRam = true;
        else if (a == "--apply-recommended") apply = true;
        else if (a == "--revert-all") revert = true;
        else if (a == "--status") status = true;
        else if (a == "--fortnite-ping") fnPing = true;
        else if (a == "--dns-test") dnsTest = true;
        else if (a == "--advisor") advisor = true;
        else if (a == "--dns-auto") dnsAuto = true;
        else if (a == "--dns" && i + 1 < argc) dnsSet = argv[++i];
        else if (a == "--count" && i + 1 < argc) count = std::max(1, atoi(argv[++i]));
        else if (a == "--ping") {
            while (i + 1 < argc && argv[i + 1][0] != '-') pings.push_back(argv[++i]);
        } else {
            Usage();
            return 1;
        }
    }
    if (revert) tw::RevertEverything();
    if (apply) tw::ApplyRecommended(true);
    if (status) {
        PrintTimerStatus();
        PrintTweaks();
    }
    if (!pings.empty()) Compare(pings, count);
    if (fnPing) FortnitePing(count);
    if (dnsTest) DnsTest();
    if (advisor) Advisor();
    if (dnsAuto) dns::Restore();
    if (!dnsSet.empty()) {
        bool found = false;
        for (auto& p : dns::Providers())
            if (ToLower(p.name) == ToLower(dnsSet)) {
                dns::Apply(p);
                found = true;
            }
        if (!found) printf("Невідомий DNS-провайдер: %s\n", dnsSet.c_str());
    }
    if (gameMode) {
        RunGame(game.empty() ? fn::kExe : game, res ? res : 5000, cleanRam);
    } else {
        if (cleanRam) CleanMemory(DefaultExclude());
        if (res) {
            ULONG actual = 0;
            if (!TimerHold(res, &actual)) {
                printf("Не вдалося встановити Timer Resolution\n");
                return 1;
            }
            printf("Timer Resolution: %.4f мс. Ctrl+C — вихід.\n", actual / 10000.0);
            SetConsoleCtrlHandler(CtrlHandler, TRUE);
            while (!g_stop) Sleep(500);
        }
    }
    Shutdown();
    return 0;
}
