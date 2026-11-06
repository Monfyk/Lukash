#include "advisor.h"

#include "core.h"
#include "dns.h"
#include "fortnite.h"
#include "tweaks.h"

#include <dxgi.h>
#include <winioctl.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace adv {

// ---------------------------------------------------------------------------
// Збір даних
// ---------------------------------------------------------------------------

static std::string Utf8(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

static std::string TrimSpaces(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
    size_t a = s.find_first_not_of(' ');
    return a == std::string::npos ? "" : s.substr(a);
}

static void CollectCpu(SystemInfo& s) {
    lb::RegGetString(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString",
                     s.cpu);
    s.cpu = TrimSpaces(s.cpu);
    DWORD len = 0;
    GetLogicalProcessorInformation(nullptr, &len);
    std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> info(len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION) + 1);
    if (GetLogicalProcessorInformation(info.data(), &len)) {
        for (size_t i = 0; i < len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION); ++i) {
            if (info[i].Relationship != RelationProcessorCore) continue;
            ++s.physicalCores;
            ULONG_PTR m = info[i].ProcessorMask;
            while (m) {
                s.logicalCores += (int)(m & 1);
                m >>= 1;
            }
        }
    }
    if (!s.logicalCores) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        s.logicalCores = (int)si.dwNumberOfProcessors;
        s.physicalCores = s.logicalCores;
    }
}

static void CollectGpus(SystemInfo& s) {
    typedef HRESULT(WINAPI * Create_t)(REFIID, void**);
    HMODULE dxgi = LoadLibraryA("dxgi.dll");
    if (!dxgi) return;
    auto create = (Create_t)(void*)GetProcAddress(dxgi, "CreateDXGIFactory1");
    IDXGIFactory1* f = nullptr;
    if (create && SUCCEEDED(create(__uuidof(IDXGIFactory1), (void**)&f)) && f) {
        IDXGIAdapter1* a = nullptr;
        for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 d;
            if (SUCCEEDED(a->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && d.VendorId != 0x1414) {
                GpuInfo g;
                g.name = TrimSpaces(Utf8(d.Description));
                g.vendor = d.VendorId;
                g.vramMb = d.DedicatedVideoMemory / (1024 * 1024);
                // Вбудована: Intel (крім Arc із власною пам'яттю) або будь-яка з <512 МБ власної пам'яті
                g.integrated = g.vramMb < 512 || (d.VendorId == 0x8086 && g.vramMb < 2048);
                bool dup = false;
                for (auto& o : s.gpus)
                    if (o.name == g.name) dup = true;
                if (!dup) s.gpus.push_back(g);
            }
            a->Release();
        }
        f->Release();
    }
    FreeLibrary(dxgi);
}

// Модулі пам'яті з таблиці SMBIOS (тип 17): заявлена і поточна частота
static void CollectRam(SystemInfo& s) {
    s.ramMb = lb::GetMemInfo().total / (1024 * 1024);
    DWORD size = GetSystemFirmwareTable(0x52534D42 /* 'RSMB' */, 0, nullptr, 0);
    if (!size) return;
    std::vector<BYTE> buf(size);
    if (GetSystemFirmwareTable(0x52534D42 /* 'RSMB' */, 0, buf.data(), size) != size || size < 8) return;
    DWORD len;
    memcpy(&len, buf.data() + 4, 4);
    const BYTE* p = buf.data() + 8;
    const BYTE* end = p + std::min<DWORD>(len, size - 8);
    while (p + 4 <= end) {
        BYTE type = p[0], flen = p[1];
        if (flen < 4 || p + flen > end) break;
        if (type == 17 && flen >= 0x22) {
            WORD modSize, speed, conf;
            memcpy(&modSize, p + 0x0C, 2);
            memcpy(&speed, p + 0x15, 2);
            memcpy(&conf, p + 0x20, 2);
            BYTE memType = p[0x12];
            if (modSize != 0 && modSize != 0xFFFF) {
                ++s.ramModules;
                if (speed && speed != 0xFFFF) s.ramRatedSpeed = std::max<int>(s.ramRatedSpeed, speed);
                if (conf && conf != 0xFFFF) s.ramSpeed = s.ramSpeed ? std::min<int>(s.ramSpeed, conf) : conf;
                if (memType == 0x1A) s.ramType = 4;
                if (memType == 0x22) s.ramType = 5;
            }
        }
        if (type == 127) break;
        // Далі — рядки, що закінчуються подвійним нулем
        const BYTE* q = p + flen;
        while (q + 1 < end && !(q[0] == 0 && q[1] == 0)) ++q;
        p = q + 2;
    }
}

