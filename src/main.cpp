// LukashBoost — Timer Resolution + мережева оптимізація для ігор (Windows 10/11)
//
// Що робить програма (тільки те, що реально працює на рівні ОС):
//   * Timer Resolution через NtSetTimerResolution (+ авто-підбір під Sleep(1) ≈ 1 мс,
//     + вимкнення "power throttling" таймера, + ключ GlobalTimerResolutionRequests для Win11)
//   * Тест мережі: ping (ICMP) і TCP-ping, мінімум/середнє/максимум/джитер/втрати,
//     порівняння кількох серверів і вибір найкращого
//   * Мережеві твіки з резервною копією і відкатом: вимкнення алгоритму Нейгла
//     (TcpAckFrequency/TCPNoDelay), NetworkThrottlingIndex, SystemResponsiveness,
//     пріоритети MMCSS для ігор
//   * QoS DSCP 46 (пріоритетна черга) для exe гри
//   * Режим гри: чекає запуск гри, ставить їй High пріоритет, вимикає EcoQoS/throttling,
//     тримає Timer Resolution, поки гра запущена
//
// Збирання: див. README.md (MSVC, MinGW або GitHub Actions).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "advapi32.lib")
#endif

static const char* kAppName = "LukashBoost";
static const char* kBackupKey = "SOFTWARE\\LukashBoost\\Backup";
static const char* kQosRoot = "SOFTWARE\\Policies\\Microsoft\\Windows\\QoS";
static const char* kQosPrefix = "LukashBoost_";

// ---------------------------------------------------------------------------
// Дрібні утиліти
// ---------------------------------------------------------------------------

static double NowMs() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
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

static bool AskYes(const char* prompt) {
    std::string a = ReadLine(prompt);
    return !a.empty() && (a[0] == 'y' || a[0] == 'Y' || a == "т" || a == "Т" ||
                          a.rfind("так", 0) == 0 || a.rfind("Так", 0) == 0);
}

static void Pause() { ReadLine("\nНатисни Enter, щоб продовжити..."); }

static std::string ToLower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

static bool IsElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION el{};
    DWORD sz = 0;
    BOOL ok = GetTokenInformation(token, TokenElevation, &el, sizeof(el), &sz);
    CloseHandle(token);
    return ok && el.TokenIsElevated;
}

static DWORD WindowsBuild() {
    typedef LONG(WINAPI * RtlGetVersion_t)(PRTL_OSVERSIONINFOW);
    auto fn = (RtlGetVersion_t)(void*)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion");
    RTL_OSVERSIONINFOW vi{};
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (fn && fn(&vi) == 0) return vi.dwBuildNumber;
    return 0;
}

// ---------------------------------------------------------------------------
// Power throttling (щоб Windows 11 не ігнорувала нашу роздільність таймера
// і не переводила гру в EcoQoS)
// ---------------------------------------------------------------------------

#ifndef PROCESS_POWER_THROTTLING_CURRENT_VERSION
#define PROCESS_POWER_THROTTLING_CURRENT_VERSION 1
typedef struct _PROCESS_POWER_THROTTLING_STATE {
    ULONG Version;
    ULONG ControlMask;
    ULONG StateMask;
} PROCESS_POWER_THROTTLING_STATE;
#endif
#ifndef PROCESS_POWER_THROTTLING_EXECUTION_SPEED
#define PROCESS_POWER_THROTTLING_EXECUTION_SPEED 0x1
#endif
#ifndef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
#endif

static bool DisablePowerThrottling(HANDLE process, ULONG mask) {
    typedef BOOL(WINAPI * SetProcessInformation_t)(HANDLE, int, LPVOID, DWORD);
    auto fn = (SetProcessInformation_t)(void*)GetProcAddress(GetModuleHandleA("kernel32.dll"),
                                                              "SetProcessInformation");
    if (!fn) return false;
    PROCESS_POWER_THROTTLING_STATE st{};
    st.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    st.ControlMask = mask;  // беремо під контроль ці прапорці...
    st.StateMask = 0;       // ...і вимикаємо їх
    const int ProcessPowerThrottling = 4;  // PROCESS_INFORMATION_CLASS
    return fn(process, ProcessPowerThrottling, &st, sizeof(st)) != FALSE;
}

// ---------------------------------------------------------------------------
// Timer Resolution
// ---------------------------------------------------------------------------

typedef LONG(NTAPI* NtQueryTimerResolution_t)(PULONG Coarsest, PULONG Finest, PULONG Current);
typedef LONG(NTAPI* NtSetTimerResolution_t)(ULONG Desired, BOOLEAN Set, PULONG Current);

struct TimerApi {
    NtQueryTimerResolution_t query = nullptr;
    NtSetTimerResolution_t set = nullptr;
    ULONG held = 0;  // 0 = нічого не тримаємо

