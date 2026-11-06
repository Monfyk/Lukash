// LukashBoost — спільна логіка (Timer Resolution, мережа, твіки, режим гри).
// Використовується і консольною, і графічною версією.
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

// ---- Лог --------------------------------------------------------------------
// Усі функції пишуть повідомлення через Log(); куди воно йде, вирішує застосунок.
void SetLogSink(std::function<void(const std::string&)> sink);
void Log(const char* fmt, ...);

// ---- Загальне ---------------------------------------------------------------
bool Init();  // WSAStartup + функції таймера з ntdll
void Shutdown();
double NowMs();
bool IsElevated();
DWORD WindowsBuild();
std::string ToLower(std::string s);
std::vector<std::string> Split(const std::string& s);

// ---- Timer Resolution (значення в одиницях 100 нс: 5000 = 0.5 мс) ----------
bool TimerQuery(ULONG& coarsest, ULONG& finest, ULONG& current);
bool TimerHold(ULONG value, ULONG* actual = nullptr);
void TimerRelease();
ULONG TimerHeld();  // 0 = нічого не тримаємо

struct SleepStats {
    double avg = 0, min = 0, max = 0;
};
SleepStats MeasureSleep1(int samples, std::vector<float>* outSamples = nullptr);
// Підбирає значення, при якому Sleep(1) найближчий до 1 мс. Залишає його утриманим.
ULONG AutoTuneResolution(const std::atomic<bool>* cancel = nullptr);
bool GlobalTimerSupported();
bool GlobalTimerEnabled();
void EnableGlobalTimerResolution();

// ---- Мережеві / системні твіки ---------------------------------------------
struct TweakStatus {
    int ifaces = 0, nagleOff = 0;
    bool throttlingOff = false;
    DWORD sysResponsiveness = 20;
    bool mmcss = false;
    bool globalTimer = false;
    bool nla = false;
    std::vector<std::string> qosGames;
};
TweakStatus GetTweakStatus();
void ApplyNetworkTweaks();
void ApplyQosForGame(const std::string& exe);
void RevertAll();
void FlushDns();

// ---- Тест мережі ------------------------------------------------------------
struct PingResult {
    std::string target, ip;
    int sent = 0, received = 0;
    double min = 0, avg = 0, max = 0, jitter = 0;
    bool resolved = false;
    double Loss() const { return sent ? 100.0 * (sent - received) / sent : 100.0; }
    double Score() const;  // менше = краще
};
// sample(index, rtt) — rtt < 0 означає втрачений пакет
using SampleFn = std::function<void(int, double)>;
// target: "host" (ICMP) або "host:port" (TCP-ping)
PingResult Ping(const std::string& target, int count, const SampleFn& sample = nullptr,
                const std::atomic<bool>* cancel = nullptr);
void LogPing(const PingResult& r);

// ---- Режим гри --------------------------------------------------------------
DWORD FindProcess(const std::string& exe);
// Блокує, поки stop != true. gamePid (необов'язково) — PID гри або 0, якщо чекаємо.
void RunGameMode(const std::string& exe, ULONG resolution, const std::atomic<bool>& stop,
                 std::atomic<DWORD>* gamePid = nullptr);

}  // namespace lb
