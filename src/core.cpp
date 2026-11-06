#include "core.h"

#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace lb {

static const char* kBackupKey = "SOFTWARE\\LukashBoost\\Backup";
static const char* kQosRoot = "SOFTWARE\\Policies\\Microsoft\\Windows\\QoS";
static const char* kQosPrefix = "LukashBoost_";
static const char* kTcpIfaces = "SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces";
static const char* kTcpQos = "SYSTEM\\CurrentControlSet\\Services\\Tcpip\\QoS";
static const char* kSysProfile = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile";
static const char* kGamesTask =
    "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile\\Tasks\\Games";
static const char* kKernel = "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\kernel";

// ---------------------------------------------------------------------------
// Лог
// ---------------------------------------------------------------------------

static std::mutex g_logMutex;
static std::function<void(const std::string&)> g_sink;

void SetLogSink(std::function<void(const std::string&)> sink) {
    std::lock_guard<std::mutex> l(g_logMutex);
    g_sink = std::move(sink);
}

void Log(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> l(g_logMutex);
    if (g_sink)
        g_sink(buf);
    else
        printf("%s\n", buf);
}

// ---------------------------------------------------------------------------
// Загальне
// ---------------------------------------------------------------------------

double NowMs() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
}

bool IsElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION el{};
    DWORD sz = 0;
    BOOL ok = GetTokenInformation(token, TokenElevation, &el, sizeof(el), &sz);
    CloseHandle(token);
    return ok && el.TokenIsElevated;
}

DWORD WindowsBuild() {
    typedef LONG(WINAPI * RtlGetVersion_t)(PRTL_OSVERSIONINFOW);
    auto fn = (RtlGetVersion_t)(void*)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion");
    RTL_OSVERSIONINFOW vi{};
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (fn && fn(&vi) == 0) return vi.dwBuildNumber;
    return 0;
}

std::string ToLower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

