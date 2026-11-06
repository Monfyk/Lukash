// KairoBoost — порадник: аналізує ПК і пояснює, які твіки вмикати, а які ні.
// Працює локально за правилами, без інтернету.
#pragma once

#include <string>
#include <vector>

namespace adv {

struct GpuInfo {
    std::string name;
    unsigned vendor = 0;          // 0x10DE NVIDIA, 0x1002 AMD, 0x8086 Intel
    unsigned long long vramMb = 0;
    bool integrated = false;
};

struct SystemInfo {
    std::string cpu;
    int physicalCores = 0, logicalCores = 0;
    std::vector<GpuInfo> gpus;
    unsigned long long ramMb = 0;
    int ramSpeed = 0, ramRatedSpeed = 0;  // МТ/с: поточна і заявлена модулем
    int ramType = 0;                      // 4 = DDR4, 5 = DDR5, 0 = невідомо
    int ramModules = 0;
    bool hasBattery = false, onBattery = false;
    bool systemSsd = true, diskKnown = false;
    unsigned long long freeDiskGb = 0;
    unsigned refreshCur = 0, refreshMax = 0;
    bool onWifi = false;
    unsigned build = 0;
    bool fortniteFound = false;
    bool fnSettingsFound = false, fnPerf = false, fnVsync = false, fnFullscreen = false;
    int fnFps = 0;

    bool HasNvidia() const;
    bool HasAmdDedicated() const;
    bool HasDiscrete() const;
    bool Hybrid() const;  // вбудована + дискретна (ноутбуки)
    std::string Summary() const;  // текстовий звіт
};

SystemInfo Collect();

enum Level { Critical, Recommended, Optional, Good, NotForYou };

enum ActionKind { None, EnableTweak, DisableTweak, FortniteMaxFps, OpenPage };

struct Advice {
    Level level;
    std::string title, text;
    ActionKind action = None;
    std::string tweakId;  // для EnableTweak / DisableTweak
    std::string page;     // для OpenPage: "fortnite", "ram", "dns", "net", "timer"
};

std::vector<Advice> Analyze(const SystemInfo& s);
const char* LevelName(Level l);

}  // namespace adv
