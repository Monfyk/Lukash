// Дві редакції з одного коду:
//  - звичайна (KairoBoost.exe) — особиста версія, налаштована під Fortnite;
//  - для продажу (KairoBoostPro.exe, збирається з -DKB_SALE) — під будь-яку гру.
#pragma once

#ifdef KB_SALE
#define KB_ED(personal, sale) sale
#define KB_IS_SALE 1
#else
#define KB_ED(personal, sale) personal
#define KB_IS_SALE 0
#endif

#define KB_VERSION "1.0.0"