static void CollectDisk(SystemInfo& s) {
    char win[MAX_PATH];
    GetWindowsDirectoryA(win, MAX_PATH);
    std::string root = std::string(win).substr(0, 3);  // "C:\"
    ULARGE_INTEGER freeBytes;
    if (GetDiskFreeSpaceExA(root.c_str(), &freeBytes, nullptr, nullptr)) s.freeDiskGb = freeBytes.QuadPart >> 30;
    std::string dev = "\\\\.\\" + root.substr(0, 2);
    HANDLE h = CreateFileA(dev.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    STORAGE_PROPERTY_QUERY q{};
    q.PropertyId = StorageDeviceSeekPenaltyProperty;
    q.QueryType = PropertyStandardQuery;
    DEVICE_SEEK_PENALTY_DESCRIPTOR d{};
    DWORD got = 0;
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), &d, sizeof(d), &got, nullptr) && got >= sizeof(d)) {
        s.diskKnown = true;
        s.systemSsd = !d.IncursSeekPenalty;
    }
    CloseHandle(h);
}

SystemInfo Collect() {
    SystemInfo s;
    CollectCpu(s);
    CollectGpus(s);
    CollectRam(s);
    CollectDisk(s);
    SYSTEM_POWER_STATUS ps;
    if (GetSystemPowerStatus(&ps)) {
        s.hasBattery = ps.BatteryFlag != 128 && ps.BatteryFlag != 255;
        s.onBattery = s.hasBattery && ps.ACLineStatus == 0;
    }
    tw::MonitorRefresh(s.refreshCur, s.refreshMax);
    s.onWifi = dns::OnWifi();
    s.build = lb::WindowsBuild();
    s.fortniteFound = !fn::ExePath().empty();
    fn::Settings fs = fn::ReadSettings();
    s.fnSettingsFound = fs.found;
    s.fnPerf = fs.performanceMode;
    s.fnVsync = fs.vsync;
    s.fnFullscreen = fs.fullscreen;
    s.fnFps = fs.fpsLimit;
    return s;
}

bool SystemInfo::HasNvidia() const {
    for (auto& g : gpus)
        if (g.vendor == 0x10DE) return true;
    return false;
}
bool SystemInfo::HasAmdDedicated() const {
    for (auto& g : gpus)
        if (g.vendor == 0x1002 && !g.integrated) return true;
    return false;
}
bool SystemInfo::HasDiscrete() const {
    for (auto& g : gpus)
        if (!g.integrated) return true;
    return false;
}
bool SystemInfo::Hybrid() const {
    bool i = false, d = false;
    for (auto& g : gpus) (g.integrated ? i : d) = true;
    return i && d;
}

std::string SystemInfo::Summary() const {
    char b[256];
    std::string r;
    snprintf(b, sizeof(b), "Процесор: %s (%d ядер / %d потоків)\n", cpu.empty() ? "?" : cpu.c_str(), physicalCores,
             logicalCores);
    r += b;
    for (auto& g : gpus) {
        snprintf(b, sizeof(b), "Відеокарта: %s, %llu МБ%s\n", g.name.c_str(), g.vramMb, g.integrated ? " (вбудована)" : "");
        r += b;
    }
    snprintf(b, sizeof(b), "Пам'ять: %.0f ГБ", ramMb / 1024.0);
    r += b;
    if (ramSpeed) {
        snprintf(b, sizeof(b), ", DDR%d %d МТ/с (заявлено %d), модулів: %d", ramType ? ramType : 0, ramSpeed,
                 ramRatedSpeed, ramModules);
        r += b;
    }
    r += "\n";
    snprintf(b, sizeof(b), "Системний диск: %s, вільно %llu ГБ\n", diskKnown ? (systemSsd ? "SSD" : "HDD") : "?",
             freeDiskGb);
    r += b;
    if (refreshCur)
        snprintf(b, sizeof(b), "Монітор: %u Гц (максимум %u Гц)\n", refreshCur, refreshMax);
    else
        snprintf(b, sizeof(b), "Монітор: частоту не вдалося визначити\n");
    r += b;
    snprintf(b, sizeof(b), "Мережа: %s · %s · Windows build %u\n", onWifi ? "Wi-Fi" : "кабель",
             hasBattery ? (onBattery ? "ноутбук від батареї" : "ноутбук від мережі") : "стаціонарний ПК", build);
    r += b;
    return r;
}