    bool Load() {
        HMODULE nt = GetModuleHandleA("ntdll.dll");
        query = (NtQueryTimerResolution_t)(void*)GetProcAddress(nt, "NtQueryTimerResolution");
        set = (NtSetTimerResolution_t)(void*)GetProcAddress(nt, "NtSetTimerResolution");
        return query && set;
    }
    bool Query(ULONG& coarsest, ULONG& finest, ULONG& current) {
        return query && query(&coarsest, &finest, &current) == 0;
    }
    // value у одиницях 100 нс (5000 = 0.5 мс)
    bool Hold(ULONG value, ULONG* actual = nullptr) {
        Release();
        ULONG cur = 0;
        if (set(value, TRUE, &cur) != 0) return false;
        held = value;
        if (actual) *actual = cur;
        return true;
    }
    void Release() {
        if (held) {
            ULONG cur;
            set(held, FALSE, &cur);
            held = 0;
        }
    }
};

static TimerApi g_timer;

struct SleepStats {
    double avg = 0, max = 0, min = 1e9;
};

static SleepStats MeasureSleep1(int samples) {
    SleepStats s;
    double sum = 0;
    for (int i = 0; i < samples; ++i) {
        double t0 = NowMs();
        Sleep(1);
        double d = NowMs() - t0;
        sum += d;
        s.max = std::max(s.max, d);
        s.min = std::min(s.min, d);
    }
    s.avg = sum / samples;
    return s;
}

static void PrintTimerStatus() {
    ULONG coarsest, finest, current;
    if (!g_timer.Query(coarsest, finest, current)) {
        printf("  Не вдалося прочитати роздільність таймера.\n");
        return;
    }
    printf("  Поточна системна роздільність: %.4f мс\n", current / 10000.0);
    printf("  Доступний діапазон:            %.4f мс ... %.4f мс\n", finest / 10000.0, coarsest / 10000.0);
    if (g_timer.held)
        printf("  LukashBoost тримає:            %.4f мс\n", g_timer.held / 10000.0);
    else
        printf("  LukashBoost зараз нічого не тримає.\n");
}

// Підбирає значення, при якому Sleep(1) найближчий до 1.0 мс (часто 0.50-0.51 мс).
static ULONG AutoTuneResolution() {
    ULONG coarsest, finest, current;
    if (!g_timer.Query(coarsest, finest, current)) return 5000;
    ULONG best = std::max<ULONG>(finest, 5000);
    double bestScore = 1e9;
    printf("  Підбираю найкраще значення (кілька секунд)...\n");
    for (ULONG v = std::max<ULONG>(finest, 5000); v <= 5200; v += 20) {
        if (!g_timer.Hold(v)) continue;
        Sleep(50);
        SleepStats s = MeasureSleep1(150);
        double score = std::fabs(s.avg - 1.0) + 0.1 * (s.max - s.avg);
        printf("    %.4f мс -> Sleep(1) = %.3f мс (max %.3f)\n", v / 10000.0, s.avg, s.max);
        if (score < bestScore) {
            bestScore = score;
            best = v;
        }
    }
    g_timer.Release();
    printf("  Найкраще: %.4f мс\n", best / 10000.0);
    return best;
}

// ---------------------------------------------------------------------------
// Реєстр з резервною копією
// ---------------------------------------------------------------------------

static const char* RootName(HKEY root) { return root == HKEY_LOCAL_MACHINE ? "HKLM" : "HKCU"; }

// Перед першою зміною значення зберігаємо оригінал (або позначку "не існувало").
static void BackupValue(HKEY root, const std::string& path, const std::string& name) {
    HKEY bk;
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, kBackupKey, 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &bk,
                        nullptr) != ERROR_SUCCESS)
        return;
    std::string id = std::string(RootName(root)) + "|" + path + "|" + name;
    if (RegQueryValueExA(bk, id.c_str(), nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
        RegCloseKey(bk);  // вже є копія оригіналу — не перезаписуємо
        return;
    }
    std::vector<BYTE> blob(4, 0);
    DWORD type = 0xFFFFFFFF;  // "значення не існувало"
    HKEY k;
    if (RegOpenKeyExA(root, path.c_str(), 0, KEY_READ, &k) == ERROR_SUCCESS) {
        DWORD t = 0, sz = 0;
        if (RegQueryValueExA(k, name.c_str(), nullptr, &t, nullptr, &sz) == ERROR_SUCCESS) {
            blob.resize(4 + sz);
            if (RegQueryValueExA(k, name.c_str(), nullptr, &t, blob.data() + 4, &sz) == ERROR_SUCCESS) {
                type = t;
                blob.resize(4 + sz);
            } else {
                blob.resize(4);
            }
        }
        RegCloseKey(k);
    }
    memcpy(blob.data(), &type, 4);
    RegSetValueExA(bk, id.c_str(), 0, REG_BINARY, blob.data(), (DWORD)blob.size());
    RegCloseKey(bk);
}

