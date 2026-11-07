"""
Справжні ціни й рейтинги через SerpApi (serpapi.com, США):
  • Google Flights — реальні ціни квитків туди-назад, авіакомпанії, пересадки,
    оцінка Google «низька / звичайна / висока ціна» та історія ціни;
  • Google Hotels — реальні ціни готелів і апартаментів (з Booking, Expedia, Agoda…),
    рейтинг гостей (1–5) і кількість відгуків.

Безкоштовний план SerpApi — 250 запитів на місяць. Результати кешуються на диску на 6 год.
"""
import json
import os
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from datetime import date, timedelta

API = os.environ.get("SERPAPI_SEARCH_URL", "https://serpapi.com/search.json")
ACCOUNT = os.environ.get("SERPAPI_ACCOUNT_URL", "https://serpapi.com/account.json")
CACHE_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), ".cache", "serpapi.json")
CACHE_TTL = 6 * 3600
MIN_REVIEWS = 30  # щоб «5.0 з 3 відгуків» не вважалось хорошим місцем
_lock = threading.Lock()
_cache = None

WEEKDAY_FLIGHT = [0.95, 0.82, 0.8, 0.95, 1.2, 1.05, 1.15]  # пн..нд — типова дешевизна днів вильоту
WEEKDAY_HOTEL = [0.9, 0.88, 0.88, 0.95, 1.2, 1.25, 0.95]


class SerpError(Exception):
    pass


# ───────────────────────── кеш і HTTP ─────────────────────────
def _load_cache():
    global _cache
    if _cache is None:
        try:
            with open(CACHE_FILE, encoding="utf-8") as f:
                _cache = json.load(f)
        except Exception:  # noqa
            _cache = {}
    return _cache


def _save_cache():
    os.makedirs(os.path.dirname(CACHE_FILE), exist_ok=True)
    tmp = CACHE_FILE + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(_cache, f, ensure_ascii=False)
    os.replace(tmp, CACHE_FILE)


