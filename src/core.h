// KairoBoost — спільна логіка (Timer Resolution, мережа, реєстр з бекапом, пам'ять, режим гри).
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace lb {

extern const char* kAppName;     // "KairoBoost"
extern const char* kAppRegRoot;  // "SOFTWARE\\KairoBoost"

// ---- Лог --------------------------------------------------------------------
void SetLogSink(std::function<void(const std::string&)> sink);
void Log(const char* fmt, ...);

// ---- Загальне ---------------------------------------------------------------
bool Init();
void Shutdown();
// Економний режим для самої програми: нижчий пріоритет, EcoQoS (E-ядра), скидання власної пам'яті.
// Timer Resolution при цьому продовжує діяти.
void SetSelfLowPower(bool on);
double NowMs();
bool IsElevated();
DWORD WindowsBuild();
std::string ToLower(std::string s);
std::vector<std::string> Split(const std::string& s);
std::string GetEnv(const char* name);
bool FileExists(const std::string& path);
void RunHidden(const char* cmdline);
std::string RunCapture(const char* cmdline);  // запуск без вікна, повертає stdout

// ---- Реєстр з резервною копією ----------------------------------------------
// Перед першою зміною значення його оригінал (або "не існувало") зберігається,
// RegRestore повертає саме його.
struct RegValue {
    HKEY root;
    std::string path, name;
    DWORD type;  // REG_DWORD або REG_SZ
    DWORD dw = 0;
    std::string sz;
};
RegValue Dw(HKEY root, const std::string& path, const std::string& name, DWORD v);
RegValue Sz(HKEY root, const std::string& path, const std::string& name, const std::string& v);
bool RegApply(const RegValue& v, bool backup = true);
bool RegIsSet(const RegValue& v);
bool RegRestore(const RegValue& v);  // false = резервної копії не було
bool RegGetDword(HKEY root, const std::string& path, const std::string& name, DWORD& out);
bool RegGetString(HKEY root, const std::string& path, const std::string& name, std::string& out);
bool RegValueExists(HKEY root, const std::string& path, const std::string& name);
std::vector<std::string> RegSubKeys(HKEY root, const std::string& path);
int RestoreAllRegistryBackups();

// Довільний збережений стан (для служб, схем живлення тощо)
bool StateSave(const std::string& id, const std::string& value, bool overwrite = false);
bool StateLoad(const std::string& id, std::string& value);
void StateDelete(const std::string& id);

// ---- Служби -----------------------------------------------------------------
bool ServiceGetStart(const char* name, DWORD& start);
bool ServiceSetStart(const char* name, DWORD start);
void ServiceStop(const char* name);
void ServiceStart(const char* name);

// ---- Timer Resolution (100 нс: 5000 = 0.5 мс) -------------------------------
bool TimerQuery(ULONG& coarsest, ULONG& finest, ULONG& current);
bool TimerHold(ULONG value, ULONG* actual = nullptr);
void TimerRelease();
ULONG TimerHeld();
struct SleepStats {
    double avg = 0, min = 0, max = 0;
};
SleepStats MeasureSleep1(int samples, std::vector<float>* outSamples = nullptr);
ULONG AutoTuneResolution(const std::atomic<bool>* cancel = nullptr);
bool GlobalTimerSupported();

// ---- QoS ----------------------------------------------------------------------
bool QosIsSet(const std::string& exe);
bool QosApply(const std::string& exe);
bool QosRemove(const std::string& exe);
int QosRemoveAll();

void FlushDns();

// ---- Пам'ять ------------------------------------------------------------------
struct MemInfo {
    ULONGLONG total = 0, avail = 0;
    DWORD load = 0;  // %
};
MemInfo GetMemInfo();
struct CleanResult {
    LONGLONG freedBytes = 0;
    int trimmed = 0;
    bool standbyPurged = false;
};
// Скидає робочі набори процесів (крім exclude) і очищає standby-список.
// purgeStandby=false — лише робочі набори (при старті гри: кеш файлів гри лишається, завантаження не сповільнюється)
CleanResult CleanMemory(const std::vector<std::string>& exclude, bool purgeStandby = true);

// ---- Тест мережі --------------------------------------------------------------
struct PingResult {
    std::string target, ip;
    int sent = 0, received = 0;
    double min = 0, avg = 0, max = 0, jitter = 0;
    bool resolved = false;
    double Loss() const { return sent ? 100.0 * (sent - received) / sent : 100.0; }
    double Score() const;
};
using SampleFn = std::function<void(int, double)>;
PingResult Ping(const std::string& target, int count, const SampleFn& sample = nullptr,
                const std::atomic<bool>* cancel = nullptr);
void LogPing(const PingResult& r);

// ---- Процеси / режим гри --------------------------------------------------------
DWORD FindProcess(const std::string& exe);
// onStart(pid) викликається щоразу, коли гру знайдено.
void RunGameMode(const std::string& exe, ULONG resolution, const std::atomic<bool>& stop,
                 std::atomic<DWORD>* gamePid = nullptr, const std::function<void(DWORD)>& onStart = nullptr);

}  // namespace lb
