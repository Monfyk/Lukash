#include "core.h"

#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace lb {

const char* kAppName = "KairoBoost";
const char* kAppRegRoot = "SOFTWARE\\KairoBoost";
static const char* kBackupKey = "SOFTWARE\\KairoBoost\\Backup";
static const char* kStateKey = "SOFTWARE\\KairoBoost\\State";
static const char* kLegacyBackupKey = "SOFTWARE\\LukashBoost\\Backup";  // попередня назва програми
static const char* kQosRoot = "SOFTWARE\\Policies\\Microsoft\\Windows\\QoS";
static const char* kQosPrefix = "KairoBoost_";
static const char* kLegacyQosPrefix = "LukashBoost_";
static const char* kTcpQos = "SYSTEM\\CurrentControlSet\\Services\\Tcpip\\QoS";

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

std::string GetEnv(const char* name) {
    char buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
    return (n && n < sizeof(buf)) ? std::string(buf, n) : std::string();
}

bool FileExists(const std::string& path) {
    DWORD a = GetFileAttributesA(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

void RunHidden(const char* cmdline) {
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

std::string RunCapture(const char* cmdline) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return "";
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    std::string cmd = cmdline, out;
    bool ok = CreateProcessA(nullptr, &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (ok) {
        char buf[1024];
        DWORD n;
        while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n) out.append(buf, n);
        WaitForSingleObject(pi.hProcess, 30000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    CloseHandle(rd);
    return out;
}

static bool EnablePrivilege(const char* name) {
    HANDLE token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return false;
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    bool ok = LookupPrivilegeValueA(nullptr, name, &tp.Privileges[0].Luid) &&
              AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr) &&
              GetLastError() == ERROR_SUCCESS;
    CloseHandle(token);
    return ok;
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
    st.ControlMask = mask;
    st.StateMask = 0;
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
    // Windows 11 ігнорує запит роздільності від згорнутих процесів — вимикаємо це для себе.
    DisablePowerThrottling(GetCurrentProcess(), PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION);
    return g_query && g_set;
}

void SetSelfLowPower(bool on) {
    HANDLE self = GetCurrentProcess();
    SetPriorityClass(self, on ? BELOW_NORMAL_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS);
    typedef BOOL(WINAPI * SetProcessInformation_t)(HANDLE, int, LPVOID, DWORD);
    auto fn = (SetProcessInformation_t)(void*)GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetProcessInformation");
    if (fn) {
        PROCESS_POWER_THROTTLING_STATE st{};
        st.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        // EcoQoS вмикаємо/вимикаємо, а "ігнорувати Timer Resolution" — завжди вимкнено
        st.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
        st.StateMask = on ? PROCESS_POWER_THROTTLING_EXECUTION_SPEED : 0;
        fn(self, 4 /*ProcessPowerThrottling*/, &st, sizeof(st));
    }
    if (on) EmptyWorkingSet(self);  // віддати системі пам'ять, яку займає інтерфейс
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
    TimerHold(best);
    Log("Найкраще значення: %.4f мс (утримується)", best / 10000.0);
    return best;
}

bool GlobalTimerSupported() { return WindowsBuild() >= 20348; }

// ---------------------------------------------------------------------------
// Реєстр з резервною копією
// ---------------------------------------------------------------------------

RegValue Dw(HKEY root, const std::string& path, const std::string& name, DWORD v) {
    RegValue r{root, path, name, REG_DWORD};
    r.dw = v;
    return r;
}

RegValue Sz(HKEY root, const std::string& path, const std::string& name, const std::string& v) {
    RegValue r{root, path, name, REG_SZ};
    r.sz = v;
    return r;
}

static std::string BackupId(HKEY root, const std::string& path, const std::string& name) {
    return std::string(root == HKEY_LOCAL_MACHINE ? "HKLM" : "HKCU") + "|" + path + "|" + name;
}

static void BackupValue(HKEY root, const std::string& path, const std::string& name) {
    HKEY bk;
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, kBackupKey, 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &bk,
                        nullptr) != ERROR_SUCCESS)
        return;
    std::string id = BackupId(root, path, name);
    if (RegQueryValueExA(bk, id.c_str(), nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
        RegCloseKey(bk);  // оригінал уже збережено
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

// Записує збережене значення назад (blob = 4 байти типу + дані)
static void WriteBackBlob(HKEY root, const std::string& path, const std::string& name, const std::vector<BYTE>& blob) {
    if (blob.size() < 4) return;
    DWORD type;
    memcpy(&type, blob.data(), 4);
    HKEY k;
    if (RegOpenKeyExA(root, path.c_str(), 0, KEY_WRITE, &k) != ERROR_SUCCESS) return;
    if (type == 0xFFFFFFFF)
        RegDeleteValueA(k, name.c_str());
    else
        RegSetValueExA(k, name.c_str(), 0, type, blob.data() + 4, (DWORD)blob.size() - 4);
    RegCloseKey(k);
}

bool RegApply(const RegValue& v, bool backup) {
    if (backup) BackupValue(v.root, v.path, v.name);
    HKEY k;
    if (RegCreateKeyExA(v.root, v.path.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return false;
    LONG r;
    if (v.type == REG_DWORD)
        r = RegSetValueExA(k, v.name.c_str(), 0, REG_DWORD, (const BYTE*)&v.dw, sizeof(v.dw));
    else
        r = RegSetValueExA(k, v.name.c_str(), 0, REG_SZ, (const BYTE*)v.sz.c_str(), (DWORD)v.sz.size() + 1);
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

bool RegGetDword(HKEY root, const std::string& path, const std::string& name, DWORD& out) {
    DWORD sz = sizeof(out);
    return RegGetValueA(root, path.c_str(), name.c_str(), RRF_RT_REG_DWORD, nullptr, &out, &sz) == ERROR_SUCCESS;
}

bool RegGetString(HKEY root, const std::string& path, const std::string& name, std::string& out) {
    char buf[1024];
    DWORD sz = sizeof(buf);
    if (RegGetValueA(root, path.c_str(), name.c_str(), RRF_RT_REG_SZ, nullptr, buf, &sz) != ERROR_SUCCESS)
        return false;
    out = buf;
    return true;
}

bool RegValueExists(HKEY root, const std::string& path, const std::string& name) {
    return RegGetValueA(root, path.c_str(), name.c_str(), RRF_RT_ANY, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

bool RegIsSet(const RegValue& v) {
    if (v.type == REG_DWORD) {
        DWORD cur;
        return RegGetDword(v.root, v.path, v.name, cur) && cur == v.dw;
    }
    std::string cur;
    return RegGetString(v.root, v.path, v.name, cur) && cur == v.sz;
}

bool RegRestore(const RegValue& v) {
    HKEY bk;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, kBackupKey, 0, KEY_READ | KEY_WRITE, &bk) != ERROR_SUCCESS) return false;
    bool found = false;
    std::string id = BackupId(v.root, v.path, v.name);
    DWORD sz = 0;
    if (RegQueryValueExA(bk, id.c_str(), nullptr, nullptr, nullptr, &sz) == ERROR_SUCCESS) {
        std::vector<BYTE> blob(sz);
        if (RegQueryValueExA(bk, id.c_str(), nullptr, nullptr, blob.data(), &sz) == ERROR_SUCCESS) {
            blob.resize(sz);
            WriteBackBlob(v.root, v.path, v.name, blob);
            found = true;
        }
        RegDeleteValueA(bk, id.c_str());
    }
    RegCloseKey(bk);
    return found;
}

std::vector<std::string> RegSubKeys(HKEY root, const std::string& path) {
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

static int RestoreBackupKey(const char* key) {
    HKEY bk;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &bk) != ERROR_SUCCESS) return 0;
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
        if (p1 == std::string::npos || p2 == p1) continue;
        HKEY root = id.substr(0, p1) == "HKLM" ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
        WriteBackBlob(root, id.substr(p1 + 1, p2 - p1 - 1), id.substr(p2 + 1), it.second);
        ++restored;
    }
    RegDeleteKeyA(HKEY_LOCAL_MACHINE, key);
    return restored;
}

int RestoreAllRegistryBackups() { return RestoreBackupKey(kBackupKey) + RestoreBackupKey(kLegacyBackupKey); }

bool StateSave(const std::string& id, const std::string& value, bool overwrite) {
    HKEY k;
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, kStateKey, 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &k, nullptr) !=
        ERROR_SUCCESS)
        return false;
    bool exists = RegQueryValueExA(k, id.c_str(), nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    bool ok = true;
    if (!exists || overwrite)
        ok = RegSetValueExA(k, id.c_str(), 0, REG_SZ, (const BYTE*)value.c_str(), (DWORD)value.size() + 1) ==
             ERROR_SUCCESS;
    RegCloseKey(k);
    return ok;
}

bool StateLoad(const std::string& id, std::string& value) { return RegGetString(HKEY_LOCAL_MACHINE, kStateKey, id, value); }

void StateDelete(const std::string& id) {
    HKEY k;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, kStateKey, 0, KEY_WRITE, &k) == ERROR_SUCCESS) {
        RegDeleteValueA(k, id.c_str());
        RegCloseKey(k);
    }
}

// ---------------------------------------------------------------------------
// Служби
// ---------------------------------------------------------------------------

bool ServiceGetStart(const char* name, DWORD& start) {
    SC_HANDLE scm = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE s = OpenServiceA(scm, name, SERVICE_QUERY_CONFIG);
    bool ok = false;
    if (s) {
        DWORD need = 0;
        QueryServiceConfigA(s, nullptr, 0, &need);
        std::vector<BYTE> buf(need + 16);
        auto* cfg = (QUERY_SERVICE_CONFIGA*)buf.data();
        if (QueryServiceConfigA(s, cfg, (DWORD)buf.size(), &need)) {
            start = cfg->dwStartType;
            ok = true;
        }
        CloseServiceHandle(s);
    }
    CloseServiceHandle(scm);
    return ok;
}

bool ServiceSetStart(const char* name, DWORD start) {
    SC_HANDLE scm = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE s = OpenServiceA(scm, name, SERVICE_CHANGE_CONFIG);
    bool ok = s && ChangeServiceConfigA(s, SERVICE_NO_CHANGE, start, SERVICE_NO_CHANGE, nullptr, nullptr, nullptr,
                                        nullptr, nullptr, nullptr, nullptr);
    if (s) CloseServiceHandle(s);
    CloseServiceHandle(scm);
    return ok;
}

void ServiceStop(const char* name) {
    SC_HANDLE scm = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return;
    SC_HANDLE s = OpenServiceA(scm, name, SERVICE_STOP);
    if (s) {
        SERVICE_STATUS st;
        ControlService(s, SERVICE_CONTROL_STOP, &st);
        CloseServiceHandle(s);
    }
    CloseServiceHandle(scm);
}

void ServiceStart(const char* name) {
    SC_HANDLE scm = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return;
    SC_HANDLE s = OpenServiceA(scm, name, SERVICE_START);
    if (s) {
        StartServiceA(s, 0, nullptr);
        CloseServiceHandle(s);
    }
    CloseServiceHandle(scm);
}

// ---------------------------------------------------------------------------
// QoS
// ---------------------------------------------------------------------------

bool QosIsSet(const std::string& exe) {
    std::string path = std::string(kQosRoot) + "\\" + kQosPrefix + exe;
    std::string v;
    return RegGetString(HKEY_LOCAL_MACHINE, path, "DSCP Value", v) && v == "46";
}

bool QosApply(const std::string& exe) {
    if (exe.empty()) return false;
    std::string path = std::string(kQosRoot) + "\\" + kQosPrefix + exe;
    // Ключ політики створюємо самі — видаляємо його цілком, окремий бекап не потрібен.
    const char* keys[][2] = {{"Version", "1.0"},
                             {"Application Name", exe.c_str()},
                             {"Protocol", "*"},
                             {"Local Port", "*"},
                             {"Local IP", "*"},
                             {"Local IP Prefix Length", "*"},
                             {"Remote Port", "*"},
                             {"Remote IP", "*"},
                             {"Remote IP Prefix Length", "*"},
                             {"DSCP Value", "46"},
                             {"Throttle Rate", "-1"}};
    bool ok = true;
    for (auto& kv : keys) ok = ok && RegApply(Sz(HKEY_LOCAL_MACHINE, path, kv[0], kv[1]), false);
    // Без цього Windows ставить DSCP лише в доменних мережах.
    ok = ok && RegApply(Sz(HKEY_LOCAL_MACHINE, kTcpQos, "Do not use NLA", "1"));
    if (ok) RunHidden("gpupdate.exe /target:computer /force");
    return ok;
}

bool QosRemove(const std::string& exe) {
    std::string path = std::string(kQosRoot) + "\\" + kQosPrefix + exe;
    bool ok = RegDeleteKeyA(HKEY_LOCAL_MACHINE, path.c_str()) == ERROR_SUCCESS;
    // Якщо інших наших QoS-політик не лишилось — повертаємо і "Do not use NLA"
    bool any = false;
    for (auto& name : RegSubKeys(HKEY_LOCAL_MACHINE, kQosRoot))
        if (name.rfind(kQosPrefix, 0) == 0) any = true;
    if (!any) RegRestore(Sz(HKEY_LOCAL_MACHINE, kTcpQos, "Do not use NLA", ""));
    RunHidden("gpupdate.exe /target:computer /force");
    return ok;
}

int QosRemoveAll() {
    int n = 0;
    for (auto& name : RegSubKeys(HKEY_LOCAL_MACHINE, kQosRoot)) {
        if (name.rfind(kQosPrefix, 0) != 0 && name.rfind(kLegacyQosPrefix, 0) != 0) continue;
        std::string path = std::string(kQosRoot) + "\\" + name;
        if (RegDeleteKeyA(HKEY_LOCAL_MACHINE, path.c_str()) == ERROR_SUCCESS) ++n;
    }
    if (n) RunHidden("gpupdate.exe /target:computer /force");
    return n;
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
// Пам'ять
// ---------------------------------------------------------------------------

MemInfo GetMemInfo() {
    MemInfo m;
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        m.total = ms.ullTotalPhys;
        m.avail = ms.ullAvailPhys;
        m.load = ms.dwMemoryLoad;
    }
    return m;
}

CleanResult CleanMemory(const std::vector<std::string>& exclude, bool purgeStandby) {
    CleanResult r;
    MemInfo before = GetMemInfo();
    EnablePrivilege("SeProfileSingleProcessPrivilege");
    EnablePrivilege("SeIncreaseQuotaPrivilege");

    std::vector<std::string> ex;
    for (auto& e : exclude) ex.push_back(ToLower(e));
    DWORD self = GetCurrentProcessId();

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32 pe{};
        pe.dwSize = sizeof(pe);
        if (Process32First(snap, &pe)) {
            do {
                if (pe.th32ProcessID == self || pe.th32ProcessID <= 4) continue;
                if (std::find(ex.begin(), ex.end(), ToLower(pe.szExeFile)) != ex.end()) continue;
                HANDLE h = OpenProcess(PROCESS_SET_QUOTA | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
                if (!h) continue;
                if (EmptyWorkingSet(h)) ++r.trimmed;
                CloseHandle(h);
            } while (Process32Next(snap, &pe));
        }
        CloseHandle(snap);
    }

    // Очищення standby-списку (кеш файлів, який іноді "не віддається" іграм)
    typedef LONG(NTAPI * NtSetSystemInformation_t)(int, PVOID, ULONG);
    auto fn = (NtSetSystemInformation_t)(void*)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtSetSystemInformation");
    if (fn && purgeStandby) {
        const int SystemMemoryListInformation = 80;
        int cmd = 4;  // MemoryPurgeStandbyList
        r.standbyPurged = fn(SystemMemoryListInformation, &cmd, sizeof(cmd)) == 0;
    }
    MemInfo after = GetMemInfo();
    r.freedBytes = (LONGLONG)after.avail - (LONGLONG)before.avail;
    Log("[OK] Очистка ОЗУ: звільнено %.0f МБ (процесів: %d, standby: %s)", r.freedBytes / 1048576.0, r.trimmed,
        r.standbyPurged ? "очищено" : (purgeStandby ? "без змін" : "не чіпали"));
    return r;
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

static double IcmpOnce(HANDLE h, const sockaddr_in& addr) {
    char payload[32] = "KairoBoost-ping";
    BYTE reply[sizeof(ICMP_ECHO_REPLY) + sizeof(payload) + 64];
    double t0 = NowMs();
    DWORD n = IcmpSendEcho(h, addr.sin_addr.s_addr, payload, sizeof(payload), nullptr, reply, sizeof(reply), 2000);
    double dt = NowMs() - t0;
    auto* rep = (ICMP_ECHO_REPLY*)reply;
    return (n > 0 && rep->Status == IP_SUCCESS) ? dt : -1;
}

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
    if (!r.resolved)
        Log("%s: не вдалося знайти адресу", r.target.c_str());
    else if (!r.received)
        Log("%s (%s): немає відповідей (100%% втрат)", r.target.c_str(), r.ip.c_str());
    else
        Log("%s (%s): мін %.1f / сер %.1f / макс %.1f мс, джитер %.1f мс, втрати %.0f%%", r.target.c_str(),
            r.ip.c_str(), r.min, r.avg, r.max, r.jitter, r.Loss());
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
    // Кілька імен через ";" — для ігор з різними версіями exe
    // (ділимо лише за ";", бо в іменах бувають пробіли: "League of Legends.exe")
    std::vector<std::string> want;
    std::string low = ToLower(exe), cur;
    for (char c : low + ";") {
        if (c == ';') {
            if (!cur.empty()) want.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (Process32First(snap, &pe)) {
        do {
            std::string name = ToLower(pe.szExeFile);
            if (std::find(want.begin(), want.end(), name) != want.end()) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

void RunGameMode(const std::string& exe, ULONG resolution, const std::atomic<bool>& stop, std::atomic<DWORD>* gamePid,
                 const std::function<void(DWORD)>& onStart) {
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
        if (onStart) onStart(pid);

        HANDLE h = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
        if (h) {
            Log("[%s] Пріоритет процесу: High", SetPriorityClass(h, HIGH_PRIORITY_CLASS) ? "OK" : "ПОМИЛКА");
            if (DisablePowerThrottling(h, PROCESS_POWER_THROTTLING_EXECUTION_SPEED |
                                              PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION))
                Log("[OK] EcoQoS / power throttling для гри вимкнено");
            else
                Log("EcoQoS: ця версія Windows не підтримує керування (пропущено)");
        } else {
            // Ігри з античитом (Fortnite + EasyAntiCheat) захищають свій процес — це нормально.
            Log("Процес захищений античитом — пріоритет ставиться через твік «Високий пріоритет CPU» (Fortnite).");
            h = OpenProcess(SYNCHRONIZE, FALSE, pid);
        }
        if (h) {
            while (!stop && WaitForSingleObject(h, 300) == WAIT_TIMEOUT) {
            }
            CloseHandle(h);
        } else {
            while (!stop && FindProcess(exe) == pid)
                for (int i = 0; i < 10 && !stop; ++i) Sleep(100);
        }
        if (gamePid) *gamePid = 0;
        if (!stop) Log("Гру закрито. Чекаю наступного запуску...");
    }
    TimerRelease();
    if (gamePid) *gamePid = 0;
    Log("Режим гри зупинено, Timer Resolution відпущено.");
}

}  // namespace lb
