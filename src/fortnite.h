// KairoBoost — усе, що стосується Fortnite
#pragma once

#include <string>
#include <vector>

namespace fn {

extern const char* kExe;  // FortniteClient-Win64-Shipping.exe

// Шлях до exe: ручний (якщо задано) або знайдений через Epic Games Launcher.
std::string FindInstallDir();  // "" якщо не знайдено
std::string ExePath();
void SetManualDir(const std::string& dir);
std::string ManualDir();

bool IsRunning();

// GameUserSettings.ini
std::string SettingsPath();
struct Settings {
    bool found = false;
    int fpsLimit = 0;  // 0 = без обмеження
    bool vsync = false;
    bool performanceMode = false;  // DirectX 11 "Продуктивність" (es31)
    bool fullscreen = false;       // справжній повноекранний режим (FullscreenMode=0)
    bool lowGraphics = false;      // тіні, ефекти, постобробка тощо на мінімумі
    std::string rhi;               // dx11 / dx12 / ...
};
Settings ReadSettings();
// Перед першою зміною робить копію GameUserSettings.ini.kairo-backup
bool WriteSettings(int fpsLimit, bool vsync, bool performanceMode, bool fullscreen, bool lowGraphics);
bool HasSettingsBackup();
bool RestoreSettingsBackup();

int CleanLogs();  // видаляє логи та звіти про збої Fortnite; повертає кількість файлів

struct Region {
    const char* code;
    const char* name;
    const char* host;
};
const std::vector<Region>& Regions();

}  // namespace fn
