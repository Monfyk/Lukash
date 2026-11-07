#!/usr/bin/env python3
"""
Мандрівний Шукач — знаходить найвигідніші дати туди-назад, транспорт і житло.

Запуск:  python3 server.py         → відкрий http://localhost:8000
СПРАВЖНІ ціни й рейтинги: Google Flights + Google Hotels через SerpApi (serpapi.com, 250 безкоштовних запитів/міс).
Ключ: змінна SERPAPI_KEY, файл serpapi_key.txt поруч із server.py, або поле «Ключ» на сторінці.
Без ключа працює лише ДЕМО-режим із вигаданими цінами (позначений на сторінці).
Без російських сервісів.

Лише стандартна бібліотека Python 3.8+, нічого встановлювати не треба.
"""
import hashlib
import json
import math
import os
import statistics
import urllib.parse
from datetime import date, datetime, timedelta
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

from cities import BY_CODE, find_city
import real

PORT = int(os.environ.get("PORT", "8000"))
HERE = os.path.dirname(os.path.abspath(__file__))
STATIC = os.path.join(HERE, "static")


def server_key() -> str:
    k = os.environ.get("SERPAPI_KEY", "").strip()
    if not k:
        try:
            with open(os.path.join(HERE, "serpapi_key.txt"), encoding="utf-8") as f:
                k = f.read().strip()
        except OSError:
            pass
    return k


# ───────────────────────── допоміжне ─────────────────────────
def rnd(*parts) -> float:
    """Детермінований «випадковий» шум 0..1 (однакові запити → однакові ціни)."""
    h = hashlib.sha256("|".join(map(str, parts)).encode()).hexdigest()
    return int(h[:12], 16) / 0xFFFFFFFFFFFF