// ---------------------------------------------------------------------------
// Правила
// ---------------------------------------------------------------------------

const char* LevelName(Level l) {
    switch (l) {
        case Critical: return "Найважливіше";
        case Recommended: return "Рекомендовано";
        case Optional: return "Можна спробувати";
        case Good: return "Уже добре";
        default: return "Не варто для твого ПК";
    }
}

static bool On(const char* id) {
    const tw::Tweak* t = tw::Find(id);
    return t && tw::Unavailable(*t).empty() && tw::IsOn(*t);
}

static bool Available(const char* id) {
    const tw::Tweak* t = tw::Find(id);
    return t && tw::Unavailable(*t).empty();
}

std::vector<Advice> Analyze(const SystemInfo& s) {
    std::vector<Advice> v;
    auto add = [&](Level l, std::string title, std::string text, ActionKind a = None, std::string id = "",
                   std::string page = "") { v.push_back({l, std::move(title), std::move(text), a, std::move(id), std::move(page)}); };
    // Твік: якщо ввімкнений — "уже добре", інакше — порада з кнопкою
    auto tweak = [&](const char* id, Level l, const std::string& title, const std::string& why) {
        if (!Available(id)) return;
        if (On(id))
            add(Good, title, why);
        else
            add(l, title, why, EnableTweak, id);
    };
    char b[512];
    bool laptop = s.hasBattery;
    bool weakCpu = s.physicalCores > 0 && s.physicalCores <= 4;
    bool weakGpu = !s.HasDiscrete() || std::all_of(s.gpus.begin(), s.gpus.end(), [](const GpuInfo& g) {
                       return g.integrated || g.vramMb < 4096;
                   });

    // --- Монітор ---
    if (s.refreshMax > s.refreshCur && s.refreshCur) {
        snprintf(b, sizeof(b),
                 "Монітор працює на %u Гц, а підтримує %u Гц. Ти бачиш лише %u кадрів на секунду, хоч би скільки "
                 "FPS видавала гра. Це найпомітніше покращення з усіх.",
                 s.refreshCur, s.refreshMax, s.refreshCur);
        add(Critical, "Увімкни максимальну частоту монітора", b, EnableTweak, "refresh");
    } else if (s.refreshCur) {
        snprintf(b, sizeof(b), "Монітор уже працює на максимальних %u Гц.", s.refreshCur);
        add(Good, "Частота монітора", b);
    }

    // --- Мережа ---
    if (s.onWifi) {
        add(Critical, "Підключи ПК кабелем",
            "Ти граєш через Wi-Fi. Кабель зазвичай дає мінус 5–30 мс пінгу і прибирає стрибки та втрати пакетів — "
            "більше, ніж усі мережеві твіки разом.");
        tweak("netpower", Recommended, "Вимкни енергозбереження Wi-Fi адаптера",
              "На Wi-Fi адаптер часто «засинає» між пакетами — звідси стрибки пінгу.");
    } else {
        add(Good, "Підключення кабелем", "Ти на кабелі — це найкраще для пінгу.");
        tweak("netpower", Optional, "Вимкни енергозбереження мережевої карти",
              "Мережева карта не знижуватиме швидкість і не засинатиме.");
    }
    tweak("fn_qos", Recommended, "QoS-пріоритет для Fortnite",
          "Роутер і Wi-Fi пропускатимуть пакети гри першими, коли вдома хтось качає чи дивиться відео.");
    tweak("throttle", Recommended, "Без обмеження мережевих пакетів", "Windows не гальмуватиме обробку пакетів.");
    if (s.physicalCores >= 6)
        tweak("netirq", Optional, "Без затримки переривань мережі",
              "У тебе достатньо ядер, тож трохи вище навантаження на CPU не завадить, а затримка трохи зменшиться.");
    else if (Available("netirq") && !On("netirq"))
        add(NotForYou, "Модерація переривань мережі", "На процесорі з 4 ядрами і менше краще не вимикати — "
            "навантаження на CPU зросте, а виграш мізерний.");
    add(Optional, "Перевір регіон Fortnite",
        "Вибери в грі регіон із найменшим пінгом — це реально знижує пінг у матчі.", OpenPage, "", "fortnite");

    // --- Живлення / процесор ---
    if (laptop) {
        if (s.onBattery)
            add(NotForYou, "Схема «Максимальна продуктивність» від батареї",
                "Ноутбук зараз від батареї: схема максимальної продуктивності сильно скоротить час роботи. "
                "Підключи зарядку для гри — тоді вмикай.");
        else
            tweak("powerplan", Recommended, "Схема живлення «Максимальна продуктивність»",
                  "Ноутбук від мережі — процесор не скидатиме частоту під час гри. Вимкни, коли працюєш від батареї.");
        if (Available("cores") && !On("cores"))
            add(NotForYou, "Тримати всі ядра активними",
                "На ноутбуці це підвищує температуру; гарячий процесор скидає частоту — FPS може навіть впасти.");
    } else {
        tweak("powerplan", Recommended, "Схема живлення «Максимальна продуктивність»",
              "На стаціонарному ПК мінусів немає: процесор завжди готовий до піку навантаження.");
        tweak("cores", Optional, "Тримати всі ядра активними",
              "Менше мікрофризів, коли гра раптово навантажує процесор.");
    }

    // --- Таймер ---
    if (lb::GlobalTimerSupported())
        tweak("globaltimer", Recommended, "Глобальний Timer Resolution",
              "У Windows 11 без цього точний таймер KairoBoost не діє на Fortnite.");
    else
        add(Optional, "Timer Resolution на Windows 10",
            "На Windows 10 (2004+) точний таймер діє лише на процес, що його задав, тож на Fortnite ефект менший. "
            "Сама гра зазвичай і так просить точний таймер.");

    // --- Відеокарта ---
    if (s.Hybrid() && s.fortniteFound)
        tweak("fn_gpu", Critical, "Fortnite на дискретній відеокарті",
              "У тебе дві відеокарти (вбудована і дискретна). Без цього Windows може запустити гру на слабкій "
              "вбудованій — FPS падає в рази.");
    else if (s.fortniteFound)
        tweak("fn_gpu", Optional, "Fortnite на потужній відеокарті", "Гарантує, що гра завжди йде на основній відеокарті.");
    if (s.HasNvidia())
        add(Recommended, "Увімкни NVIDIA Reflex у грі",
            "Fortnite → Налаштування → Відео → «NVIDIA Reflex» → «Увімк. + прискорення». Мінус 10–30 мс затримки вводу. "
            "Програма змінити це не може — тільки в грі.");
    else if (s.HasAmdDedicated())
        add(Recommended, "Увімкни AMD Anti-Lag",
            "AMD Software → Ігри → Fortnite → Radeon Anti-Lag. Зменшує затримку вводу.");
    if (s.build >= 22621) tweak("winopt", Recommended, "Оптимізації для віконних ігор",
                                "Windows 11: менша затримка, якщо граєш у «вікні без рамки».");
    if (s.HasDiscrete() && !weakGpu && s.build >= 19041)
        tweak("hags", Optional, "Апаратне планування GPU",
              "Сучасна відеокарта — зазвичай трохи знижує затримку. Якщо з'являться фризи, вимкни.");
    else if (!s.gpus.empty() && Available("hags") && !On("hags"))
        add(NotForYou, "Апаратне планування GPU",
            "Для вбудованої або старої відеокарти користі майже немає, а ризик фризів є.");
    tweak("gamedvr", Recommended, "Вимкни фоновий запис ігрової панелі",
          "Запис у фоні з'їдає ресурси відеокарти і диска.");

    // --- Fortnite: налаштування гри ---
    if (s.fnSettingsFound) {
        bool needMax = (!s.fnPerf && (weakGpu || weakCpu)) || s.fnVsync || !s.fnFullscreen;
        std::string what;
        if (!s.fnPerf && (weakGpu || weakCpu)) what += "режим продуктивності (твоєму ПК він дасть найбільший приріст FPS); ";
        if (s.fnVsync) what += "VSync вимкнути (додає затримку вводу); ";
        if (!s.fnFullscreen) what += "повноекранний режим (менша затримка, ніж у вікні без рамки); ";
        if (needMax)
            add(Critical, "Налаштування Fortnite для макс. FPS", "Рекомендую: " + what + "Гра має бути закрита.",
                FortniteMaxFps);
        else
            add(Good, "Налаштування Fortnite", "VSync вимкнено, повноекранний режим, продуктивність — усе як треба.");
        if (s.fnFps && s.refreshMax && s.fnFps < (int)s.refreshMax) {
            snprintf(b, sizeof(b), "У грі стоїть обмеження %d FPS, а монітор %u Гц. Підніми ліміт до %u або вище.",
                     s.fnFps, s.refreshMax, s.refreshMax);
            add(Recommended, "Обмеження FPS нижче частоти монітора", b, OpenPage, "", "fortnite");
        }
    } else if (s.fortniteFound) {
        add(Optional, "Налаштування Fortnite", "Запусти Fortnite хоча б раз — тоді порадник перевірить налаштування гри.");
    }
    if (s.fortniteFound)
        tweak("fn_priority", Recommended, "Високий пріоритет CPU для Fortnite",
              "Fortnite отримує процесор першим — менше просідань FPS через фонові програми.");
    if (weakCpu)
        tweak("sysresp", Recommended, "Мінімум ресурсів для фону",
              "На процесорі з 4 ядрами і менше кожен відсоток CPU для гри помітний.");
    else
        tweak("sysresp", Optional, "Мінімум ресурсів для фону", "Невеликий плюс для гри.");
    tweak("mmcss", Optional, "Пріоритет MMCSS для ігор", "Менше ризику фризів звуку під навантаженням.");

    // --- Пам'ять ---
    if (s.ramSpeed && s.ramRatedSpeed && s.ramSpeed + 100 < s.ramRatedSpeed) {
        snprintf(b, sizeof(b),
                 "Пам'ять працює на %d МТ/с, а модулі розраховані на %d. Увімкни XMP (Intel) або EXPO (AMD) у BIOS — "
                 "це +5–15%% FPS у Fortnite без жодного ризику.",
                 s.ramSpeed, s.ramRatedSpeed);
        add(Critical, "Пам'ять працює повільніше, ніж може", b);
    } else if ((s.ramType == 4 && s.ramSpeed && s.ramSpeed <= 2666) || (s.ramType == 5 && s.ramSpeed && s.ramSpeed <= 4800)) {
        snprintf(b, sizeof(b),
                 "Пам'ять DDR%d працює на %d МТ/с — це базова частота. Якщо твої модулі швидші (дивись на наклейці), "
                 "увімкни XMP/EXPO у BIOS: +5–15%% FPS.",
                 s.ramType, s.ramSpeed);
        add(Recommended, "Перевір XMP / EXPO у BIOS", b);
    }
    if (s.ramModules == 1 && !laptop)
        add(Recommended, "Одна планка пам'яті",
            "З одним модулем пам'ять працює в одноканальному режимі — FPS у Fortnite може бути на 10–20% нижчим. "
            "Дві однакові планки дають двоканальний режим.");
    if (s.ramMb && s.ramMb < 12 * 1024)
        add(Recommended, "Мало оперативної пам'яті",
            "Менше 12 ГБ: увімкни автоочистку ОЗУ «коли вільної пам'яті менше 20%» і закривай браузер перед грою.",
            OpenPage, "", "ram");
    else if (s.ramMb)
        add(Good, "Обсяг пам'яті", "Пам'яті достатньо — часта очистка ОЗУ не потрібна.");

    // --- Диск ---
    if (s.diskKnown && !s.systemSsd) {
        add(Recommended, "Системний диск — HDD",
            "Fortnite на HDD довго вантажиться і може фризити під час підвантаження карти. SSD — найкраще оновлення.");
        if (Available("sysmain") && !On("sysmain"))
            add(NotForYou, "Вимкнення SysMain", "На HDD SysMain пришвидшує запуск програм — не вимикай.");
    }
    if (s.freeDiskGb && s.freeDiskGb < 20) {
        snprintf(b, sizeof(b), "На системному диску лише %llu ГБ вільно. Звільни хоча б 20–30 ГБ — Windows і гра "
                               "потребують місця для кешу та оновлень.", s.freeDiskGb);
        add(Recommended, "Мало місця на диску", b);
    }

    // Сортування за важливістю
    std::stable_sort(v.begin(), v.end(), [](const Advice& a, const Advice& b) { return a.level < b.level; });
    return v;
}

}  // namespace adv
