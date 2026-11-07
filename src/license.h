// KairoBoost — ліцензії (лише редакція для продажу).
// Ключ = дані (рівень, номер) + цифровий підпис ECDSA P-256. У програмі лише публічний ключ,
// тому створити дійсний ключ без секретного ключа продавця неможливо. Перевірка — без інтернету.
#pragma once

#include <string>

namespace lic {

enum Tier { None = 0, Standard = 1, Pro = 2 };

struct Status {
    Tier tier = None;      // що відкрито зараз (з урахуванням пробного періоду)
    Tier keyTier = None;   // рівень активованого ключа
    bool trial = false;    // діє пробний період Pro
    int trialDaysLeft = 0;
    unsigned serial = 0;
};

const int kTrialDays = 7;

Status Get();                  // кешується, оновлюється після Activate/Deactivate
bool IsPro();                  // Pro-функції доступні (ключ Pro або пробний період)
bool HasAccess();              // програмою взагалі можна користуватися
const char* TierName(Tier t);  // "Standard" / "Pro"

// false + текст помилки, якщо ключ недійсний
bool Activate(const std::string& key, std::string& error);
void Deactivate();

bool Verify(const std::string& key, Tier& tier, unsigned& serial);

}  // namespace lic