def km(a, b) -> float:
    r = 6371
    p1, p2 = math.radians(a["lat"]), math.radians(b["lat"])
    dp, dl = p2 - p1, math.radians(b["lon"] - a["lon"])
    x = math.sin(dp / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 2 * r * math.asin(math.sqrt(x))


def season(d: date) -> float:
    m = d.month
    if m in (7, 8):
        return 1.45
    if m in (6, 12):
        return 1.2 if not (m == 12 and d.day < 15) else 0.95
    if m in (1, 2, 11):
        return 0.8
    return 1.0


# ───────────────────────── модель цін (режим оцінки) ─────────────────────────
WEEKDAY_FLIGHT = [0.95, 0.82, 0.8, 0.95, 1.2, 1.05, 1.15]  # пн..нд
WEEKDAY_HOTEL = [0.9, 0.88, 0.88, 0.95, 1.2, 1.25, 0.95]


def est_flight_oneway(o, d, day: date) -> dict:
    dist = km(o, d)
    base = 15 + 0.055 * dist if dist < 3500 else 120 + 0.045 * dist
    if o["lowcost"] and d["lowcost"] and dist < 4000:
        base *= 0.7
    lead = (day - date.today()).days
    lead_f = 1.35 if lead < 10 else (1.1 if lead < 25 else (0.9 if lead < 120 else 1.0))
    noise = 0.65 + 0.8 * rnd("f", o["code"], d["code"], day)
    price = base * WEEKDAY_FLIGHT[day.weekday()] * season(day) * lead_f * noise
    # «помилкові тарифи» / флеш-розпродажі — ті самі «смішно дешеві» квитки
    if rnd("deal", o["code"], d["code"], day) < 0.05:
        price *= 0.25
    transfers = 0 if (o["lowcost"] and d["lowcost"] and dist < 3000) else (1 if dist < 7000 else 1 + int(rnd("t", day) > 0.6))
    return {"price": max(round(price), 9), "transfers": transfers}


def est_ground_oneway(o, d, day: date):
    dist = km(o, d) * 1.25  # дорога довша за пряму
    if dist > 1600:
        return None
    bus = (5 + 0.045 * dist) * (0.6 + 0.7 * rnd("b", o["code"], d["code"], day))
    train = (12 + 0.075 * dist) * (0.6 + 0.8 * rnd("tr", o["code"], d["code"], day))
    if rnd("bdeal", o["code"], d["code"], day) < 0.06:
        bus *= 0.35  # акційні квитки FlixBus/RegioJet за 4.99€
    hours_bus = dist / 65
    hours_train = dist / 90
    if bus <= train:
        return {"type": "bus", "price": max(round(bus), 5), "hours": round(hours_bus, 1)}
    return {"type": "train", "price": max(round(train), 9), "hours": round(hours_train, 1)}


def quality_factor(rating: float) -> float:
    """Чим вищий рейтинг гостей, тим дорожче: 6.5 → ×0.6, 8.0 → ×1.0, 9.5 → ×1.65."""
    return 0.6 + 0.27 * max(0.0, rating - 6.5) + 0.25 * max(0.0, rating - 8.5) ** 2


def est_stay(d, checkin: date, nights: int, adults: int, min_rating: float = 8.0, no_hostels: bool = True):
    """Найдешевший варіант кожного типу житла з рейтингом не нижче min_rating."""
    total = 0.0
    for i in range(nights):
        day = checkin + timedelta(days=i)
        n = 0.75 + 0.5 * rnd("h", d["code"], day)
        total += d["hotel"] * WEEKDAY_HOTEL[day.weekday()] * season(day) * n
    rooms = max(1, math.ceil(adults / 2))
    base = {
        "hostel": total * 0.45 * adults,
        "apartment": total * 0.9 * rooms * (1 if adults <= 2 else 0.85),
        "hotel": total * 1.15 * rooms,
    }
    opts = []
    for typ, b in base.items():
        if no_hostels and typ == "hostel":
            continue
        best = None
        # кілька «пропозицій» з різним рейтингом гостей, як у пошуку на Booking
        for k in range(10):
            r = round(6.0 + 3.7 * rnd("r", d["code"], typ, k), 1)
            if r < min_rating:
                continue
            p = b * quality_factor(r) * (0.8 + 0.4 * rnd("p", d["code"], typ, k, checkin))
            if rnd("hdeal", d["code"], typ, k, checkin, nights) < 0.03:
                p *= 0.5  # рідкісна знижка в хорошому місці
            if best is None or p < best["price"]:
                best = {"type": typ, "price": round(p), "rating": r}
        if best is None:  # навіть найсуворіший фільтр: беремо топове місце
            r = max(min_rating, 9.5)
            best = {"type": typ, "price": round(b * quality_factor(r)), "rating": round(r, 1)}
        opts.append(best)
    return sorted(opts, key=lambda x: x["price"])


# ───────────────────────── посилання на сайти ─────────────────────────
def links_flight(o, d, dep: date, ret: date, adults: int):
    a = adults
    return [
        {"site": "Skyscanner", "url": f"https://www.skyscanner.net/transport/flights/{o['code'].lower()}/{d['code'].lower()}/{dep:%y%m%d}/{ret:%y%m%d}/?adultsv2={a}&rtn=1"},
        {"site": "Google Flights", "url": "https://www.google.com/travel/flights?q=" + urllib.parse.quote(f"Flights from {o['code']} to {d['code']} on {dep} through {ret} {a} adults")},
        {"site": "Kiwi.com", "url": f"https://www.kiwi.com/deep?from={o['code']}&to={d['code']}&departure={dep:%d-%m-%Y}&return={ret:%d-%m-%Y}&adults={a}"},
        {"site": "Kayak", "url": f"https://www.kayak.com/flights/{o['code']}-{d['code']}/{dep}/{ret}/{a}adults?sort=price_a"},
        {"site": "Momondo", "url": f"https://www.momondo.com/flight-search/{o['code']}-{d['code']}/{dep}/{ret}/{a}adults?sort=price_a"},
    ]


def links_ground(o, d, dep: date, adults: int):
    on, dn = urllib.parse.quote(o["en"]), urllib.parse.quote(d["en"])
    return [
        {"site": "Omio", "url": f"https://www.omio.com/search?departure={on}&arrival={dn}&departureDate={dep}&passengers={adults}"},
        {"site": "FlixBus", "url": f"https://www.flixbus.com/search?departureCity={on}&arrivalCity={dn}&rideDate={dep:%d.%m.%Y}&adult={adults}"},
        {"site": "RegioJet", "url": f"https://regiojet.com/?fromLocationName={on}&toLocationName={dn}&departureDate={dep}"},
        {"site": "ÖBB", "url": "https://shop.oebbtickets.at/en/ticket"},
        {"site": "BlaBlaCar", "url": f"https://www.blablacar.com/search?fn={on}&tn={dn}&db={dep}&seats={adults}"},
        {"site": "Rome2Rio", "url": f"https://www.rome2rio.com/map/{on}/{dn}"},
    ]


def links_stay(d, cin: date, cout: date, adults: int, min_rating: float = 8.0, no_hostels: bool = True):
    c = urllib.parse.quote(d["en"])
    n = (cout - cin).days
    # Booking: фільтр «оцінка гостей» (60/70/80/90 = 6+/7+/8+/9+) — вже вбудований у посилання
    score = int(min_rating) * 10
    nflt = f"review_score%3D{score}" if score >= 60 else ""
    links = [
        {"site": f"Booking.com ★{min_rating:g}+", "url": f"https://www.booking.com/searchresults.html?ss={c}&checkin={cin}&checkout={cout}&group_adults={adults}&no_rooms=1&order=price" + (f"&nflt={nflt}" if nflt else "")},
        {"site": "Airbnb" + (" (Guest favourite)" if min_rating >= 8.5 else ""), "url": f"https://www.airbnb.com/s/{c}/homes?checkin={cin}&checkout={cout}&adults={adults}" + ("&guest_favorite=true" if min_rating >= 8.5 else "") + ("&room_types%5B%5D=Entire%20home%2Fapt" if no_hostels else "")},
        {"site": "Agoda", "url": f"https://www.agoda.com/search?textToSearch={c}&checkIn={cin}&los={n}&adults={adults}&rooms=1&sort=priceLowToHigh"},
        {"site": "Hotels.com", "url": f"https://www.hotels.com/Hotel-Search?destination={c}&startDate={cin}&endDate={cout}&adults={adults}&sort=PRICE_LOW_TO_HIGH"},
        {"site": "Trivago", "url": f"https://www.trivago.com/en-US/srl?search={c}"},
    ]
    if not no_hostels:
        links.insert(2, {"site": "Hostelworld", "url": f"https://www.hostelworld.com/st/hostels/search/?search_keywords={c}&from={cin}&to={cout}&guests={adults}"})
    return links


# ───────────────────────── пошук ─────────────────────────
def search(o, d, start: date, end: date, nmin: int, nmax: int, adults: int, with_links=True,
           min_rating: float = 8.0, no_hostels: bool = True):
    cells = []
    day = start
    while day <= end:
        for n in range(nmin, nmax + 1):
            ret = day + timedelta(days=n)
            # переліт
            a, b = est_flight_oneway(o, d, day), est_flight_oneway(d, o, ret)
            fl = {"price": (a["price"] + b["price"]) * adults, "transfers": max(a["transfers"], b["transfers"]), "source": "est"}
            # автобус/потяг
            g1, g2 = est_ground_oneway(o, d, day), est_ground_oneway(d, o, ret)
            ground = None
            if g1 and g2:
                ground = {"type": g1["type"] if g1["price"] <= g2["price"] else g2["type"], "price": (g1["price"] + g2["price"]) * adults,
                          "hours": g1["hours"], "source": "est"}
            transport = {"mode": "flight", **fl}
            if ground and (ground["price"] < fl["price"] * 0.85 or km(o, d) < 350):
                transport = {"mode": ground["type"], **ground}
            stays = est_stay(d, day, n, adults, min_rating, no_hostels)
            total = transport["price"] + stays[0]["price"]
            cells.append({
                "dep": day.isoformat(), "ret": ret.isoformat(), "nights": n,
                "total": total, "per_person": round(total / adults),
                "transport": transport, "flight": fl, "ground": ground,
                "stay": stays[0], "stays": stays,
            })
        day += timedelta(days=1)

    if not cells:
        return {"error": "Немає дат у вибраному діапазоні"}
    totals = [c["total"] for c in cells]
    med = statistics.median(totals)
    for c in cells:
        c["ratio"] = round(c["total"] / med, 3)
        c["deal"] = "absurd" if c["ratio"] < 0.55 else ("great" if c["ratio"] < 0.75 else None)
    best = sorted(cells, key=lambda c: c["total"])[:12]
    if with_links:
        for c in best:
            dep, ret = date.fromisoformat(c["dep"]), date.fromisoformat(c["ret"])
            c["links"] = {
                "flight": links_flight(o, d, dep, ret, adults),
                "ground": links_ground(o, d, dep, adults) if c["ground"] else [],
                "ground_back": links_ground(d, o, ret, adults) if c["ground"] else [],
                "stay": links_stay(d, dep, ret, adults, min_rating, no_hostels),
            }
    return {
        "origin": o, "destination": d, "distance_km": round(km(o, d)),
        "adults": adults, "nights": [nmin, nmax], "min_rating": min_rating, "no_hostels": no_hostels, "range": [start.isoformat(), end.isoformat()],
        "median": round(med), "min": min(totals), "max": max(totals),
        "cells": [{k: c[k] for k in ("dep", "ret", "nights", "total", "deal", "ratio")} for c in cells],
        "best": best,
        "mode": "estimate",
    }


def search_anywhere(o, start, end, nmin, nmax, adults, min_rating=8.0, no_hostels=True):
    out = []
    for d in BY_CODE.values():
        if d["code"] == o["code"]:
            continue
        r = search(o, d, start, end, nmin, nmax, adults, False, min_rating, no_hostels)
        if "best" not in r:
            continue
        b = r["best"][0]
        out.append({"destination": d, "best": b, "median": r["median"], "mode": r["mode"],
                    "distance_km": r["distance_km"]})
    out.sort(key=lambda x: x["best"]["total"])
    return {"origin": o, "results": out[:30], "adults": adults, "min_rating": min_rating}



# ───────────────────────── справжні ціни (SerpApi) ─────────────────────────
def booking_hotel_link(name, d, cin, cout, adults):
    q = urllib.parse.quote(f"{name}, {d['en']}")
    return f"https://www.booking.com/searchresults.html?ss={q}&checkin={cin}&checkout={cout}&group_adults={adults}&no_rooms=1"


def search_real(o, d, start, end, nmin, nmax, adults, min_rating, no_hostels, stay_kind, checks, key):
    dist = km(o, d)
    res = real.real_search(o, d, start, end, nmin, nmax, adults, min_rating, no_hostels, stay_kind, checks, key,
                           skip_flights=dist < 400)
    for c in res:
        dep, ret = date.fromisoformat(c["dep"]), date.fromisoformat(c["ret"])
        fl = links_flight(o, d, dep, ret, adults)
        if c.get("flights_url"):
            fl.insert(0, {"site": "Google Flights (ця ціна)", "url": c["flights_url"]})
        c["links"] = {
            "flight": fl,
            "ground": links_ground(o, d, dep, adults) if dist < 1300 or c["ground_only"] else [],
            "ground_back": links_ground(d, o, ret, adults) if dist < 1300 or c["ground_only"] else [],
            "stay": links_stay(d, dep, ret, adults, min_rating, no_hostels),
        }
        for h in ([c["stay"]] if c["stay"] else []) + c["stay_alts"]:
            h["booking"] = booking_hotel_link(h["name"], d, dep, ret, adults)
    ok = [c for c in res if c["total"] is not None]
    ok.sort(key=lambda c: c["total"])
    return {
        "mode": "real", "origin": o, "destination": d, "distance_km": round(dist), "adults": adults,
        "min_rating": min_rating, "no_hostels": no_hostels, "stay_kind": stay_kind,
        "checked": sorted(res, key=lambda c: c["dep"]), "best": ok,
        "errors": sorted({e for c in res for e in c["errors"]}),
    }


def search_anywhere_real(o, start, end, nmin, nmax, adults, min_rating, no_hostels, stay_kind, n, key):
    """Модель лише відбирає n потенційно дешевих напрямків — ціни потім перевіряються по-справжньому."""
    pre = [r for r in search_anywhere(o, start, end, nmin, nmax, adults, min_rating, no_hostels)["results"]
           if r["distance_km"] >= 400][:n]

    def one(r):
        b = r["best"]
        return r["destination"], real.check_pair(o, r["destination"], date.fromisoformat(b["dep"]), b["nights"], adults,
                                                 min_rating, no_hostels, stay_kind, key)
    from concurrent.futures import ThreadPoolExecutor
    with ThreadPoolExecutor(max_workers=4) as ex:
        res = list(ex.map(one, pre))
    out = [{"destination": d, "best": c} for d, c in res if c["total"] is not None]
    out.sort(key=lambda x: x["best"]["total"])
    return {"mode": "real", "origin": o, "results": out, "adults": adults, "min_rating": min_rating,
            "errors": sorted({e for _, c in res for e in c["errors"]})}


# ───────────────────────── HTTP ─────────────────────────
class Handler(SimpleHTTPRequestHandler):
    def __init__(self, *a, **kw):
        super().__init__(*a, directory=STATIC, **kw)

    def log_message(self, fmt, *args):
        pass

    def send_json(self, obj, code=200):
        body = json.dumps(obj, ensure_ascii=False, default=str).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        q = {k: v[0] for k, v in urllib.parse.parse_qs(u.query).items()}
        if u.path == "/api/cities":
            return self.send_json(sorted(BY_CODE.values(), key=lambda c: c["name"]))
        key = (self.headers.get("X-Serpapi-Key") or "").strip() or server_key()
        if u.path == "/api/status":
            st = {"has_key": bool(key)}
            if key:
                try:
                    st["account"] = real.account(key)
                except Exception as e:  # noqa
                    st["key_error"] = str(e)
            return self.send_json(st)
        if u.path == "/api/search":
            try:
                o = find_city(q.get("from", "VIE"))
                if not o:
                    return self.send_json({"error": "Не знаю такого міста відправлення"}, 400)
                today = date.today()
                start = date.fromisoformat(q["start"]) if q.get("start") else today + timedelta(days=14)
                end = date.fromisoformat(q["end"]) if q.get("end") else start + timedelta(days=45)
                start = max(start, today + timedelta(days=1))
                end = min(max(end, start), start + timedelta(days=120))
                nmin = max(1, min(int(q.get("nmin", 3)), 30))
                nmax = max(nmin, min(int(q.get("nmax", 7)), 30))
                adults = max(1, min(int(q.get("adults", 1)), 9))
                min_rating = max(0.0, min(float(q.get("rating", 8)), 9.5))
                no_hostels = q.get("nohostel", "1") != "0"
                extra = (min_rating, no_hostels)
                stay_kind = q.get("stay", "both") if q.get("stay") in ("hotel", "apartment", "both") else "both"
                checks = max(1, min(int(q.get("checks", 4)), 10))
                use_real = bool(key) and q.get("demo") != "1"
                to = q.get("to", "").strip()
                if not to or to.lower() in ("any", "куди завгодно", "*"):
                    if use_real:
                        return self.send_json(search_anywhere_real(o, start, end, nmin, nmax, adults, *extra, stay_kind, checks, key))
                    return self.send_json(search_anywhere(o, start, end, nmin, nmax, adults, *extra))
                d = find_city(to)
                if not d:
                    return self.send_json({"error": f"Не знаю місто «{to}». Вибери зі списку."}, 400)
                if d["code"] == o["code"]:
                    return self.send_json({"error": "Звідки і куди — одне й те саме місто 🙂"}, 400)
                if use_real:
                    return self.send_json(search_real(o, d, start, end, nmin, nmax, adults, *extra, stay_kind, checks, key))
                return self.send_json(search(o, d, start, end, nmin, nmax, adults, True, *extra))
            except Exception as e:  # noqa
                return self.send_json({"error": f"Помилка: {e}"}, 500)
        return super().do_GET()


if __name__ == "__main__":
    print(f"✈  Мандрівний Шукач: http://localhost:{PORT}")
    print("   Ключ SerpApi:", "знайдено — СПРАВЖНІ ціни" if server_key() else "немає (можна ввести на сторінці), інакше ДЕМО-режим")
    ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()
