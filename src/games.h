// KairoBoost — підтримувані ігри та поточна обрана гра.
// У звичайній редакції гра завжди Fortnite; у редакції для продажу її обирає користувач.
#pragma once

#include <string>
#include <vector>

namespace game {

struct Profile {
    const char* id;
    const char* name;
    std::vector<std::string> exes;  // імена процесів (кілька — для різних версій гри)
    const char* tip;                // що налаштувати в самій грі
    unsigned steamAppId = 0;        // для автопошуку в Steam
    const char* steamRel = nullptr; // шлях до exe всередині папки гри Steam
    const char* fixedRel = nullptr; // типовий шлях від кореня диска / Program Files (Riot, Battle.net)
};

const std::vector<Profile>& Profiles();
const Profile& Current();
bool Select(const std::string& id);  // false — невідомий id
bool IsFortnite();
// Назва для показу: для «Своєї гри» — ім'я її exe
std::string DisplayName();

// Своя гра: exe задає користувач
void SetCustomExe(const std::string& exeName);

// Шлях до exe поточної гри: вручну вказаний, знайдений автоматично або шлях запущеного процесу
std::string CurrentPath();
void SetManualPath(const std::string& path);  // для поточної гри
std::string ManualPath();

std::vector<std::string> CurrentExes();
std::string ExeList();  // "a.exe;b.exe" — для режиму гри
bool IsRunning();

// Процеси античитів — їх пам'ять і процеси ніколи не чіпаємо
const std::vector<std::string>& AntiCheatProcesses();

}  // namespace game
