// LukashBoost — консольна версія (меню + командний рядок)
#include "core.h"

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

static void PrintTimerStatus() {
    ULONG coarsest, finest, current;
    if (!TimerQuery(coarsest, finest, current)) {
        printf("  Не вдалося прочитати роздільність таймера.\n");
        return;
    }
    printf("  Поточна роздільність: %.4f мс (діапазон %.4f ... %.4f мс)\n", current / 10000.0, finest / 10000.0,
           coarsest / 10000.0);
    ULONG held = TimerHeld();
    if (held) printf("  LukashBoost тримає:   %.4f мс\n", held / 10000.0);
}

static void PrintStatus() {
    PrintTimerStatus();
    TweakStatus s = GetTweakStatus();
    printf("  Нейгл вимкнено:       %d з %d інтерфейсів\n", s.nagleOff, s.ifaces);
    printf("  Network throttling:   %s\n", s.throttlingOff ? "вимкнено" : "за замовчуванням");
    printf("  SystemResponsiveness: %lu\n", s.sysResponsiveness);
    printf("  MMCSS для ігор:       %s\n", s.mmcss ? "так" : "ні");
    printf("  Глобальний таймер:    %s\n", s.globalTimer ? "увімкнено" : "вимкнено");
    for (auto& g : s.qosGames) printf("  QoS DSCP 46:          %s\n", g.c_str());
}

static void RunGame(const std::string& exe, ULONG res) {
    printf("  Ctrl+C — зупинити режим гри\n");
    g_stop = false;
    SetConsoleCtrlHandler(CtrlHandler, TRUE);
    RunGameMode(exe, res, g_stop);
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

static void TimerMenu() {
    for (;;) {
        printf("\n=== Timer Resolution ===\n");
        PrintTimerStatus();
        printf("  1) 0.5 мс   2) Своє значення   3) Авто-підбір   4) Тест Sleep(1)\n"
               "  5) Відпустити   6) Глобальний таймер (Windows 11)   0) Назад\n");
        std::string c = ReadLine("> ");
        if (c == "1") {
            printf("  %s\n", TimerHold(5000) ? "Готово." : "Помилка.");
        } else if (c == "2") {
            ULONG v = ParseResolutionMs(ReadLine("  Значення в мс (напр. 0.507): "), 0);
            if (v) printf("  %s\n", TimerHold(v) ? "Готово." : "Помилка.");
        } else if (c == "3") {
            AutoTuneResolution();
        } else if (c == "4") {
            SleepStats s = MeasureSleep1(300);
            printf("  Sleep(1): сер %.3f мс, мін %.3f, макс %.3f\n", s.avg, s.min, s.max);
        } else if (c == "5") {
            TimerRelease();
        } else if (c == "6") {
            EnableGlobalTimerResolution();
        } else {
            return;
        }
    }
}

static void MainMenu() {
    for (;;) {
        printf("\n=== LukashBoost ===\n");
        if (!IsElevated()) printf("  [!] Без прав адміністратора твіки не спрацюють.\n");
        printf("  1) Timer Resolution\n  2) Тест мережі\n  3) Мережеві твіки\n  4) QoS для гри\n"
               "  5) Режим гри\n  6) Стан\n  7) Відкотити все\n  8) Очистити DNS\n  0) Вихід\n");
        std::string c = ReadLine("> ");
        if (c == "1") {
            TimerMenu();
        } else if (c == "2") {
            auto t = Split(ReadLine("  Сервери (напр. 1.1.1.1 8.8.8.8 google.com:443): "));
            int n = atoi(ReadLine("  Кількість запитів [20]: ").c_str());
            if (!t.empty()) Compare(t, n > 0 ? n : 20);
            Pause();
        } else if (c == "3") {
            ApplyNetworkTweaks();
            Pause();
        } else if (c == "4") {
            ApplyQosForGame(ReadLine("  exe гри (напр. cs2.exe): "));
            Pause();
        } else if (c == "5") {
            std::string exe = ReadLine("  exe гри (напр. cs2.exe): ");
            if (!exe.empty()) RunGame(exe, ParseResolutionMs(ReadLine("  Timer Resolution в мс [0.5]: "), 5000));
            Pause();
        } else if (c == "6") {
            PrintStatus();
            Pause();
        } else if (c == "7") {
            if (ReadLine("  Точно? (y/n): ") == "y") RevertAll();
            Pause();
        } else if (c == "8") {
            FlushDns();
            Pause();
        } else if (c == "0") {
            return;
        }
    }
}

static void Usage() {
    printf("LukashBoost-cli.exe                              меню\n"
           "  --res 0.5                                      тримати Timer Resolution\n"
           "  --game cs2.exe [--res 0.5] [--qos]             режим гри\n"
           "  --ping host[:port] [...] [--count 20]          тест / порівняння серверів\n"
           "  --apply | --revert | --status\n");
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
    ULONG res = 0;
    bool qos = false, apply = false, revert = false, status = false;
    int count = 20;
    std::vector<std::string> pings;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--res" && i + 1 < argc) res = ParseResolutionMs(argv[++i], 5000);
        else if (a == "--game" && i + 1 < argc) game = argv[++i];
        else if (a == "--qos") qos = true;
        else if (a == "--apply") apply = true;
        else if (a == "--revert") revert = true;
        else if (a == "--status") status = true;
        else if (a == "--count" && i + 1 < argc) count = std::max(1, atoi(argv[++i]));
        else if (a == "--ping") {
            while (i + 1 < argc && argv[i + 1][0] != '-') pings.push_back(argv[++i]);
        } else {
            Usage();
            return 1;
        }
    }
    if (revert) RevertAll();
    if (apply) ApplyNetworkTweaks();
    if (status) PrintStatus();
    if (!pings.empty()) Compare(pings, count);
    if (!game.empty()) {
        if (qos) ApplyQosForGame(game);
        RunGame(game, res ? res : 5000);
    } else if (res) {
        ULONG actual = 0;
        if (!TimerHold(res, &actual)) {
            printf("Не вдалося встановити Timer Resolution\n");
            return 1;
        }
        printf("Timer Resolution: %.4f мс. Ctrl+C — вихід.\n", actual / 10000.0);
        SetConsoleCtrlHandler(CtrlHandler, TRUE);
        while (!g_stop) Sleep(500);
    }
    Shutdown();
    return 0;
}
