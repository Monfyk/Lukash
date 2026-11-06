// KairoBoost — вибір найшвидшого DNS-сервера
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace dns {

struct Provider {
    const char* name;
    const char* note;
    const char* v4[2];
    const char* v6[2];  // nullptr, якщо немає
};
const std::vector<Provider>& Providers();

// Час відповіді на справжній DNS-запит (UDP/53), мс; < 0 — немає відповіді
double QueryMs(const std::string& server, const char* domain, int timeoutMs = 1500, bool strict = true);

// true, якщо DNS-запити перехоплюються (провайдер, роутер, антивірус): відповідь приходить навіть
// з адреси, де DNS-сервера бути не може. Тоді вибір DNS у Windows нічого не змінить.
bool InterceptionDetected();

struct Result {
    double avg = -1, min = -1;
    int ok = 0, total = 0;
    double Loss() const { return total ? 100.0 * (total - ok) / total : 0; }
};
// Кілька запитів до різних популярних доменів
Result Measure(const std::string& server, int count, const std::atomic<bool>* cancel = nullptr,
               const std::function<void(double)>& sample = nullptr);

struct Adapter {
    unsigned index = 0;
    std::string guid, name;
    std::vector<std::string> dns4, dns6;
};
// Активні мережеві адаптери з виходом в інтернет
std::vector<Adapter> ActiveAdapters();
std::vector<std::string> CurrentServers();  // IPv4 DNS активних адаптерів
int CurrentProvider();                      // індекс у Providers() або -1

// Встановлює DNS на всіх активних адаптерах (оригінальні налаштування зберігаються)
bool Apply(const Provider& p);
// Повертає налаштування DNS, що були до KairoBoost (або автоматичні від провайдера)
bool Restore();
bool IsChanged();

}  // namespace dns
