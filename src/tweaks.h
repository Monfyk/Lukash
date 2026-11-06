// KairoBoost — каталог твіків. Кожен твік можна окремо ввімкнути і вимкнути;
// вимкнення повертає саме ті значення, що були до ввімкнення.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "core.h"

namespace tw {

enum Category { Network, System, Cpu, Gpu, Input, Disk, Fortnite, CategoryCount };
const char* CategoryName(Category c);

struct Tweak {
    const char* id;
    Category category;
    const char* name;
    const char* desc;
    const char* caution;  // "Не варто вмикати, якщо..."
    bool reboot;          // потрібне перезавантаження
    bool recommended;     // входить у "Застосувати рекомендовані"

    // Реєстрові твіки: список значень (може залежати від ПК, тому функція)
    std::function<std::vector<lb::RegValue>()> regs;
    // Нестандартні твіки (служби, живлення...) або додаткові дії
    std::function<bool()> isOn;
    std::function<bool()> apply;
    std::function<bool()> revert;
    std::function<void()> after;  // після apply/revert (напр. застосувати налаштування миші одразу)
    std::function<std::string()> unavailable;  // непорожній рядок = твік недоступний (причина)

    bool gameRestart = false;  // діє з наступного запуску гри
};
// Коли твік починає діяти: "одразу", "після перезапуску гри" або "після перезавантаження"
const char* EffectText(const Tweak& t);

const std::vector<Tweak>& All();
const Tweak* Find(const std::string& id);
bool IsOn(const Tweak& t);
bool Apply(const Tweak& t);
bool Revert(const Tweak& t);
std::string Unavailable(const Tweak& t);

void ApplyRecommended(bool includeFortnite);

// Повтор клавіш (твік "keyboard", через FilterKeys): затримка перед повтором і інтервал між повторами, мс
void SetKeyRepeat(int delayMs, int intervalMs);
void GetKeyRepeat(int& delayMs, int& intervalMs);
// Поточні значення з системи, якщо твік увімкнено (false — вимкнено)
bool ReadSystemKeyRepeat(int& delayMs, int& intervalMs);
void RevertEverything();

}  // namespace tw