static bool SetDword(HKEY root, const std::string& path, const std::string& name, DWORD v, bool backup = true) {
    if (backup) BackupValue(root, path, name);
    HKEY k;
    if (RegCreateKeyExA(root, path.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return false;
    bool ok = RegSetValueExA(k, name.c_str(), 0, REG_DWORD, (const BYTE*)&v, sizeof(v)) == ERROR_SUCCESS;
    RegCloseKey(k);
    return ok;
}

static bool SetString(HKEY root, const std::string& path, const std::string& name, const std::string& v,
                      bool backup = true) {
    if (backup) BackupValue(root, path, name);
    HKEY k;
    if (RegCreateKeyExA(root, path.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return false;
    bool ok = RegSetValueExA(k, name.c_str(), 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)v.size() + 1) ==
              ERROR_SUCCESS;
    RegCloseKey(k);
    return ok;
}

static bool GetDword(HKEY root, const std::string& path, const std::string& name, DWORD& out) {
    DWORD sz = sizeof(out);
    return RegGetValueA(root, path.c_str(), name.c_str(), RRF_RT_REG_DWORD, nullptr, &out, &sz) == ERROR_SUCCESS;
}

static std::vector<std::string> SubKeys(HKEY root, const std::string& path) {
    std::vector<std::string> out;
    HKEY k;
    if (RegOpenKeyExA(root, path.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return out;
    char name[256];
    for (DWORD i = 0;; ++i) {
        DWORD len = sizeof(name);
        if (RegEnumKeyExA(k, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        out.push_back(name);
    }
    RegCloseKey(k);
    return out;
}

static int RestoreBackup() {
    HKEY bk;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, kBackupKey, 0, KEY_READ, &bk) != ERROR_SUCCESS) return 0;
    int restored = 0;
    std::vector<std::pair<std::string, std::vector<BYTE>>> items;
    for (DWORD i = 0;; ++i) {
        char name[2048];
        DWORD nlen = sizeof(name), dlen = 0;
        if (RegEnumValueA(bk, i, name, &nlen, nullptr, nullptr, nullptr, &dlen) != ERROR_SUCCESS) break;
        std::vector<BYTE> data(dlen);
        nlen = sizeof(name);
        if (RegEnumValueA(bk, i, name, &nlen, nullptr, nullptr, data.data(), &dlen) != ERROR_SUCCESS) continue;
        data.resize(dlen);
        items.emplace_back(name, data);
    }
    RegCloseKey(bk);

    for (auto& it : items) {
        const std::string& id = it.first;
        size_t p1 = id.find('|'), p2 = id.rfind('|');
        if (p1 == std::string::npos || p2 == p1 || it.second.size() < 4) continue;
        HKEY root = id.substr(0, p1) == "HKLM" ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
        std::string path = id.substr(p1 + 1, p2 - p1 - 1);
        std::string name = id.substr(p2 + 1);
        DWORD type;
        memcpy(&type, it.second.data(), 4);
        HKEY k;
        if (RegOpenKeyExA(root, path.c_str(), 0, KEY_WRITE, &k) != ERROR_SUCCESS) continue;
        if (type == 0xFFFFFFFF)
            RegDeleteValueA(k, name.c_str());
        else
            RegSetValueExA(k, name.c_str(), 0, type, it.second.data() + 4, (DWORD)it.second.size() - 4);
        RegCloseKey(k);
        ++restored;
    }
    RegDeleteKeyA(HKEY_LOCAL_MACHINE, kBackupKey);
    return restored;
}

// ---------------------------------------------------------------------------
// Мережеві та системні твіки
// ---------------------------------------------------------------------------

static const char* kTcpIfaces = "SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces";
static const char* kSysProfile = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile";
static const char* kGamesTask =
    "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile\\Tasks\\Games";
static const char* kKernel = "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\kernel";

static bool InterfaceHasIp(const std::string& guid) {
    std::string path = std::string(kTcpIfaces) + "\\" + guid;
    char buf[512];
    DWORD sz = sizeof(buf);
    if (RegGetValueA(HKEY_LOCAL_MACHINE, path.c_str(), "DhcpIPAddress", RRF_RT_REG_SZ, nullptr, buf, &sz) ==
            ERROR_SUCCESS &&
        strcmp(buf, "0.0.0.0") != 0 && buf[0])
        return true;
    sz = sizeof(buf);
    if (RegGetValueA(HKEY_LOCAL_MACHINE, path.c_str(), "IPAddress", RRF_RT_REG_MULTI_SZ, nullptr, buf, &sz) ==
            ERROR_SUCCESS &&
        buf[0] && strcmp(buf, "0.0.0.0") != 0)
        return true;
    return false;
}

static void ApplyNetworkTweaks() {
    int n = 0;
    for (auto& guid : SubKeys(HKEY_LOCAL_MACHINE, kTcpIfaces)) {
        if (!InterfaceHasIp(guid)) continue;
        std::string path = std::string(kTcpIfaces) + "\\" + guid;
        bool ok = SetDword(HKEY_LOCAL_MACHINE, path, "TcpAckFrequency", 1) &&
                  SetDword(HKEY_LOCAL_MACHINE, path, "TCPNoDelay", 1) &&
                  SetDword(HKEY_LOCAL_MACHINE, path, "TcpDelAckTicks", 0);
        printf("  [%s] Нейгл вимкнено для інтерфейсу %s\n", ok ? "OK" : "ПОМИЛКА", guid.c_str());
        ++n;
    }
    if (!n) printf("  [!] Не знайдено активних мережевих інтерфейсів з IP.\n");

    bool ok = SetDword(HKEY_LOCAL_MACHINE, kSysProfile, "NetworkThrottlingIndex", 0xFFFFFFFF);
    printf("  [%s] NetworkThrottlingIndex = вимкнено\n", ok ? "OK" : "ПОМИЛКА");
    ok = SetDword(HKEY_LOCAL_MACHINE, kSysProfile, "SystemResponsiveness", 10);
    printf("  [%s] SystemResponsiveness = 10 (мінімум, який приймає Windows)\n", ok ? "OK" : "ПОМИЛКА");

    ok = SetDword(HKEY_LOCAL_MACHINE, kGamesTask, "GPU Priority", 8) &&
         SetDword(HKEY_LOCAL_MACHINE, kGamesTask, "Priority", 6) &&
         SetString(HKEY_LOCAL_MACHINE, kGamesTask, "Scheduling Category", "High") &&
         SetString(HKEY_LOCAL_MACHINE, kGamesTask, "SFIO Priority", "High");
    printf("  [%s] MMCSS пріоритети для ігор (GPU 8, CPU 6, High)\n", ok ? "OK" : "ПОМИЛКА");

    printf("\n  Частина змін (Нейгл, MMCSS) набуде чинності після ПЕРЕЗАВАНТАЖЕННЯ.\n");
}

static void EnableGlobalTimerResolution() {
    DWORD build = WindowsBuild();
    if (build < 20348) {
        printf("  Твоя збірка Windows (%lu) не підтримує GlobalTimerResolutionRequests.\n", build);
        printf("  На Windows 10 до 2004 роздільність і так глобальна; на 10 2004+ — лише на процес.\n");
        return;
    }
    bool ok = SetDword(HKEY_LOCAL_MACHINE, kKernel, "GlobalTimerResolutionRequests", 1);
    printf("  [%s] GlobalTimerResolutionRequests = 1 (потрібне ПЕРЕЗАВАНТАЖЕННЯ)\n", ok ? "OK" : "ПОМИЛКА");
}

static void ApplyQosForGame(const std::string& exe) {
    std::string path = std::string(kQosRoot) + "\\" + kQosPrefix + exe;
    // Ключ політики створюємо самі, тому окремий бекап не потрібен — відкат видаляє його цілком.
    bool ok = SetString(HKEY_LOCAL_MACHINE, path, "Version", "1.0", false) &&
              SetString(HKEY_LOCAL_MACHINE, path, "Application Name", exe, false) &&
              SetString(HKEY_LOCAL_MACHINE, path, "Protocol", "*", false) &&
              SetString(HKEY_LOCAL_MACHINE, path, "Local Port", "*", false) &&
              SetString(HKEY_LOCAL_MACHINE, path, "Local IP", "*", false) &&
              SetString(HKEY_LOCAL_MACHINE, path, "Local IP Prefix Length", "*", false) &&
              SetString(HKEY_LOCAL_MACHINE, path, "Remote Port", "*", false) &&
              SetString(HKEY_LOCAL_MACHINE, path, "Remote IP", "*", false) &&
              SetString(HKEY_LOCAL_MACHINE, path, "Remote IP Prefix Length", "*", false) &&
              SetString(HKEY_LOCAL_MACHINE, path, "DSCP Value", "46", false) &&
              SetString(HKEY_LOCAL_MACHINE, path, "Throttle Rate", "-1", false);
    // Без цього Windows застосовує DSCP лише в доменних мережах.
    ok = ok && SetString(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Services\\Tcpip\\QoS", "Do not use NLA",
                         "1");
    printf("  [%s] QoS DSCP 46 для %s\n", ok ? "OK" : "ПОМИЛКА", exe.c_str());
    if (ok) {
        printf("  Оновлюю групові політики (gpupdate)...\n");
        system("gpupdate /target:computer /force >nul 2>&1");
    }
}

static int RemoveQosPolicies() {
    int n = 0;
    for (auto& name : SubKeys(HKEY_LOCAL_MACHINE, kQosRoot)) {
        if (name.rfind(kQosPrefix, 0) != 0) continue;
        std::string path = std::string(kQosRoot) + "\\" + name;
        if (RegDeleteKeyA(HKEY_LOCAL_MACHINE, path.c_str()) == ERROR_SUCCESS) ++n;
    }
    return n;
}

static void RevertAll() {
    int r = RestoreBackup();
    int q = RemoveQosPolicies();
    printf("  Відновлено значень реєстру: %d, видалено QoS-політик: %d\n", r, q);
    if (q) system("gpupdate /target:computer /force >nul 2>&1");
    printf("  Перезавантаж ПК, щоб усе повернулося повністю.\n");
}

static void PrintTweakStatus() {
    DWORD v;
    int nagle = 0, total = 0;
    for (auto& guid : SubKeys(HKEY_LOCAL_MACHINE, kTcpIfaces)) {
        if (!InterfaceHasIp(guid)) continue;
        ++total;
        std::string path = std::string(kTcpIfaces) + "\\" + guid;
        if (GetDword(HKEY_LOCAL_MACHINE, path, "TcpAckFrequency", v) && v == 1 &&
            GetDword(HKEY_LOCAL_MACHINE, path, "TCPNoDelay", v) && v == 1)
            ++nagle;
    }
    printf("  Нейгл вимкнено:                %d з %d інтерфейсів\n", nagle, total);
    if (GetDword(HKEY_LOCAL_MACHINE, kSysProfile, "NetworkThrottlingIndex", v))
        printf("  NetworkThrottlingIndex:        %s\n", v == 0xFFFFFFFF ? "вимкнено" : std::to_string(v).c_str());
    else
        printf("  NetworkThrottlingIndex:        за замовчуванням (10)\n");
    if (GetDword(HKEY_LOCAL_MACHINE, kSysProfile, "SystemResponsiveness", v))
        printf("  SystemResponsiveness:          %lu\n", v);
    bool global = GetDword(HKEY_LOCAL_MACHINE, kKernel, "GlobalTimerResolutionRequests", v) && v == 1;
    printf("  GlobalTimerResolutionRequests: %s\n", global ? "увімкнено" : "вимкнено");
    int qos = 0;
    for (auto& name : SubKeys(HKEY_LOCAL_MACHINE, kQosRoot))
        if (name.rfind(kQosPrefix, 0) == 0) {
            printf("  QoS-політика:                  %s\n", name.c_str() + strlen(kQosPrefix));
            ++qos;
        }
    if (!qos) printf("  QoS-політик немає\n");
}

// ---------------------------------------------------------------------------
// Тест мережі
// ---------------------------------------------------------------------------

struct PingResult {
    std::string host;
    std::string ip;
    int sent = 0, received = 0;
    double min = 0, avg = 0, max = 0, jitter = 0;
    bool resolved = false;
};

static bool Resolve(const std::string& host, sockaddr_in& out) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
    out = *(sockaddr_in*)res->ai_addr;
    freeaddrinfo(res);
    return true;
}

static void Finish(PingResult& r, const std::vector<double>& rtts) {
    r.received = (int)rtts.size();
    if (rtts.empty()) return;
    r.min = *std::min_element(rtts.begin(), rtts.end());
    r.max = *std::max_element(rtts.begin(), rtts.end());
    double sum = 0, jit = 0;
    for (size_t i = 0; i < rtts.size(); ++i) {
        sum += rtts[i];
        if (i) jit += std::fabs(rtts[i] - rtts[i - 1]);
    }
    r.avg = sum / rtts.size();
    r.jitter = rtts.size() > 1 ? jit / (rtts.size() - 1) : 0;
}

static PingResult IcmpPing(const std::string& host, int count, bool verbose) {
    PingResult r;
    r.host = host;
    sockaddr_in addr{};
    if (!Resolve(host, addr)) return r;
    r.resolved = true;
    char ipbuf[64];
    inet_ntop(AF_INET, &addr.sin_addr, ipbuf, sizeof(ipbuf));
    r.ip = ipbuf;

    HANDLE h = IcmpCreateFile();
    if (h == INVALID_HANDLE_VALUE) return r;
    char payload[32] = "LukashBoost-ping";
    std::vector<BYTE> reply(sizeof(ICMP_ECHO_REPLY) + sizeof(payload) + 64);
    std::vector<double> rtts;
    for (int i = 0; i < count; ++i) {
        ++r.sent;
        double t0 = NowMs();
        DWORD n = IcmpSendEcho(h, addr.sin_addr.s_addr, payload, sizeof(payload), nullptr, reply.data(),
                               (DWORD)reply.size(), 2000);
        double dt = NowMs() - t0;
        auto* rep = (ICMP_ECHO_REPLY*)reply.data();
        if (n > 0 && rep->Status == IP_SUCCESS) {
            // RoundTripTime має точність 1 мс, тому беремо власний вимір QPC (мінус накладні витрати мізерні)
            rtts.push_back(dt);
            if (verbose) printf("    #%-3d %7.2f мс\n", i + 1, dt);
        } else if (verbose) {
            printf("    #%-3d втрачено\n", i + 1);
        }
        if (i + 1 < count) Sleep(200);
    }
    IcmpCloseHandle(h);
    Finish(r, rtts);
    return r;
}

// TCP "ping" — час встановлення з'єднання (для серверів, що блокують ICMP)
static PingResult TcpPing(const std::string& host, int port, int count, bool verbose) {
    PingResult r;
    r.host = host + ":" + std::to_string(port);
    sockaddr_in addr{};
    if (!Resolve(host, addr)) return r;
    r.resolved = true;
    addr.sin_port = htons((u_short)port);
    char ipbuf[64];
    inet_ntop(AF_INET, &addr.sin_addr, ipbuf, sizeof(ipbuf));
    r.ip = ipbuf;
    std::vector<double> rtts;
    for (int i = 0; i < count; ++i) {
        ++r.sent;
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) continue;
        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);
        double t0 = NowMs();
        connect(s, (sockaddr*)&addr, sizeof(addr));
        fd_set wfds, efds;
        FD_ZERO(&wfds);
        FD_ZERO(&efds);
        FD_SET(s, &wfds);
        FD_SET(s, &efds);
        timeval tv{2, 0};
        int sel = select(0, nullptr, &wfds, &efds, &tv);
        double dt = NowMs() - t0;
        bool ok = sel > 0 && FD_ISSET(s, &wfds) && !FD_ISSET(s, &efds);
        closesocket(s);
        if (ok) {
            rtts.push_back(dt);
            if (verbose) printf("    #%-3d %7.2f мс\n", i + 1, dt);
        } else if (verbose) {
            printf("    #%-3d немає відповіді\n", i + 1);
        }
        if (i + 1 < count) Sleep(200);
    }
    Finish(r, rtts);
    return r;
}

static void PrintPing(const PingResult& r) {
    if (!r.resolved) {
        printf("  %s: не вдалося знайти адресу\n", r.host.c_str());
        return;
    }
    double loss = r.sent ? 100.0 * (r.sent - r.received) / r.sent : 100.0;
    if (!r.received) {
        printf("  %s (%s): немає відповідей (100%% втрат)\n", r.host.c_str(), r.ip.c_str());
        return;
    }
    printf("  %s (%s)\n    мін %.1f / сер %.1f / макс %.1f мс, джитер %.1f мс, втрати %.0f%%\n", r.host.c_str(),
           r.ip.c_str(), r.min, r.avg, r.max, r.jitter, loss);
}

static std::vector<std::string> Split(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ' ' || c == ',' || c == ';' || c == '\t') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// Розбирає "host" або "host:port" (порт => TCP-ping)
static PingResult PingAny(const std::string& target, int count, bool verbose) {
    size_t c = target.rfind(':');
    if (c != std::string::npos && c + 1 < target.size()) {
        int port = atoi(target.c_str() + c + 1);
        if (port > 0 && port < 65536) return TcpPing(target.substr(0, c), port, count, verbose);
    }
    return IcmpPing(target, count, verbose);
}

static void CompareServers(const std::vector<std::string>& targets, int count) {
    std::vector<PingResult> res;
    for (auto& t : targets) {
        printf("  Перевіряю %s...\n", t.c_str());
        res.push_back(PingAny(t, count, false));
    }
    // Оцінка: середній пінг + 2*джитер + штраф за втрати
    auto score = [](const PingResult& r) {
        if (!r.received) return 1e12;
        double loss = 100.0 * (r.sent - r.received) / r.sent;
        return r.avg + 2 * r.jitter + loss * 10;
    };
    std::sort(res.begin(), res.end(), [&](const PingResult& a, const PingResult& b) { return score(a) < score(b); });
    printf("\n  Результати (найкращий — зверху):\n");
    for (auto& r : res) PrintPing(r);
}

// ---------------------------------------------------------------------------
// Режим гри
// ---------------------------------------------------------------------------

static DWORD FindProcess(const std::string& exe) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 pe{};
    pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    std::string want = ToLower(exe);
    if (Process32First(snap, &pe)) {
        do {
            if (ToLower(pe.szExeFile) == want) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

static volatile bool g_stop = false;

static BOOL WINAPI CtrlHandler(DWORD) {
    g_stop = true;
    return TRUE;
}

static void GameMode(const std::string& exe, ULONG resolution) {
    printf("\n  Режим гри для %s (Ctrl+C — вихід)\n", exe.c_str());
    ULONG actual = 0;
    if (g_timer.Hold(resolution, &actual))
        printf("  [OK] Timer Resolution утримується: %.4f мс\n", actual / 10000.0);
    else
        printf("  [!] Не вдалося встановити Timer Resolution\n");

    g_stop = false;
    SetConsoleCtrlHandler(CtrlHandler, TRUE);
    bool waitingShown = false;
    while (!g_stop) {
        DWORD pid = FindProcess(exe);
        if (!pid) {
            if (!waitingShown) {
                printf("  Чекаю на запуск %s...\n", exe.c_str());
                waitingShown = true;
            }
            Sleep(1000);
            continue;
        }
        waitingShown = false;
        printf("  Гру знайдено (PID %lu)\n", pid);
        HANDLE h = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
        if (!h) {
            printf("  [!] Немає доступу до процесу (запусти від адміністратора). Код %lu\n", GetLastError());
            Sleep(3000);
            continue;
        }
        printf("  [%s] Пріоритет процесу: High\n", SetPriorityClass(h, HIGH_PRIORITY_CLASS) ? "OK" : "ПОМИЛКА");
        printf("  [%s] EcoQoS / power throttling для гри вимкнено\n",
               DisablePowerThrottling(h, PROCESS_POWER_THROTTLING_EXECUTION_SPEED |
                                             PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION)
                   ? "OK"
                   : "--");
        printf("  Працюю, поки гра запущена...\n");
        while (!g_stop && WaitForSingleObject(h, 500) == WAIT_TIMEOUT) {
        }
        CloseHandle(h);
        if (!g_stop) printf("  Гру закрито. Чекаю наступного запуску...\n");
    }
    SetConsoleCtrlHandler(CtrlHandler, FALSE);
    g_timer.Release();
    printf("\n  Режим гри зупинено, Timer Resolution відпущено.\n");
}

// ---------------------------------------------------------------------------
// Меню
// ---------------------------------------------------------------------------

static ULONG ParseResolutionMs(const std::string& s, ULONG def) {
    if (s.empty()) return def;
    double ms = atof(s.c_str());
    if (ms <= 0) return def;
    return (ULONG)std::lround(ms * 10000.0);
}

static void TimerMenu() {
    for (;;) {
        printf("\n=== Timer Resolution ===\n");
        PrintTimerStatus();
        printf("\n  1) Встановити 0.5 мс і тримати\n");
        printf("  2) Встановити своє значення\n");
        printf("  3) Авто-підбір (Sleep(1) максимально близький до 1 мс)\n");
        printf("  4) Тест точності Sleep(1)\n");
        printf("  5) Відпустити роздільність\n");
        printf("  6) Увімкнути GlobalTimerResolutionRequests (Windows 11, щоб діяло на ігри)\n");
        printf("  0) Назад\n");
        std::string c = ReadLine("> ");
        ULONG actual = 0;
        if (c == "1") {
            printf("  %s\n", g_timer.Hold(5000, &actual) ? "Готово." : "Помилка.");
        } else if (c == "2") {
            ULONG v = ParseResolutionMs(ReadLine("  Значення в мс (напр. 0.507): "), 0);
            if (v) printf("  %s\n", g_timer.Hold(v, &actual) ? "Готово." : "Помилка.");
        } else if (c == "3") {
            ULONG best = AutoTuneResolution();
            g_timer.Hold(best, &actual);
            printf("  Утримую %.4f мс\n", best / 10000.0);
        } else if (c == "4") {
            SleepStats s = MeasureSleep1(300);
            printf("  Sleep(1): сер %.3f мс, мін %.3f, макс %.3f\n", s.avg, s.min, s.max);
            printf("  (без зміни роздільності зазвичай ~15.6 мс або ~2 мс; з 0.5 мс — ~1.0-1.5 мс)\n");
        } else if (c == "5") {
            g_timer.Release();
            printf("  Відпущено.\n");
        } else if (c == "6") {
            EnableGlobalTimerResolution();
        } else if (c == "0" || c.empty()) {
            return;
        }
    }
}

static void NetworkTestMenu() {
    printf("\n=== Тест мережі ===\n");
    printf("  Введи один або кілька серверів через пробіл.\n");
    printf("  Формат: адреса (ICMP ping) або адреса:порт (TCP ping, якщо сервер не відповідає на ping).\n");
    printf("  Приклад: 1.1.1.1 8.8.8.8 google.com:443\n");
    auto targets = Split(ReadLine("  Сервери: "));
    if (targets.empty()) return;
    int count = atoi(ReadLine("  Кількість запитів [20]: ").c_str());
    if (count <= 0) count = 20;
    if (targets.size() == 1) {
        PingResult r = PingAny(targets[0], count, true);
        printf("\n");
        PrintPing(r);
    } else {
        CompareServers(targets, count);
    }
}

static void PrintHonestNote() {
    printf("\n=== Як це працює насправді ===\n"
           "  ExitLag та подібні сервіси зменшують пінг, пускаючи трафік гри через СВОЇ сервери\n"
           "  (кращий маршрут + дублювання пакетів кількома шляхами). Без мережі таких серверів\n"
           "  жодна локальна програма не змінить маршрут в інтернеті — це чесно.\n\n"
           "  Що LukashBoost реально робить:\n"
           "   * Timer Resolution — точніші таймери: стабільніший frametime, менша затримка вводу.\n"
           "   * Вимкнення Нейгла — TCP-пакети йдуть одразу, без накопичення (для TCP-ігор).\n"
           "   * NetworkThrottlingIndex — Windows не обмежує обробку мережевих пакетів.\n"
           "   * QoS DSCP 46 — пакети гри йдуть у пріоритетну чергу (Wi‑Fi WMM і роутери з QoS\n"
           "     це враховують; особливо помітно, коли хтось качає/дивиться відео в тій самій мережі).\n"
           "   * High пріоритет і вимкнення EcoQoS для гри.\n"
           "   * Тест пінгу/джитеру/втрат — щоб вибрати найкращий сервер/регіон гри.\n");
}

static void MainMenu() {
    for (;;) {
        printf("\n==============================\n");
        printf("  %s — Timer Resolution + Net\n", kAppName);
        printf("==============================\n");
        if (!IsElevated()) printf("  [!] Запущено БЕЗ прав адміністратора — твіки реєстру не спрацюють.\n");
        printf("  1) Timer Resolution\n");
        printf("  2) Тест мережі (пінг / джитер / втрати / порівняння серверів)\n");
        printf("  3) Застосувати мережеві твіки (з резервною копією)\n");
        printf("  4) QoS-пріоритет для гри (DSCP 46)\n");
        printf("  5) Режим гри (автоматично для exe)\n");
        printf("  6) Стан твіків\n");
        printf("  7) Відкотити ВСІ зміни\n");
        printf("  8) Очистити DNS-кеш\n");
        printf("  9) Як це працює\n");
        printf("  0) Вихід\n");
        std::string c = ReadLine("> ");
        if (c == "1") {
            TimerMenu();
        } else if (c == "2") {
            NetworkTestMenu();
            Pause();
        } else if (c == "3") {
            printf("\n");
            ApplyNetworkTweaks();
            Pause();
        } else if (c == "4") {
            std::string exe = ReadLine("  Ім'я exe гри (напр. cs2.exe): ");
            if (!exe.empty()) ApplyQosForGame(exe);
            Pause();
        } else if (c == "5") {
            std::string exe = ReadLine("  Ім'я exe гри (напр. cs2.exe): ");
            if (exe.empty()) continue;
            ULONG res = ParseResolutionMs(ReadLine("  Timer Resolution в мс [0.5]: "), 5000);
            GameMode(exe, res);
            Pause();
        } else if (c == "6") {
            printf("\n");
            PrintTimerStatus();
            PrintTweakStatus();
            Pause();
        } else if (c == "7") {
            if (AskYes("  Точно відкотити всі зміни? (y/n): ")) RevertAll();
            Pause();
        } else if (c == "8") {
            system("ipconfig /flushdns");
            Pause();
        } else if (c == "9") {
            PrintHonestNote();
            Pause();
        } else if (c == "0") {
            return;
        }
    }
}

static void Usage() {
    printf("Використання:\n"
           "  LukashBoost.exe                         інтерактивне меню\n"
           "  LukashBoost.exe --res 0.5               тримати Timer Resolution (Ctrl+C — вихід)\n"
           "  LukashBoost.exe --game cs2.exe [--res 0.5] [--qos]   режим гри\n"
           "  LukashBoost.exe --ping host[:port] [...] [--count 20] тест / порівняння серверів\n"
           "  LukashBoost.exe --apply                 мережеві твіки\n"
           "  LukashBoost.exe --revert                відкотити всі зміни\n"
           "  LukashBoost.exe --status                стан\n");
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    if (!g_timer.Load()) {
        printf("Не вдалося завантажити функції таймера з ntdll.dll\n");
        return 1;
    }
    // Windows 11 ігнорує запит роздільності від згорнутих/невидимих процесів — вимикаємо це для себе.
    DisablePowerThrottling(GetCurrentProcess(), PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION);

    if (argc <= 1) {
        MainMenu();
        g_timer.Release();
        WSACleanup();
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
    if (status) {
        PrintTimerStatus();
        PrintTweakStatus();
    }
    if (pings.size() == 1) {
        PingResult r = PingAny(pings[0], count, true);
        PrintPing(r);
    } else if (pings.size() > 1) {
        CompareServers(pings, count);
    }
    if (!game.empty()) {
        if (qos) ApplyQosForGame(game);
        GameMode(game, res ? res : 5000);
    } else if (res) {
        ULONG actual = 0;
        if (!g_timer.Hold(res, &actual)) {
            printf("Не вдалося встановити Timer Resolution\n");
            return 1;
        }
        printf("Timer Resolution: %.4f мс. Ctrl+C — вихід.\n", actual / 10000.0);
        SetConsoleCtrlHandler(CtrlHandler, TRUE);
        while (!g_stop) Sleep(500);
        g_timer.Release();
    }
    WSACleanup();
    return 0;
}