def _get(url, timeout=60):
    req = urllib.request.Request(url, headers={"User-Agent": "travel-finder/2.0"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read().decode())
    except urllib.error.HTTPError as e:
        try:
            body = json.loads(e.read().decode())
            raise SerpError(body.get("error") or str(e))
        except (ValueError, AttributeError):
            raise SerpError(str(e))


def serp(params: dict, key: str) -> dict:
    """Запит до SerpApi з кешем. Порожній результат — це не помилка."""
    ck = json.dumps(params, sort_keys=True)
    with _lock:
        hit = _load_cache().get(ck)
        if hit and time.time() - hit["t"] < CACHE_TTL:
            return hit["data"]
    data = _get(API + "?" + urllib.parse.urlencode({**params, "api_key": key}))
    err = data.get("error")
    if err and "hasn't returned any results" not in err:
        raise SerpError(err)
    with _lock:
        _cache[ck] = {"t": time.time(), "data": data}
        _save_cache()
    return data


def account(key: str):
    """Скільки запитів лишилось (цей запит квоту не витрачає)."""
    d = _get(ACCOUNT + "?" + urllib.parse.urlencode({"api_key": key}), timeout=15)
    if d.get("error"):
        raise SerpError(d["error"])
    return {"left": d.get("plan_searches_left", d.get("total_searches_left")),
            "per_month": d.get("searches_per_month"), "plan": d.get("plan_name")}


# ───────────────────────── вибір дат для перевірки ─────────────────────────
def candidate_pairs(start: date, end: date, nmin: int, nmax: int, k: int):
    """Діапазон дат ділимо на k відрізків; у кожному беремо комбінацію, яка зазвичай найдешевша
    (виліт/повернення у вт-ср, менше ночей на вихідних). Ціни потім перевіряємо по-справжньому."""
    days = (end - start).days + 1
    k = max(1, min(k, days))
    out = []
    for w in range(k):
        a = start + timedelta(days=w * days // k)
        b = start + timedelta(days=max((w + 1) * days // k - 1, w * days // k))
        best, bs = None, 1e9
        d = a
        while d <= b:
            for n in range(nmin, nmax + 1):
                r = d + timedelta(days=n)
                hotel = sum(WEEKDAY_HOTEL[(d + timedelta(days=i)).weekday()] for i in range(n)) / n
                s = WEEKDAY_FLIGHT[d.weekday()] + WEEKDAY_FLIGHT[r.weekday()] + hotel + 0.08 * (n - nmin)
                if s < bs:
                    best, bs = (d, n), s
            d += timedelta(days=1)
        out.append(best)
    return out


# ───────────────────────── розбір відповідей ─────────────────────────
def _mins(m):
    if not m:
        return None
    return f"{m // 60} год {m % 60:02d} хв" if m >= 60 else f"{m} хв"


def parse_flights(data: dict):
    items = (data.get("best_flights") or []) + (data.get("other_flights") or [])
    offers = []
    for it in items:
        price = it.get("price")
        legs = it.get("flights") or []
        if not isinstance(price, (int, float)) or not legs:
            continue
        first, last = legs[0], legs[-1]
        airlines = []
        for l in legs:
            if l.get("airline") and l["airline"] not in airlines:
                airlines.append(l["airline"])
        offers.append({
            "price": price,
            "airlines": airlines,
            "logo": it.get("airline_logo") or first.get("airline_logo"),
            "from": (first.get("departure_airport") or {}).get("id"),
            "to": (last.get("arrival_airport") or {}).get("id"),
            "depart": (first.get("departure_airport") or {}).get("time"),
            "arrive": (last.get("arrival_airport") or {}).get("time"),
            "stops": len(legs) - 1,
            "duration": _mins(it.get("total_duration")),
            "flight_numbers": [l.get("flight_number") for l in legs if l.get("flight_number")],
        })
    offers.sort(key=lambda o: o["price"])
    pi = data.get("price_insights") or {}
    return {
        "offers": offers[:3],
        "insights": {
            "lowest": pi.get("lowest_price"),
            "level": pi.get("price_level"),  # low / typical / high
            "typical": pi.get("typical_price_range"),
            "history": pi.get("price_history") or [],
        },
        "url": (data.get("search_metadata") or {}).get("google_flights_url"),
    }


def parse_hotels(data: dict, nights: int, min_rating5: float, no_hostels: bool):
    out = []
    for p in data.get("properties") or []:
        rating, reviews = p.get("overall_rating"), p.get("reviews") or 0
        if not isinstance(rating, (int, float)) or rating < min_rating5 or reviews < MIN_REVIEWS:
            continue
        name = p.get("name") or ""
        if no_hostels and "hostel" in name.lower():
            continue
        total = (p.get("total_rate") or {}).get("extracted_lowest")
        per_night = (p.get("rate_per_night") or {}).get("extracted_lowest")
        if not isinstance(total, (int, float)):
            if not isinstance(per_night, (int, float)):
                continue
            total = per_night * nights
        sources = sorted(
            [{"source": s.get("source"), "price": (s.get("rate_per_night") or {}).get("extracted_lowest"), "link": s.get("link")}
             for s in (p.get("prices") or []) if s.get("source")],
            key=lambda s: s["price"] if isinstance(s["price"], (int, float)) else 1e9)
        imgs = p.get("images") or []
        out.append({
            "name": name,
            "kind": "apartment" if "rental" in (p.get("type") or "").lower() else "hotel",
            "rating": rating,
            "rating10": round(rating * 2, 1),
            "reviews": reviews,
            "stars": p.get("extracted_hotel_class"),
            "price": round(total),
            "per_night": round(per_night) if isinstance(per_night, (int, float)) else round(total / nights),
            "link": p.get("link"),
            "photo": (imgs[0].get("thumbnail") or imgs[0].get("original_image")) if imgs else None,
            "location_rating": p.get("location_rating"),
            "sources": sources[:3],
        })
    out.sort(key=lambda h: h["price"])
    return out


# ───────────────────────── пошук по одній комбінації дат ─────────────────────────
def check_pair(o, d, dep: date, nights: int, adults: int, min_rating10: float, no_hostels: bool, stay_kind: str, key: str,
               skip_flights: bool = False):
    ret = dep + timedelta(days=nights)
    errors = []
    flights = None
    try:
        if skip_flights:
            raise LookupError
        fl = serp({
            "engine": "google_flights", "departure_id": o["airports"], "arrival_id": d["airports"],
            "outbound_date": dep.isoformat(), "return_date": ret.isoformat(), "type": "1",
            "adults": adults, "currency": "EUR", "hl": "en", "gl": "at", "sort_by": "2",
        }, key)
        flights = parse_flights(fl)
    except LookupError:
        pass
    except Exception as e:  # noqa
        errors.append(f"Google Flights: {e}")

    min5 = min_rating10 / 2
    api_rating = "9" if min5 >= 4.5 else ("8" if min5 >= 4.0 else "7")
    stays = []
    kinds = ["hotel", "apartment"] if stay_kind == "both" else [stay_kind]
    for kind in kinds:
        params = {
            "engine": "google_hotels", "q": f"{d['en']}", "check_in_date": dep.isoformat(),
            "check_out_date": ret.isoformat(), "adults": adults, "currency": "EUR",
            "hl": "en", "gl": "at", "sort_by": "3", "rating": api_rating,
        }
        if kind == "apartment":
            params["vacation_rentals"] = "true"
            params.pop("sort_by")
        try:
            stays += [{**h, "kind": kind} for h in parse_hotels(serp(params, key), nights, min5, no_hostels)]
        except Exception as e:  # noqa
            errors.append(f"Google Hotels: {e}")
    stays.sort(key=lambda h: h["price"])

    gh = "https://www.google.com/travel/search?" + urllib.parse.urlencode({
        "q": f"hotels in {d['en']}", "checkin": dep.isoformat(), "checkout": ret.isoformat(), "adults": adults})
    flight = flights["offers"][0] if flights and flights["offers"] else None
    stay = stays[0] if stays else None
    deal = None
    if flight and flights["insights"].get("typical"):
        lo = flights["insights"]["typical"][0]
        if flight["price"] < lo * 0.7:
            deal = "absurd"
        elif flights["insights"].get("level") == "low" or flight["price"] < lo:
            deal = "great"
    return {
        "dep": dep.isoformat(), "ret": ret.isoformat(), "nights": nights,
        "flight": flight, "flight_alts": flights["offers"][1:] if flights else [],
        "insights": flights["insights"] if flights else None,
        "flights_url": (flights or {}).get("url"),
        "stay": stay, "stay_alts": stays[1:4], "hotels_url": gh,
        "ground_only": skip_flights,
        "total": (stay["price"] + (flight["price"] if flight else 0)) if (stay and (flight or skip_flights)) else None,
        "deal": deal, "errors": errors,
    }


def real_search(o, d, start, end, nmin, nmax, adults, min_rating10, no_hostels, stay_kind, checks, key, skip_flights=False):
    pairs = candidate_pairs(start, end, nmin, nmax, checks)
    with ThreadPoolExecutor(max_workers=4) as ex:
        res = list(ex.map(lambda p: check_pair(o, d, p[0], p[1], adults, min_rating10, no_hostels, stay_kind, key, skip_flights), pairs))
    return res