std::vector<std::string> Split(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ' ' || c == ',' || c == ';' || c == '\t' || c == '\r' || c == '\n') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// Запуск консольної команди без миготіння вікна
static void RunHidden(const char* cmdline) {
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::string cmd = cmdline;
    if (CreateProcessA(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 60000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

// ---------------------------------------------------------------------------
// Power throttling
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
    st.ControlMask = mask;  // беремо ці прапорці під контроль...
    st.StateMask = 0;       // ...і вимикаємо їх
    const int ProcessPowerThrottling = 4;
    return fn(process, ProcessPowerThrottling, &st, sizeof(st)) != FALSE;
}

// ---------------------------------------------------------------------------
// Timer Resolution
// ---------------------------------------------------------------------------

typedef LONG(NTAPI* NtQueryTimerResolution_t)(PULONG, PULONG, PULONG);
typedef LONG(NTAPI* NtSetTimerResolution_t)(ULONG, BOOLEAN, PULONG);
static NtQueryTimerResolution_t g_query = nullptr;
static NtSetTimerResolution_t g_set = nullptr;
static std::mutex g_timerMutex;
static ULONG g_held = 0;

bool Init() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    g_query = (NtQueryTimerResolution_t)(void*)GetProcAddress(nt, "NtQueryTimerResolution");
    g_set = (NtSetTimerResolution_t)(void*)GetProcAddress(nt, "NtSetTimerResolution");
    // Windows 11 ігнорує запит роздільності від згорнутих/невидимих процесів — вимикаємо це для себе.
    DisablePowerThrottling(GetCurrentProcess(), PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION);
    return g_query && g_set;
}

void Shutdown() {
    TimerRelease();
    WSACleanup();
}

bool TimerQuery(ULONG& coarsest, ULONG& finest, ULONG& current) {
    return g_query && g_query(&coarsest, &finest, &current) == 0;
}

static void ReleaseLocked() {
    if (g_held) {
        ULONG cur;
        g_set(g_held, FALSE, &cur);
        g_held = 0;
    }
}

bool TimerHold(ULONG value, ULONG* actual) {
    std::lock_guard<std::mutex> l(g_timerMutex);
    if (!g_set) return false;
    ReleaseLocked();
    ULONG cur = 0;
    if (g_set(value, TRUE, &cur) != 0) return false;
    g_held = value;
    if (actual) *actual = cur;
    return true;
}

void TimerRelease() {
    std::lock_guard<std::mutex> l(g_timerMutex);
    if (g_set) ReleaseLocked();
}

ULONG TimerHeld() {
    std::lock_guard<std::mutex> l(g_timerMutex);
    return g_held;
}

SleepStats MeasureSleep1(int samples, std::vector<float>* out) {
    SleepStats s;
    s.min = 1e9;
    double sum = 0;
    if (out) out->clear();
    for (int i = 0; i < samples; ++i) {
        double t0 = NowMs();
        Sleep(1);
        double d = NowMs() - t0;
        sum += d;
        s.max = std::max(s.max, d);
        s.min = std::min(s.min, d);
        if (out) out->push_back((float)d);
    }
    s.avg = samples ? sum / samples : 0;
    return s;
}

ULONG AutoTuneResolution(const std::atomic<bool>* cancel) {
    ULONG coarsest, finest, current;
    if (!TimerQuery(coarsest, finest, current)) return 5000;
    ULONG start = std::max<ULONG>(finest, 5000);
    ULONG best = start;
    double bestScore = 1e9;
    Log("Авто-підбір Timer Resolution...");
    for (ULONG v = start; v <= 5200; v += 20) {
        if (cancel && *cancel) break;
        if (!TimerHold(v)) continue;
        Sleep(50);
        SleepStats s = MeasureSleep1(150);
        double score = std::fabs(s.avg - 1.0) + 0.1 * (s.max - s.avg);
        Log("  %.4f мс -> Sleep(1) = %.3f мс (макс %.3f)", v / 10000.0, s.avg, s.max);
        if (score < bestScore) {
            bestScore = score;
            best = v;
        }
    }
    ULONG actual = 0;
    TimerHold(best, &actual);
    Log("Найкраще значення: %.4f мс (утримується)", best / 10000.0);
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
        RegCloseKey(bk);  // оригінал уже збережено — не перезаписуємо
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

static bool GetString(HKEY root, const std::string& path, const std::string& name, std::string& out) {
    char buf[512];
    DWORD sz = sizeof(buf);
    if (RegGetValueA(root, path.c_str(), name.c_str(), RRF_RT_REG_SZ, nullptr, buf, &sz) != ERROR_SUCCESS)
        return false;
    out = buf;
    return true;
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

    int restored = 0;
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
// Твіки
// ---------------------------------------------------------------------------

static bool InterfaceHasIp(const std::string& guid) {
    std::string path = std::string(kTcpIfaces) + "\\" + guid;
    char buf[512];
    DWORD sz = sizeof(buf);
    if (RegGetValueA(HKEY_LOCAL_MACHINE, path.c_str(), "DhcpIPAddress", RRF_RT_REG_SZ, nullptr, buf, &sz) ==
            ERROR_SUCCESS &&
        buf[0] && strcmp(buf, "0.0.0.0") != 0)
        return true;
    sz = sizeof(buf);
    if (RegGetValueA(HKEY_LOCAL_MACHINE, path.c_str(), "IPAddress", RRF_RT_REG_MULTI_SZ, nullptr, buf, &sz) ==
            ERROR_SUCCESS &&
        buf[0] && strcmp(buf, "0.0.0.0") != 0)
        return true;
    return false;
}

void ApplyNetworkTweaks() {
    int n = 0;
    for (auto& guid : SubKeys(HKEY_LOCAL_MACHINE, kTcpIfaces)) {
        if (!InterfaceHasIp(guid)) continue;
        std::string path = std::string(kTcpIfaces) + "\\" + guid;
        bool ok = SetDword(HKEY_LOCAL_MACHINE, path, "TcpAckFrequency", 1) &&
                  SetDword(HKEY_LOCAL_MACHINE, path, "TCPNoDelay", 1) &&
                  SetDword(HKEY_LOCAL_MACHINE, path, "TcpDelAckTicks", 0);
        Log("[%s] Нейгл вимкнено: %s", ok ? "OK" : "ПОМИЛКА", guid.c_str());
        ++n;
    }
    if (!n) Log("[!] Не знайдено активних мережевих інтерфейсів з IP");

    bool ok = SetDword(HKEY_LOCAL_MACHINE, kSysProfile, "NetworkThrottlingIndex", 0xFFFFFFFF);
    Log("[%s] NetworkThrottlingIndex вимкнено", ok ? "OK" : "ПОМИЛКА");
    ok = SetDword(HKEY_LOCAL_MACHINE, kSysProfile, "SystemResponsiveness", 10);
    Log("[%s] SystemResponsiveness = 10", ok ? "OK" : "ПОМИЛКА");
    ok = SetDword(HKEY_LOCAL_MACHINE, kGamesTask, "GPU Priority", 8) &&
         SetDword(HKEY_LOCAL_MACHINE, kGamesTask, "Priority", 6) &&
         SetString(HKEY_LOCAL_MACHINE, kGamesTask, "Scheduling Category", "High") &&
         SetString(HKEY_LOCAL_MACHINE, kGamesTask, "SFIO Priority", "High");
    Log("[%s] MMCSS пріоритети для ігор (GPU 8, CPU 6, High)", ok ? "OK" : "ПОМИЛКА");
    Log("Частина змін набуде чинності після перезавантаження ПК.");
}

bool GlobalTimerSupported() { return WindowsBuild() >= 20348; }

bool GlobalTimerEnabled() {
    DWORD v;
    return GetDword(HKEY_LOCAL_MACHINE, kKernel, "GlobalTimerResolutionRequests", v) && v == 1;
}

void EnableGlobalTimerResolution() {
    if (!GlobalTimerSupported()) {
        Log("Збірка Windows %lu не підтримує GlobalTimerResolutionRequests", WindowsBuild());
        return;
    }
    bool ok = SetDword(HKEY_LOCAL_MACHINE, kKernel, "GlobalTimerResolutionRequests", 1);
    Log("[%s] GlobalTimerResolutionRequests = 1 (потрібне перезавантаження)", ok ? "OK" : "ПОМИЛКА");
}

void ApplyQosForGame(const std::string& exe) {
    if (exe.empty()) return;
    std::string path = std::string(kQosRoot) + "\\" + kQosPrefix + exe;
    // Ключ політики створюємо самі — відкат видаляє його цілком, бекап не потрібен.
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
    // Без цього Windows ставить DSCP лише в доменних мережах.
    ok = ok && SetString(HKEY_LOCAL_MACHINE, kTcpQos, "Do not use NLA", "1");
    Log("[%s] QoS DSCP 46 для %s", ok ? "OK" : "ПОМИЛКА", exe.c_str());
    if (ok) {
        Log("Оновлюю групові політики (gpupdate)...");
        RunHidden("gpupdate.exe /target:computer /force");
        Log("Готово.");
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

void RevertAll() {
    int r = RestoreBackup();
    int q = RemoveQosPolicies();
    Log("Відновлено значень реєстру: %d, видалено QoS-політик: %d", r, q);
    if (q) RunHidden("gpupdate.exe /target:computer /force");
    Log("Перезавантаж ПК, щоб усе повернулося повністю.");
}

TweakStatus GetTweakStatus() {
    TweakStatus s;
    DWORD v;
    for (auto& guid : SubKeys(HKEY_LOCAL_MACHINE, kTcpIfaces)) {
        if (!InterfaceHasIp(guid)) continue;
        ++s.ifaces;
        std::string path = std::string(kTcpIfaces) + "\\" + guid;
        if (GetDword(HKEY_LOCAL_MACHINE, path, "TcpAckFrequency", v) && v == 1 &&
            GetDword(HKEY_LOCAL_MACHINE, path, "TCPNoDelay", v) && v == 1)
            ++s.nagleOff;
    }
    s.throttlingOff = GetDword(HKEY_LOCAL_MACHINE, kSysProfile, "NetworkThrottlingIndex", v) && v == 0xFFFFFFFF;
    if (GetDword(HKEY_LOCAL_MACHINE, kSysProfile, "SystemResponsiveness", v)) s.sysResponsiveness = v;
    std::string cat;
    s.mmcss = GetDword(HKEY_LOCAL_MACHINE, kGamesTask, "GPU Priority", v) && v == 8 &&
              GetString(HKEY_LOCAL_MACHINE, kGamesTask, "Scheduling Category", cat) && cat == "High";
    s.globalTimer = GlobalTimerEnabled();
    std::string nla;
    s.nla = GetString(HKEY_LOCAL_MACHINE, kTcpQos, "Do not use NLA", nla) && nla == "1";
    for (auto& name : SubKeys(HKEY_LOCAL_MACHINE, kQosRoot))
        if (name.rfind(kQosPrefix, 0) == 0) s.qosGames.push_back(name.substr(strlen(kQosPrefix)));
    return s;
}

void FlushDns() {
    typedef BOOL(WINAPI * Flush_t)();
    HMODULE dns = LoadLibraryA("dnsapi.dll");
    auto fn = dns ? (Flush_t)(void*)GetProcAddress(dns, "DnsFlushResolverCache") : nullptr;
    bool ok = fn && fn();
    Log("[%s] DNS-кеш очищено", ok ? "OK" : "ПОМИЛКА");
    if (dns) FreeLibrary(dns);
}

// ---------------------------------------------------------------------------
// Тест мережі
// ---------------------------------------------------------------------------

double PingResult::Score() const {
    if (!received) return 1e12;
    return avg + 2 * jitter + Loss() * 10;
}

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

// Один ICMP-запит, повертає RTT або -1
static double IcmpOnce(HANDLE h, const sockaddr_in& addr) {
    char payload[32] = "LukashBoost-ping";
    BYTE reply[sizeof(ICMP_ECHO_REPLY) + sizeof(payload) + 64];
    double t0 = NowMs();
    DWORD n = IcmpSendEcho(h, addr.sin_addr.s_addr, payload, sizeof(payload), nullptr, reply, sizeof(reply), 2000);
    double dt = NowMs() - t0;
    auto* rep = (ICMP_ECHO_REPLY*)reply;
    return (n > 0 && rep->Status == IP_SUCCESS) ? dt : -1;
}

// Один TCP-connect, повертає час встановлення з'єднання або -1
static double TcpOnce(const sockaddr_in& addr) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return -1;
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    double t0 = NowMs();
    connect(s, (const sockaddr*)&addr, sizeof(addr));
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
    return ok ? dt : -1;
}

PingResult Ping(const std::string& target, int count, const SampleFn& sample, const std::atomic<bool>* cancel) {
    PingResult r;
    r.target = target;
    std::string host = target;
    int port = 0;
    size_t c = target.rfind(':');
    if (c != std::string::npos && c + 1 < target.size()) {
        int p = atoi(target.c_str() + c + 1);
        if (p > 0 && p < 65536) {
            port = p;
            host = target.substr(0, c);
        }
    }
    sockaddr_in addr{};
    if (!Resolve(host, addr)) return r;
    r.resolved = true;
    char ipbuf[64];
    inet_ntop(AF_INET, &addr.sin_addr, ipbuf, sizeof(ipbuf));
    r.ip = ipbuf;
    addr.sin_port = htons((u_short)port);

    HANDLE icmp = port ? INVALID_HANDLE_VALUE : IcmpCreateFile();
    if (!port && icmp == INVALID_HANDLE_VALUE) return r;
    std::vector<double> rtts;
    for (int i = 0; i < count; ++i) {
        if (cancel && *cancel) break;
        ++r.sent;
        double rtt = port ? TcpOnce(addr) : IcmpOnce(icmp, addr);
        if (rtt >= 0) rtts.push_back(rtt);
        if (sample) sample(i, rtt);
        if (i + 1 < count) Sleep(200);
    }
    if (icmp != INVALID_HANDLE_VALUE) IcmpCloseHandle(icmp);
    Finish(r, rtts);
    return r;
}

void LogPing(const PingResult& r) {
    if (!r.resolved) {
        Log("%s: не вдалося знайти адресу", r.target.c_str());
    } else if (!r.received) {
        Log("%s (%s): немає відповідей (100%% втрат)", r.target.c_str(), r.ip.c_str());
    } else {
        Log("%s (%s): мін %.1f / сер %.1f / макс %.1f мс, джитер %.1f мс, втрати %.0f%%", r.target.c_str(),
            r.ip.c_str(), r.min, r.avg, r.max, r.jitter, r.Loss());
    }
}

// ---------------------------------------------------------------------------
// Режим гри
// ---------------------------------------------------------------------------

DWORD FindProcess(const std::string& exe) {
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

void RunGameMode(const std::string& exe, ULONG resolution, const std::atomic<bool>& stop,
                 std::atomic<DWORD>* gamePid) {
    Log("Режим гри: %s", exe.c_str());
    ULONG actual = 0;
    if (TimerHold(resolution, &actual))
        Log("[OK] Timer Resolution утримується: %.4f мс", actual / 10000.0);
    else
        Log("[!] Не вдалося встановити Timer Resolution");

    bool waitingShown = false;
    while (!stop) {
        DWORD pid = FindProcess(exe);
        if (!pid) {
            if (gamePid) *gamePid = 0;
            if (!waitingShown) {
                Log("Чекаю на запуск %s...", exe.c_str());
                waitingShown = true;
            }
            for (int i = 0; i < 10 && !stop; ++i) Sleep(100);
            continue;
        }
        waitingShown = false;
        if (gamePid) *gamePid = pid;
        Log("Гру знайдено (PID %lu)", pid);
        HANDLE h = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
        if (!h) {
            Log("[!] Немає доступу до процесу (потрібні права адміністратора), код %lu", GetLastError());
            for (int i = 0; i < 30 && !stop; ++i) Sleep(100);
            continue;
        }
        Log("[%s] Пріоритет процесу: High", SetPriorityClass(h, HIGH_PRIORITY_CLASS) ? "OK" : "ПОМИЛКА");
        bool pt = DisablePowerThrottling(h, PROCESS_POWER_THROTTLING_EXECUTION_SPEED |
                                                PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION);
        if (pt)
            Log("[OK] EcoQoS / power throttling для гри вимкнено");
        else
            Log("EcoQoS: ця версія Windows не підтримує керування (пропущено)");
        while (!stop && WaitForSingleObject(h, 300) == WAIT_TIMEOUT) {
        }
        CloseHandle(h);
        if (gamePid) *gamePid = 0;
        if (!stop) Log("Гру закрито. Чекаю наступного запуску...");
    }
    TimerRelease();
    if (gamePid) *gamePid = 0;
    Log("Режим гри зупинено, Timer Resolution відпущено.");
}

}  // namespace lb
