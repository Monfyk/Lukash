#include "tweaks.h"

#include <powrprof.h>
#include <powersetting.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

#include "dns.h"
#include "edition.h"
#include "fortnite.h"
#include "games.h"

namespace tw {

using lb::Dw;
using lb::RegValue;
using lb::Sz;

static const HKEY LM = HKEY_LOCAL_MACHINE;
static const HKEY CU = HKEY_CURRENT_USER;

static const char* kTcpIfaces = "SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces";
static const char* kNetClass = "SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e972-e325-11ce-bfc1-08002be10318}";
static const char* kSysProfile = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile";
static const char* kGamesTask = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile\\Tasks\\Games";
static const char* kKernel = "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\kernel";
static const char* kIfeo = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\";

const char* CategoryName(Category c) {
    static const char* n[] = {"Мережа", "Система", "Процесор", "Відеокарта", "Миша і клавіатура", "Диск",
                              KB_ED("Fortnite", "Гра")};
    return c < CategoryCount ? n[c] : "";
}

// ---------------------------------------------------------------------------
// Динамічні списки значень
// ---------------------------------------------------------------------------

static bool IfaceHasIp(const std::string& path) {
    std::string ip;
    if (lb::RegGetString(LM, path, "DhcpIPAddress", ip) && !ip.empty() && ip != "0.0.0.0") return true;
    char buf[512];
    DWORD sz = sizeof(buf);
    return RegGetValueA(LM, path.c_str(), "IPAddress", RRF_RT_REG_MULTI_SZ, nullptr, buf, &sz) == ERROR_SUCCESS &&
           buf[0] && strcmp(buf, "0.0.0.0") != 0;
}

static std::vector<RegValue> NagleRegs() {
    std::vector<RegValue> v;
    for (auto& guid : lb::RegSubKeys(LM, kTcpIfaces)) {
        std::string p = std::string(kTcpIfaces) + "\\" + guid;
        if (!IfaceHasIp(p)) continue;
        v.push_back(Dw(LM, p, "TcpAckFrequency", 1));
        v.push_back(Dw(LM, p, "TCPNoDelay", 1));
        v.push_back(Dw(LM, p, "TcpDelAckTicks", 0));
    }
    return v;
}

// Енергозбереження фізичних мережевих адаптерів
static std::vector<RegValue> NetPowerRegs() {
    std::vector<RegValue> v;
    static const char* kOff[] = {"*EEE", "EEELinkAdvertisement", "EnableGreenEthernet", "GreenEthernet",
                                 "PowerSavingMode", "ULPMode", "*SelectiveSuspend", "AutoPowerSaveModeEnabled",
                                 "EnablePowerManagement", "ReduceSpeedOnPowerDown", "SavePowerNowEnabled"};
    for (auto& sub : lb::RegSubKeys(LM, kNetClass)) {
        if (sub.size() != 4 || sub[0] < '0' || sub[0] > '9') continue;
        std::string p = std::string(kNetClass) + "\\" + sub;
        DWORD ch = 0;
        if (!lb::RegGetDword(LM, p, "Characteristics", ch) || !(ch & 0x4)) continue;  // NCF_PHYSICAL
        v.push_back(Dw(LM, p, "PnPCapabilities", 24));  // "Дозволити вимикати пристрій для економії" = ні
        for (const char* name : kOff)
            if (lb::RegValueExists(LM, p, name)) v.push_back(Sz(LM, p, name, "0"));
    }
    return v;
}

// Модерація переривань: адаптер збирає пакети пачками, щоб менше навантажувати CPU — ціною затримки
static std::vector<RegValue> NetIrqRegs() {
    std::vector<RegValue> v;
    for (auto& sub : lb::RegSubKeys(LM, kNetClass)) {
        if (sub.size() != 4 || sub[0] < '0' || sub[0] > '9') continue;
        std::string p = std::string(kNetClass) + "\\" + sub;
        DWORD ch = 0;
        if (!lb::RegGetDword(LM, p, "Characteristics", ch) || !(ch & 0x4)) continue;
        if (lb::RegValueExists(LM, p, "*InterruptModeration")) v.push_back(Sz(LM, p, "*InterruptModeration", "0"));
    }
    return v;
}

// Windows 11: "Оптимізації для віконних ігор" — DX10/11 у вікні без рамки отримують сучасний
// flip-режим виводу (менша затримка, підтримка VRR), як у повноекранному режимі.
static const char* kDxUser = "Software\\Microsoft\\DirectX\\UserGpuPreferences";
static const char* kDxGlobal = "DirectXUserGlobalSettings";

static std::vector<RegValue> WinOptRegs() {
    std::string cur;
    lb::RegGetString(CU, kDxUser, kDxGlobal, cur);
    // Зберігаємо інші параметри в рядку, міняємо лише SwapEffectUpgradeEnable
    std::string out;
    size_t pos = 0;
    while (pos < cur.size()) {
        size_t end = cur.find(';', pos);
        std::string item = cur.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        if (!item.empty() && item.rfind("SwapEffectUpgradeEnable=", 0) != 0) out += item + ";";
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    out += "SwapEffectUpgradeEnable=1;";
    return {Sz(CU, kDxUser, kDxGlobal, out)};
}

static bool WinOptOn() {
    std::string cur;
    return lb::RegGetString(CU, kDxUser, kDxGlobal, cur) && cur.find("SwapEffectUpgradeEnable=1;") != std::string::npos;
}

// ---------------------------------------------------------------------------
// Частота оновлення монітора (основний дисплей)
// ---------------------------------------------------------------------------

static bool DisplayModes(DEVMODEA& cur, DEVMODEA& best, int& distinct) {
    cur = {};
    cur.dmSize = sizeof(cur);
    if (!EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &cur)) return false;
    best = cur;
    std::vector<DWORD> freqs;
    DEVMODEA m{};
    m.dmSize = sizeof(m);
    for (DWORD i = 0; EnumDisplaySettingsA(nullptr, i, &m); ++i) {
        if (m.dmPelsWidth != cur.dmPelsWidth || m.dmPelsHeight != cur.dmPelsHeight || m.dmBitsPerPel != cur.dmBitsPerPel)
            continue;
        if (std::find(freqs.begin(), freqs.end(), m.dmDisplayFrequency) == freqs.end()) freqs.push_back(m.dmDisplayFrequency);
        if (m.dmDisplayFrequency > best.dmDisplayFrequency) best = m;
    }
    distinct = (int)freqs.size();
    return true;
}

bool MonitorRefresh(unsigned& curHz, unsigned& maxHz) {
    DEVMODEA cur, best;
    int n;
    if (!DisplayModes(cur, best, n)) return false;
    curHz = cur.dmDisplayFrequency;
    maxHz = best.dmDisplayFrequency;
    return true;
}

static bool SetRefresh(DWORD hz) {
    DEVMODEA cur, best;
    int n;
    if (!DisplayModes(cur, best, n)) return false;
    DEVMODEA m = cur;
    m.dmDisplayFrequency = hz;
    m.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
    return ChangeDisplaySettingsA(&m, CDS_UPDATEREGISTRY) == DISP_CHANGE_SUCCESSFUL;
}

static bool RefreshOn() {
    DEVMODEA cur, best;
    int n;
    return DisplayModes(cur, best, n) && n > 1 && cur.dmDisplayFrequency >= best.dmDisplayFrequency;
}

static bool RefreshApply() {
    DEVMODEA cur, best;
    int n;
    if (!DisplayModes(cur, best, n)) return false;
    lb::StateSave("hz", std::to_string(cur.dmDisplayFrequency));
    bool ok = SetRefresh(best.dmDisplayFrequency);
    if (ok) lb::Log("Частота монітора: %lu Гц -> %lu Гц", cur.dmDisplayFrequency, best.dmDisplayFrequency);
    return ok;
}

static bool RefreshRevert() {
    std::string v;
    if (!lb::StateLoad("hz", v)) return false;
    bool ok = SetRefresh((DWORD)atoi(v.c_str()));
    lb::StateDelete("hz");
    return ok;
}

static std::string RefreshUnavailable() {
    DEVMODEA cur, best;
    int n;
    if (!DisplayModes(cur, best, n)) return "Не вдалося прочитати режими монітора";
    if (n <= 1) return "Монітор підтримує лише " + std::to_string(cur.dmDisplayFrequency) + " Гц";
    return "";
}

// Значення для повного шляху до exe поточної гри (налаштування графіки / сумісності Windows)
static std::vector<RegValue> FortniteExeRegs(const char* subkey, const std::string& value) {
    std::string exe = game::CurrentPath();
    if (exe.empty()) return {};
    return {Sz(CU, subkey, exe, value)};
}

// ---------------------------------------------------------------------------
// Служби
// ---------------------------------------------------------------------------

static bool SvcDisabled(const char* name) {
    DWORD s;
    return lb::ServiceGetStart(name, s) && s == SERVICE_DISABLED;
}

static bool SvcDisable(const char* name) {
    DWORD s;
    if (!lb::ServiceGetStart(name, s)) return false;
    lb::StateSave(std::string("svc|") + name, std::to_string(s));
    bool ok = lb::ServiceSetStart(name, SERVICE_DISABLED);
    lb::ServiceStop(name);
    return ok;
}

static bool SvcRestore(const char* name) {
    std::string v;
    DWORD start = SERVICE_AUTO_START;
    if (lb::StateLoad(std::string("svc|") + name, v)) start = (DWORD)atoi(v.c_str());
    bool ok = lb::ServiceSetStart(name, start);
    if (start == SERVICE_AUTO_START) lb::ServiceStart(name);
    lb::StateDelete(std::string("svc|") + name);
    return ok;
}

static std::string SvcMissing(const char* name) {
    DWORD s;
    return lb::ServiceGetStart(name, s) ? "" : "Службу не знайдено на цьому ПК";
}

// ---------------------------------------------------------------------------
// Живлення
// ---------------------------------------------------------------------------

static const GUID kUltimate = {0xe9a42b02, 0xd5df, 0x448d, {0xaa, 0x00, 0x03, 0xf1, 0x47, 0x49, 0xeb, 0x61}};
static const GUID kHighPerf = {0x8c5e7fda, 0xe8bf, 0x4a96, {0x9a, 0x85, 0xa6, 0xe2, 0x3a, 0x8c, 0x63, 0x5c}};
static const GUID kBalanced = {0x381b4222, 0xf694, 0x41f0, {0x96, 0x85, 0xff, 0x5b, 0xb2, 0x60, 0xdf, 0x2e}};
static const GUID kSubProcessor = {0x54533251, 0x82be, 0x4824, {0x96, 0xc1, 0x47, 0xb6, 0x0b, 0x74, 0x0d, 0x00}};
static const GUID kCpMinCores = {0x0cc5b647, 0xc1df, 0x4637, {0x89, 0x1a, 0xde, 0xc3, 0x5c, 0x31, 0x85, 0x83}};

static std::string GuidStr(const GUID& g) {
    char b[64];
    snprintf(b, sizeof(b), "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", (unsigned long)g.Data1, g.Data2,
             g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return b;
}

static bool ParseGuid(const std::string& s, GUID& g) {
    unsigned long d1;
    unsigned int d2, d3, b[8];
    if (sscanf(s.c_str(), "%8lx-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x", &d1, &d2, &d3, &b[0], &b[1], &b[2], &b[3], &b[4],
               &b[5], &b[6], &b[7]) != 11)
        return false;
    g.Data1 = d1;
    g.Data2 = (unsigned short)d2;
    g.Data3 = (unsigned short)d3;
    for (int i = 0; i < 8; ++i) g.Data4[i] = (unsigned char)b[i];
    return true;
}

static bool ActiveScheme(GUID& g) {
    GUID* p = nullptr;
    if (PowerGetActiveScheme(nullptr, &p) != ERROR_SUCCESS || !p) return false;
    g = *p;
    LocalFree(p);
    return true;
}

static bool PowerPlanOn() {
    GUID a;
    if (!ActiveScheme(a)) return false;
    std::string ours;
    if (lb::StateLoad("pwr|ours", ours) && ours == GuidStr(a)) return true;
    return IsEqualGUID(a, kUltimate) || IsEqualGUID(a, kHighPerf);
}

static bool PowerPlanApply() {
    GUID prev;
    if (ActiveScheme(prev)) lb::StateSave("pwr|prev", GuidStr(prev));
    // "Максимальна продуктивність" прихована на більшості ПК — створюємо її копію
    GUID target = kHighPerf;
    std::string ours;
    GUID oursG;
    if (lb::StateLoad("pwr|ours", ours) && ParseGuid(ours, oursG) && PowerSetActiveScheme(nullptr, &oursG) == 0)
        return true;
    // Те саме, що "powercfg -duplicatescheme": виводить GUID нової схеми
    std::string out = lb::RunCapture("powercfg.exe -duplicatescheme e9a42b02-d5df-448d-aa00-03f14749eb61");
    for (size_t i = 0; i + 36 <= out.size(); ++i) {
        GUID g;
        if (out[i + 8] == '-' && out[i + 13] == '-' && ParseGuid(out.substr(i, 36), g)) {
            target = g;
            lb::StateSave("pwr|ours", GuidStr(target), true);
            break;
        }
    }
    bool ok = PowerSetActiveScheme(nullptr, &target) == ERROR_SUCCESS;
    if (!ok) ok = PowerSetActiveScheme(nullptr, &kHighPerf) == ERROR_SUCCESS;
    return ok;
}

static bool PowerPlanRevert() {
    GUID prev = kBalanced;
    std::string s;
    if (lb::StateLoad("pwr|prev", s)) ParseGuid(s, prev);
    bool ok = PowerSetActiveScheme(nullptr, &prev) == ERROR_SUCCESS;
    GUID ours;
    if (lb::StateLoad("pwr|ours", s) && ParseGuid(s, ours) && !IsEqualGUID(ours, prev)) PowerDeleteScheme(nullptr, &ours);
    lb::StateDelete("pwr|ours");
    lb::StateDelete("pwr|prev");
    return ok;
}

// «Мінімум активних ядер» (CPMINCORES) поточної схеми живлення
static bool CoresRead(DWORD& ac, DWORD& dc) {
    GUID a;
    if (!ActiveScheme(a)) return false;
    DWORD r = PowerReadACValueIndex(nullptr, &a, &kSubProcessor, &kCpMinCores, &ac);
    PowerReadDCValueIndex(nullptr, &a, &kSubProcessor, &kCpMinCores, &dc);
    return r == ERROR_SUCCESS;
}

static bool CoresOn() {
    DWORD ac = 0, dc = 0;
    return CoresRead(ac, dc) && ac == 100;
}

static std::string CoresUnavailable() {
    DWORD ac, dc;
    return CoresRead(ac, dc) ? "" : "Windows не дає керувати паркуванням ядер на цьому ПК";
}

// Запис значень: спершу через API, якщо Windows відмовила — через powercfg; успіх перевіряємо читанням
static bool CoresWrite(DWORD ac, DWORD dc) {
    GUID a;
    if (!ActiveScheme(a)) {
        lb::Log("[!] Не вдалося визначити активну схему живлення");
        return false;
    }
    DWORD ra = PowerWriteACValueIndex(nullptr, &a, &kSubProcessor, &kCpMinCores, ac);
    DWORD rd = PowerWriteDCValueIndex(nullptr, &a, &kSubProcessor, &kCpMinCores, dc);
    PowerSetActiveScheme(nullptr, &a);  // застосувати зміни
    DWORD nac = 0, ndc = 0;
    if (CoresRead(nac, ndc) && nac == ac) return true;
    lb::Log("Запис через API не вдався (код %lu / %lu), пробую powercfg...", ra, rd);
    std::string out = lb::RunCapture(("powercfg.exe /setacvalueindex SCHEME_CURRENT SUB_PROCESSOR CPMINCORES " +
                                      std::to_string(ac)).c_str());
    lb::RunCapture(("powercfg.exe /setdcvalueindex SCHEME_CURRENT SUB_PROCESSOR CPMINCORES " + std::to_string(dc)).c_str());
    lb::RunHidden("powercfg.exe /setactive SCHEME_CURRENT");
    if (CoresRead(nac, ndc) && nac == ac) return true;
    // Прибираємо переноси рядків, щоб повідомлення powercfg вмістилося в журнал
    for (auto& c : out)
        if (c == '\r' || c == '\n') c = ' ';
    lb::Log("[!] Windows не дозволила змінити паркування ядер (код %lu)%s%s", ra, out.empty() ? "" : ": ", out.c_str());
    return false;
}

static bool CoresApply() {
    GUID a;
    DWORD ac = 0, dc = 0;
    if (!ActiveScheme(a) || !CoresRead(ac, dc)) return false;
    lb::StateSave("cores|" + GuidStr(a), std::to_string(ac) + " " + std::to_string(dc));
    return CoresWrite(100, 100);
}

static bool CoresRevert() {
    GUID a;
    if (!ActiveScheme(a)) return false;
    std::string s;
    unsigned ac = 10, dc = 10;  // типові значення Windows (Збалансована)
    if (lb::StateLoad("cores|" + GuidStr(a), s)) sscanf(s.c_str(), "%u %u", &ac, &dc);
    bool ok = CoresWrite(ac, dc);
    if (ok) lb::StateDelete("cores|" + GuidStr(a));
    return ok;
}

// ---------------------------------------------------------------------------
// Миша і клавіатура — застосувати без перезаходу
// ---------------------------------------------------------------------------

static int RegInt(HKEY root, const char* path, const char* name, int def) {
    std::string s;
    return lb::RegGetString(root, path, name, s) ? atoi(s.c_str()) : def;
}

static void RefreshMouse() {
    int m[3] = {RegInt(CU, "Control Panel\\Mouse", "MouseThreshold1", 6),
                RegInt(CU, "Control Panel\\Mouse", "MouseThreshold2", 10),
                RegInt(CU, "Control Panel\\Mouse", "MouseSpeed", 1)};
    SystemParametersInfoA(SPI_SETMOUSE, 0, m, SPIF_SENDCHANGE);
}

static const char* kKbResponse = "Control Panel\\Accessibility\\Keyboard Response";
static std::atomic<int> g_kbDelay{150}, g_kbInterval{10};

void SetKeyRepeat(int delayMs, int intervalMs) {
    g_kbDelay = std::clamp(delayMs, 100, 1000);
    g_kbInterval = std::clamp(intervalMs, 5, 500);
}

void GetKeyRepeat(int& delayMs, int& intervalMs) {
    delayMs = g_kbDelay;
    intervalMs = g_kbInterval;
}

// Стандартні налаштування Windows дозволяють максимум ~30 повторів/с із затримкою 250 мс.
// Швидший повтор дає вбудована функція "Фільтрація введення" (FilterKeys) з власними інтервалами —
// при нульових DelayBeforeAcceptance і BounceTime вона нічого не фільтрує, лише задає повтор.
static void RefreshKeyboard() {
    SystemParametersInfoA(SPI_SETKEYBOARDDELAY, RegInt(CU, "Control Panel\\Keyboard", "KeyboardDelay", 1), nullptr,
                          SPIF_SENDCHANGE);
    SystemParametersInfoA(SPI_SETKEYBOARDSPEED, RegInt(CU, "Control Panel\\Keyboard", "KeyboardSpeed", 31), nullptr,
                          SPIF_SENDCHANGE);
    FILTERKEYS fk{};
    fk.cbSize = sizeof(fk);
    fk.dwFlags = (DWORD)RegInt(CU, kKbResponse, "Flags", 126);
    fk.iWaitMSec = (DWORD)RegInt(CU, kKbResponse, "DelayBeforeAcceptance", 1000);
    fk.iDelayMSec = (DWORD)RegInt(CU, kKbResponse, "AutoRepeatDelay", 1000);
    fk.iRepeatMSec = (DWORD)RegInt(CU, kKbResponse, "AutoRepeatRate", 500);
    fk.iBounceMSec = (DWORD)RegInt(CU, kKbResponse, "BounceTime", 0);
    SystemParametersInfoA(SPI_SETFILTERKEYS, sizeof(fk), &fk, SPIF_SENDCHANGE);
}

// ---------------------------------------------------------------------------
// Каталог
// ---------------------------------------------------------------------------

static std::string NoFortnite() {
    if (game::CurrentExes().empty()) return "Вкажи exe своєї гри";
    return game::CurrentPath().empty() ? KB_ED("Fortnite не знайдено — вкажи папку гри",
                                               "Вкажи шлях до гри або запусти її")
                                       : "";
}

static std::string NoGameExe() { return game::CurrentExes().empty() ? "Вкажи exe своєї гри" : ""; }

static std::vector<RegValue> PriorityRegs() {
    std::vector<RegValue> v;
    for (auto& e : game::CurrentExes()) v.push_back(Dw(LM, std::string(kIfeo) + e + "\\PerfOptions", "CpuPriorityClass", 3));
    return v;
}

static bool GameQosOn() {
    auto ex = game::CurrentExes();
    if (ex.empty()) return false;
    for (auto& e : ex)
        if (!lb::QosIsSet(e)) return false;
    return true;
}
static bool GameQosApply() {
    bool ok = !game::CurrentExes().empty();
    for (auto& e : game::CurrentExes()) ok = lb::QosApply(e) && ok;
    return ok;
}
static bool GameQosRemove() {
    bool ok = true;
    for (auto& e : game::CurrentExes()) ok = lb::QosRemove(e) && ok;
    return ok;
}

const std::vector<Tweak>& All() {
    static const std::vector<Tweak> list = [] {
        std::vector<Tweak> v;
        auto add = [&](Tweak t) { v.push_back(std::move(t)); };

        // ---- Мережа ----
        add({"nagle", Network, "Вимкнути алгоритм Нейгла",
             "TCP-пакети відправляються одразу. Діє на лаунчер, лобі й чат; ігровий трафік Fortnite іде по UDP, "
             "тож на пінг у матчі не впливає.",
             "Майже ніколи не шкодить.", true, true,
             NagleRegs});
        add({"throttle", Network, "Без обмеження мережевих пакетів",
             "NetworkThrottlingIndex: Windows не обмежує обробку пакетів, поки грає музика чи відео.",
             "Немає суттєвих мінусів.", true, true,
             [] { return std::vector<RegValue>{Dw(LM, kSysProfile, "NetworkThrottlingIndex", 0xFFFFFFFF)}; }});
        add({"netpower", Network, "Вимкнути енергозбереження мережі",
             "Мережевий адаптер не засинає і не знижує швидкість (Green Ethernet, EEE, Selective Suspend).",
             "Ноутбук від батареї — трохи швидше сідатиме.", true, false, NetPowerRegs});

        // ---- Система ----
        add({"mmcss", System, "Максимальний пріоритет для ігор",
             "Планувальник Windows (MMCSS) дає вищий пріоритет потокам, що ним користуються: звук, частина ігор і "
             "відео. Зменшує ризик фризів звуку і кадрів під навантаженням.",
             "Немає суттєвих мінусів.", true, true, [] {
                 return std::vector<RegValue>{Dw(LM, kGamesTask, "GPU Priority", 8), Dw(LM, kGamesTask, "Priority", 6),
                                              Sz(LM, kGamesTask, "Scheduling Category", "High"),
                                              Sz(LM, kGamesTask, "SFIO Priority", "High")};
             }});
        add({"sysresp", System, "Мінімум ресурсів для фонових задач",
             "SystemResponsiveness = 10: менше процесорного часу резервується для фону.",
             "Якщо стрімиш або записуєш відео тим самим ПК — запис може смикатися.", true, true,
             [] { return std::vector<RegValue>{Dw(LM, kSysProfile, "SystemResponsiveness", 10)}; }});
        add({"gamemode", System, "Ігровий режим Windows",
             "Windows віддає грі пріоритет і не встановлює оновлення під час гри. У Windows 10/11 зазвичай уже "
             "ввімкнено — твік гарантує, що його ніхто не вимкнув.", "Немає суттєвих мінусів.", false,
             true, [] {
                 return std::vector<RegValue>{Dw(CU, "Software\\Microsoft\\GameBar", "AutoGameModeEnabled", 1),
                                              Dw(CU, "Software\\Microsoft\\GameBar", "AllowAutoGameMode", 1)};
             }});
        add({"gamedvr", System, "Вимкнути фоновий запис ігрової панелі",
             "Xbox Game Bar не записує гру у фоні — менше навантаження на відеокарту і диск.",
             "Якщо користуєшся записом кліпів через Win+Alt+G.", false, true, [] {
                 return std::vector<RegValue>{
                     Dw(CU, "System\\GameConfigStore", "GameDVR_Enabled", 0),
                     Dw(CU, "Software\\Microsoft\\Windows\\CurrentVersion\\GameDVR", "AppCaptureEnabled", 0),
                     Dw(LM, "SOFTWARE\\Policies\\Microsoft\\Windows\\GameDVR", "AllowGameDVR", 0)};
             }});
        add({"globaltimer", System, "Глобальний Timer Resolution",
             "Роздільність таймера з KairoBoost діє на всі процеси, зокрема на Fortnite (Windows 11).",
             "Трохи більше споживання енергії, поки KairoBoost тримає таймер.", true, true,
             [] { return std::vector<RegValue>{Dw(LM, kKernel, "GlobalTimerResolutionRequests", 1)}; }, nullptr,
             nullptr, nullptr, nullptr,
             [] { return lb::GlobalTimerSupported() ? std::string() : std::string("Потрібна Windows 11"); }});
        add({"sysmain", System, "Вимкнути SysMain (SuperFetch)",
             "Windows не підвантажує програми в пам'ять наперед.",
             "ПК з HDD: програми запускатимуться повільніше. На SSD ефект малий.", false, false, nullptr,
             [] { return SvcDisabled("SysMain"); }, [] { return SvcDisable("SysMain"); },
             [] { return SvcRestore("SysMain"); }, nullptr, [] { return SvcMissing("SysMain"); }});
        add({"wsearch", Disk, "Вимкнути індексацію файлів",
             "Служба пошуку Windows не сканує диск у фоні.", "Пошук у меню Пуск і Провіднику стане повільнішим.",
             false, false, nullptr, [] { return SvcDisabled("WSearch"); }, [] { return SvcDisable("WSearch"); },
             [] { return SvcRestore("WSearch"); }, nullptr, [] { return SvcMissing("WSearch"); }});
        add({"lastaccess", Disk, "Вимкнути мітки останнього доступу",
             "NTFS не записує час кожного відкриття файлу — менше дрібних записів на диск.",
             "Якщо користуєшся програмами резервного копіювання, що на це спираються (рідко).", true, false,
             [] {
                 return std::vector<RegValue>{
                     Dw(LM, "SYSTEM\\CurrentControlSet\\Control\\FileSystem", "NtfsDisableLastAccessUpdate", 0x80000001)};
             },
             [] {
                 DWORD d;
                 return lb::RegGetDword(LM, "SYSTEM\\CurrentControlSet\\Control\\FileSystem",
                                        "NtfsDisableLastAccessUpdate", d) &&
                        (d & 1);
             }});

        // ---- Процесор ----
        add({"powerplan", Cpu, "Схема живлення «Максимальна продуктивність»",
             "Процесор не знижує частоту і швидко реагує на навантаження.",
             "Ноутбук від батареї: гріється і швидше сідає.", false, true, nullptr, PowerPlanOn, PowerPlanApply,
             PowerPlanRevert});
        add({"cores", Cpu, "Тримати всі ядра активними",
             "Вимикає «паркування» ядер: Windows не присипляє ядра процесора під час гри.",
             "Ноутбуки: вище температура і споживання.", false, false, nullptr, CoresOn, CoresApply, CoresRevert,
             nullptr, CoresUnavailable});

        // ---- Відеокарта ----
        add({"refresh", Gpu, "Максимальна частота монітора",
             "Windows часто лишає 60 Гц навіть на моніторі 144/240 Гц — тоді ти бачиш лише 60 кадрів. Ставить "
             "найвищу частоту, яку підтримує монітор.",
             "Якщо монітор блимає або гасне на високій частоті (поганий кабель) — вимкни, повернеться як було.",
             false, true, nullptr, RefreshOn, RefreshApply, RefreshRevert, nullptr, RefreshUnavailable});
        add({"netirq", Network, "Без затримки переривань мережі",
             "Мережева карта передає кожен пакет одразу, а не пачками: трохи менші затримка і джитер.",
             "Слабкий процесор і дуже швидкий інтернет (1 Гбіт/с і більше) — трохи вище навантаження на CPU.", true,
             false, NetIrqRegs, nullptr, nullptr, nullptr, nullptr,
             [] { return NetIrqRegs().empty() ? std::string("Мережева карта не має цього параметра") : std::string(); }});
        add({"winopt", Gpu, "Оптимізації для віконних ігор",
             "Windows 11: гра у «вікні без рамки» виводить кадри як у повноекранному режимі — менша затримка "
             "і підтримка G-Sync/FreeSync.",
             "Рідко — конфлікти зі старими оверлеями. Якщо гра почне блимати, вимкни.", false, true, WinOptRegs,
             WinOptOn, nullptr, nullptr, nullptr,
             [] { return lb::WindowsBuild() >= 22621 ? std::string() : std::string("Потрібна Windows 11 22H2+"); }});
        add({"hags", Gpu, "Апаратне планування GPU",
             "Відеокарта сама керує своєю чергою кадрів — менша затримка і навантаження на CPU.",
             "Старі драйвери або відеокарти до GTX 1000 / RX 5000 — можуть бути фризи.", true, false,
             [] {
                 return std::vector<RegValue>{
                     Dw(LM, "SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers", "HwSchMode", 2)};
             },
             nullptr, nullptr, nullptr, nullptr,
             [] { return lb::WindowsBuild() >= 19041 ? std::string() : std::string("Потрібна Windows 10 2004+"); }});
        add({"fso", Gpu, "Вимкнути оптимізацію на весь екран",
             "Повноекранні ігри працюють у справжньому ексклюзивному режимі.",
             "Windows 11: часто краще залишити ввімкненою; перевір FPS в обох варіантах.", false, false, [] {
                 return std::vector<RegValue>{Dw(CU, "System\\GameConfigStore", "GameDVR_FSEBehaviorMode", 2),
                                              Dw(CU, "System\\GameConfigStore", "GameDVR_HonorUserFSEBehaviorMode", 1),
                                              Dw(CU, "System\\GameConfigStore", "GameDVR_FSEBehavior", 2),
                                              Dw(CU, "System\\GameConfigStore", "GameDVR_DXGIHonorFSEWindowsCompatible", 1)};
             }});

        // ---- Миша і клавіатура ----
        add({"mouseaccel", Input, "Вимкнути прискорення миші",
             "Курсор рухається рівно стільки, скільки рухаєш мишу (для прицілювання в меню і Windows).",
             "Немає мінусів для ігор. У Fortnite приціл і так працює з «сирим» вводом.", false, true,
             [] {
                 return std::vector<RegValue>{Sz(CU, "Control Panel\\Mouse", "MouseSpeed", "0"),
                                              Sz(CU, "Control Panel\\Mouse", "MouseThreshold1", "0"),
                                              Sz(CU, "Control Panel\\Mouse", "MouseThreshold2", "0")};
             },
             nullptr, nullptr, nullptr, RefreshMouse});
        add({"keyboard", Input, "Швидший повтор клавіш",
             "Повтор утриманої клавіші швидше за максимум Windows (250 мс, 30/с); значення — повзунками нижче. "
             "На керування в грі не впливає.",
             "Якщо під час набору часто дублюються символи. Вмикає «Фільтрацію введення» Windows (без фільтрації).",
             false, false,
             [] {
                 return std::vector<RegValue>{Sz(CU, "Control Panel\\Keyboard", "KeyboardDelay", "0"),
                                              Sz(CU, "Control Panel\\Keyboard", "KeyboardSpeed", "31"),
                                              Sz(CU, kKbResponse, "AutoRepeatDelay", std::to_string(g_kbDelay.load())),
                                              Sz(CU, kKbResponse, "AutoRepeatRate", std::to_string(g_kbInterval.load())),
                                              Sz(CU, kKbResponse, "DelayBeforeAcceptance", "0"),
                                              Sz(CU, kKbResponse, "BounceTime", "0"),
                                              Sz(CU, kKbResponse, "Flags", "3")};  // увімкнено, без гарячої клавіші
             },
             [] {
                 std::string f;
                 return lb::RegGetString(CU, kKbResponse, "Flags", f) && f == "3" &&
                        lb::RegValueExists(CU, kKbResponse, "AutoRepeatRate");
             },
             nullptr, nullptr, RefreshKeyboard});

        // ---- Гра (у звичайній редакції — Fortnite) ----
        add({"fn_priority", Fortnite, KB_ED("Високий пріоритет CPU для Fortnite", "Високий пріоритет CPU для гри"),
             KB_ED("Windows запускає Fortnite одразу з високим пріоритетом. Працює разом з EasyAntiCheat.",
                   "Windows запускає гру одразу з високим пріоритетом. Працює разом з античитами."),
             "Якщо поряд стрімиш/записуєш — OBS може отримати менше ресурсів.", false, true, PriorityRegs, nullptr,
             nullptr, nullptr, nullptr, NoGameExe});
        add({"fn_gpu", Fortnite, KB_ED("Fortnite на потужній відеокарті", "Гра на потужній відеокарті"),
             KB_ED("Windows завжди запускає Fortnite на дискретній відеокарті, а не на вбудованій.",
                   "Windows завжди запускає гру на дискретній відеокарті, а не на вбудованій."),
             "ПК лише з однією відеокартою — нічого не змінить (але й не зашкодить).", false, true,
             [] { return FortniteExeRegs("Software\\Microsoft\\DirectX\\UserGpuPreferences", "GpuPreference=2;"); },
             nullptr, nullptr, nullptr, nullptr, NoFortnite});
        add({"fn_fso", Fortnite, KB_ED("Без оптимізації на весь екран (Fortnite)", "Без оптимізації на весь екран (гра)"),
             KB_ED("Вимикає повноекранну оптимізацію лише для Fortnite (режим «Повноекранний» у грі).",
                   "Вимикає повноекранну оптимізацію лише для цієї гри (режим «Повноекранний» у грі)."),
             "Граєш у «Вікні без рамки» — ефекту не буде.", false, false,
             [] {
                 return FortniteExeRegs("Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers",
                                        "~ DISABLEDXMAXIMIZEDWINDOWEDMODE");
             },
             nullptr, nullptr, nullptr, nullptr, NoFortnite});
        add({"fn_qos", Fortnite, KB_ED("QoS-пріоритет пакетів Fortnite", "QoS-пріоритет пакетів гри"),
             "Пакети гри позначаються DSCP 46: Wi-Fi і роутер з QoS пропускають їх першими.",
             "Не допоможе, якщо в мережі ніхто інший нічого не качає.", false, true, nullptr, GameQosOn, GameQosApply,
             GameQosRemove, nullptr, NoGameExe});
        // Ці твіки підхоплюються при наступному запуску гри
        for (auto& t : v)
            for (const char* id : {"gamedvr", "fso", "winopt", "fn_priority", "fn_gpu", "fn_fso", "fn_qos"})
                if (std::string(t.id) == id) t.gameRestart = true;
        return v;
    }();
    return list;
}

bool ReadSystemKeyRepeat(int& delayMs, int& intervalMs) {
    const Tweak* t = Find("keyboard");
    if (!t || !IsOn(*t)) return false;
    delayMs = RegInt(CU, kKbResponse, "AutoRepeatDelay", 150);
    intervalMs = RegInt(CU, kKbResponse, "AutoRepeatRate", 10);
    return true;
}

const char* EffectText(const Tweak& t) {
    if (t.reboot) return "після перезавантаження ПК";
    if (t.gameRestart) return "після перезапуску гри";
    return "одразу";
}

const Tweak* Find(const std::string& id) {
    for (auto& t : All())
        if (id == t.id) return &t;
    return nullptr;
}

std::string Unavailable(const Tweak& t) { return t.unavailable ? t.unavailable() : ""; }

bool IsOn(const Tweak& t) {
    if (t.isOn) return t.isOn();
    if (!t.regs) return false;
    auto regs = t.regs();
    if (regs.empty()) return false;
    for (auto& r : regs)
        if (!lb::RegIsSet(r)) return false;
    return true;
}

bool Apply(const Tweak& t) {
    std::string why = Unavailable(t);
    if (!why.empty()) {
        lb::Log("[!] %s: %s", t.name, why.c_str());
        return false;
    }
    bool ok = true;
    if (t.apply) {
        ok = t.apply();
    } else if (t.regs) {
        auto regs = t.regs();
        if (regs.empty()) ok = false;
        for (auto& r : regs) ok = lb::RegApply(r) && ok;
    }
    if (t.after) t.after();
    lb::Log("[%s] Увімкнено: %s%s%s", ok ? "OK" : "ПОМИЛКА", t.name, ok ? " — діє " : "", ok ? EffectText(t) : "");
    return ok;
}

bool Revert(const Tweak& t) {
    bool ok = true;
    if (t.revert) {
        ok = t.revert();
    } else if (t.regs) {
        bool any = false;
        for (auto& r : t.regs()) any = lb::RegRestore(r) || any;
        if (!any) {
            lb::Log("%s: ці значення були встановлені ще до KairoBoost — залишаю як є", t.name);
            return false;
        }
    }
    if (t.after) t.after();
    lb::Log("[%s] Вимкнено: %s", ok ? "OK" : "ПОМИЛКА", t.name);
    return ok;
}

void ApplyRecommended(bool includeFortnite) {
    for (auto& t : All()) {
        if (!t.recommended || (t.category == Fortnite && !includeFortnite)) continue;
        if (!Unavailable(t).empty() || IsOn(t)) continue;
        Apply(t);
    }
}

void RevertEverything() {
    for (auto& t : All())
        if (IsOn(t)) Revert(t);
    int r = lb::RestoreAllRegistryBackups();
    int q = lb::QosRemoveAll();
    if (fn::HasSettingsBackup()) fn::RestoreSettingsBackup();
    if (dns::IsChanged()) dns::Restore();
    lb::Log("Усе відкочено (додатково відновлено значень реєстру: %d, QoS-політик: %d). Перезавантаж ПК.", r, q);
}

}  // namespace tw
