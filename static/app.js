const $ = (s) => document.querySelector(s);
const out = $("#out"), msg = $("#msg");
const MODE_ICON = { flight: "✈️ Літак", bus: "🚌 Автобус", train: "🚆 Потяг" };
const STAY_NAME = { hostel: "🛏 Хостел", apartment: "🏠 Апартаменти", hotel: "🏨 Готель" };
const fmtD = (s) => new Date(s + "T00:00").toLocaleDateString("uk-UA", { day: "numeric", month: "short", weekday: "short" });
const eur = (n) => `${Math.round(n).toLocaleString("uk-UA")} €`;
const esc = (s) => String(s ?? "").replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));

// ── початкові значення ──
const iso = (d) => d.toISOString().slice(0, 10);
const t = new Date();
$("#start").value = iso(new Date(t.getTime() + 14 * 864e5));
$("#end").value = iso(new Date(t.getTime() + 60 * 864e5));

fetch("/api/cities").then((r) => r.json()).then((list) => {
  const opts = list.map((c) => `<option value="${esc(c.name)}">${esc(c.en)} · ${esc(c.country)} (${c.code})</option>`).join("");
  $("#cities").innerHTML = opts;
  $("#cities-to").innerHTML = `<option value="Куди завгодно">🌍 найдешевші напрямки</option>` + opts;
});
// ── ключ SerpApi (зберігається лише в цьому браузері) ──
const store = {
  get() { try { return localStorage.getItem("serpapi_key") || ""; } catch { return ""; } },
  set(v) { try { v ? localStorage.setItem("serpapi_key", v) : localStorage.removeItem("serpapi_key"); } catch {} },
};
const hdrs = () => (store.get() ? { "X-Serpapi-Key": store.get() } : {});
let REAL = false;
async function status() {
  const s = await fetch("/api/status", { headers: hdrs() }).then((r) => r.json()).catch(() => ({}));
  REAL = !!s.has_key && !s.key_error;
  const left = s.account && s.account.left != null ? ` · лишилось запитів: ${s.account.left}` : "";
  $("#mode").textContent = REAL ? `🟢 справжні ціни (Google Flights + Google Hotels)${left}` : "🟡 ДЕМО — ціни вигадані";
  $("#keystate").textContent = s.key_error ? `— помилка ключа: ${s.key_error}` : REAL ? "— підключено ✅" : "— не задано";
  if (!REAL) $("#keybox").open = true;
}
$("#savekey").onclick = () => { store.set($("#key").value.trim()); $("#key").value = ""; status(); };
$("#delkey").onclick = () => { store.set(""); status(); };
status();

$("#swap").onclick = () => { const a = $("#from").value; $("#from").value = $("#to").value; $("#to").value = a; };
$("#anywhere").onclick = () => { $("#to").value = "Куди завгодно"; run(); };
$("#f").onsubmit = (e) => { e.preventDefault(); run(); };

function params(extra = {}) {
  return new URLSearchParams({
    from: $("#from").value || "Відень", to: $("#to").value,
    start: $("#start").value, end: $("#end").value,
    nmin: $("#nmin").value, nmax: $("#nmax").value, adults: $("#adults").value,
    rating: $("#rating").value, nohostel: $("#nohostel").checked ? "1" : "0",
    stay: $("#stay").value, checks: $("#checks").value, ...extra,
  });
}

async function run() {
  msg.innerHTML = REAL
    ? `<p class="loading">⏳ Перевіряю справжні ціни в Google Flights і Google Hotels… (до хвилини; ≈ ${$("#checks").value * ($("#stay").value === "both" ? 3 : 2)} запитів, повторний пошук — з кешу безкоштовно)</p>`
    : `<p class="loading">⏳ Шукаю серед сотень комбінацій дат…</p>`;
  out.innerHTML = "";
  try {
    const r = await fetch("/api/search?" + params(), { headers: hdrs() });
    const data = await r.json();
    if (data.error) { msg.innerHTML = `<p class="err">${esc(data.error)}</p>`; return; }
    msg.innerHTML = "";
    if (data.mode === "real") data.results ? renderAnywhereReal(data) : renderReal(data);
    else data.results ? renderAnywhere(data) : renderRoute(data);
  } catch (e) {
    msg.innerHTML = `<p class="err">Сервер не відповідає: ${esc(e.message)}</p>`;
  }
}

// ── колір для теплової карти: зелений (дешево) → жовтий → червоний ──
function heatColor(v, lo, hi) {
  const x = hi === lo ? 0 : (v - lo) / (hi - lo);
  const stops = [[31, 174, 106], [242, 201, 76], [229, 72, 77]];
  const [a, b, k] = x < 0.5 ? [stops[0], stops[1], x * 2] : [stops[1], stops[2], (x - 0.5) * 2];
  return `rgb(${a.map((c, i) => Math.round(c + (b[i] - c) * k)).join(",")})`;
}

function renderRoute(d) {
  const o = d.origin, ds = d.destination, best = d.best[0];
  const cheapest = d.best.filter((c) => c.deal === "absurd").length;
  out.innerHTML = DEMO_BANNER + `
  <div class="card">
    <h2>${esc(o.name)} → ${esc(ds.name)} <span class="muted">${d.distance_km} км · ${d.adults} ос. · житло ★${d.min_rating}+${d.no_hostels ? ", без хостелів" : ""} · оцінка цін</span></h2>
    <div class="stats">
      <div class="stat"><span>Найдешевша поїздка (все разом)</span><b>${eur(best.total)}</b><span>${fmtD(best.dep)} → ${fmtD(best.ret)}</span></div>
      <div class="stat"><span>Звичайна ціна (медіана)</span><b>${eur(d.median)}</b><span>економія ${Math.round((1 - best.total / d.median) * 100)}%</span></div>
      <div class="stat"><span>Найдорожчий варіант</span><b>${eur(d.max)}</b><span>в ці ж дати</span></div>
      <div class="stat"><span>«Смішно дешевих» знахідок</span><b>${d.cells.filter((c) => c.deal === "absurd").length}</b><span>дешевше 55% від звичайного</span></div>
    </div>
  </div>
  <div class="card"><h2>📈 Найнижча ціна за датою вильоту</h2><div id="chart"></div></div>
  <div class="card"><h2>🗓 Теплова карта: дата вильоту × кількість ночей</h2>
    <p class="muted">Число в клітинці — повна ціна (транспорт + житло) у €. Клікни клітинку, щоб побачити деталі й посилання.</p>
    <div class="heat-wrap" id="heat"></div>
    <div class="legend">дешево <i></i> дорого · <span style="color:var(--wow)">▢</span> червона рамка — «аж смішно дешево»</div>
  </div>
  <div id="picked"></div>
  <h2 style="margin-top:22px">🏆 Топ найвигідніших поїздок ${cheapest ? `<span class="muted">(з них ${cheapest} — 🔥 аномально дешеві)</span>` : ""}</h2>
  <div class="deals">${d.best.map((c) => dealCard(c, d)).join("")}</div>`;
  drawChart(d);
  drawHeat(d);
}

function drawChart(d) {
  const byDay = {};
  d.cells.forEach((c) => { if (!byDay[c.dep] || c.total < byDay[c.dep]) byDay[c.dep] = c.total; });
  const days = Object.keys(byDay).sort(), vals = days.map((k) => byDay[k]);
  const W = Math.max(600, $("#chart").clientWidth), H = 220, P = { l: 48, r: 12, t: 12, b: 28 };
  const lo = Math.min(...vals) * 0.9, hi = Math.max(...vals) * 1.05;
  const x = (i) => P.l + (i / Math.max(1, days.length - 1)) * (W - P.l - P.r);
  const y = (v) => P.t + (1 - (v - lo) / (hi - lo)) * (H - P.t - P.b);
  const pts = vals.map((v, i) => `${x(i)},${y(v)}`).join(" ");
  const ticksY = [0, 0.25, 0.5, 0.75, 1].map((k) => lo + (hi - lo) * k);
  const step = Math.ceil(days.length / 8);
  const minI = vals.indexOf(Math.min(...vals));
  $("#chart").innerHTML = `<svg viewBox="0 0 ${W} ${H}" width="100%" role="img" aria-label="Графік цін">
    ${ticksY.map((v) => `<line x1="${P.l}" x2="${W - P.r}" y1="${y(v)}" y2="${y(v)}" stroke="var(--line)"/><text x="${P.l - 6}" y="${y(v) + 4}" text-anchor="end">${Math.round(v)}€</text>`).join("")}
    <line x1="${P.l}" x2="${W - P.r}" y1="${y(d.median)}" y2="${y(d.median)}" stroke="var(--muted)" stroke-dasharray="4 4"/>
    <text x="${W - P.r}" y="${y(d.median) - 5}" text-anchor="end">медіана ${d.median}€</text>
    <polyline points="${pts}" fill="none" stroke="var(--accent)" stroke-width="2.5" stroke-linejoin="round"/>
    ${vals.map((v, i) => `<circle cx="${x(i)}" cy="${y(v)}" r="${i === minI ? 6 : 3}" fill="${v < d.median * 0.55 ? "var(--wow)" : "var(--accent)"}"><title>${fmtD(days[i])}: від ${v}€</title></circle>`).join("")}
    ${days.map((k, i) => (i % step ? "" : `<text x="${x(i)}" y="${H - 8}" text-anchor="middle">${k.slice(8)}.${k.slice(5, 7)}</text>`)).join("")}
  </svg>`;
}

function drawHeat(d) {
  const days = [...new Set(d.cells.map((c) => c.dep))].sort();
  const nights = [...new Set(d.cells.map((c) => c.nights))].sort((a, b) => a - b);
  const map = {}; d.cells.forEach((c) => (map[c.dep + "|" + c.nights] = c));
  const lo = d.min, hi = Math.min(d.max, d.median * 1.8);
  let h = `<table class="heat"><tr><th>ночей ↓ / виліт →</th>${days.map((k) => `<th>${k.slice(8)}.${k.slice(5, 7)}</th>`).join("")}</tr>`;
  nights.forEach((n) => {
    h += `<tr><th>${n}</th>` + days.map((k) => {
      const c = map[k + "|" + n];
      if (!c) return "<td></td>";
      return `<td class="${c.deal === "absurd" ? "absurd" : ""}" style="background:${heatColor(Math.min(c.total, hi), lo, hi)}"
        data-dep="${k}" data-n="${n}" title="${fmtD(k)} → ${fmtD(c.ret)}: ${c.total}€">${Math.round(c.total)}</td>`;
    }).join("") + "</tr>";
  });
  $("#heat").innerHTML = h + "</table>";
  $("#heat").onclick = async (e) => {
    const td = e.target.closest("td[data-dep]"); if (!td) return;
    const r = await fetch("/api/search?" + params({ to: d.destination.code, start: td.dataset.dep, end: td.dataset.dep, nmin: td.dataset.n, nmax: td.dataset.n, demo: "1" }));
    const one = await r.json();
    if (one.best) {
      one.best[0].deal = (d.cells.find((c) => c.dep === td.dataset.dep && c.nights == td.dataset.n) || {}).deal;
      $("#picked").innerHTML = `<h2>👆 Вибраний варіант</h2><div class="deals">${dealCard(one.best[0], d)}</div>`;
      $("#picked").scrollIntoView({ behavior: "smooth", block: "start" });
    }
  };
}

function linkRow(list) {
  return `<div class="links">${(list || []).map((l) => `<a href="${esc(l.url)}" target="_blank" rel="noopener">${esc(l.site)} ↗</a>`).join("")}</div>`;
}

function dealCard(c, d) {
  const tr = c.transport, L = c.links || {};
  const tag = c.deal === "absurd" ? `<span class="tag">🔥 аж смішно дешево</span>` : c.deal === "great" ? `<span class="tag great">👍 вигідно</span>` : "";
  const trInfo = tr.mode === "flight"
    ? `${tr.transfers ? tr.transfers + " перес." : "прямий"}${tr.airline ? " · " + esc(tr.airline) : ""}`
    : `~${tr.hours} год в дорозі`;
  const alt = tr.mode !== "flight" && c.flight ? `<div class="src">або літак ≈ ${eur(c.flight.price)}</div>` : "";
  const altG = tr.mode === "flight" && c.ground ? `<div class="src">або ${MODE_ICON[c.ground.type]} ≈ ${eur(c.ground.price)}</div>` : "";
  return `<div class="deal ${c.deal === "absurd" ? "absurd" : ""}">
    ${tag}
    <div class="price">${eur(c.total)} <small>${d.adults > 1 ? `· ${eur(c.per_person)}/ос.` : "все разом"}</small></div>
    <div class="dates">${fmtD(c.dep)} → ${fmtD(c.ret)} · ${c.nights} ноч.</div>
    <div class="line"><div>${MODE_ICON[tr.mode]} туди-назад<div class="m">${trInfo}</div>${alt}${altG}</div><b>${eur(tr.price)}</b></div>
    ${tr.mode === "flight" ? linkRow(L.flight) : `<div class="muted">Туди:</div>${linkRow(L.ground)}<div class="muted">Назад:</div>${linkRow(L.ground_back)}<div class="muted">Літаком:</div>${linkRow(L.flight)}`}
    ${tr.mode === "flight" && c.ground ? `<div class="muted">Автобус / потяг:</div>${linkRow(L.ground)}` : ""}
    <div class="line"><div>${STAY_NAME[c.stay.type]} <span class="stars">★ ${c.stay.rating}</span><div class="m">${c.stays.slice(1).map((s) => `${STAY_NAME[s.type]} ★${s.rating} ≈ ${eur(s.price)}`).join(" · ")}</div></div><b>${eur(c.stay.price)}</b></div>
    ${linkRow(L.stay)}
    <div class="src">≈ оціночна ціна, точну перевір на сайті</div>
  </div>`;
}

function renderAnywhere(d) {
  const max = Math.max(...d.results.map((r) => r.best.total));
  out.innerHTML = DEMO_BANNER + `<div class="card"><h2>🌍 Найдешевші напрямки з міста ${esc(d.origin.name)}</h2>
    <p class="muted">Повна ціна найкращої комбінації дат (транспорт + житло з рейтингом ★${d.min_rating}+) для ${d.adults} ос. Клікни напрямок — покажу всі дати й посилання.</p>
    <div class="dest-list">${d.results.map((r) => {
      const b = r.best, absurd = b.ratio < 0.45;
      return `<div class="item" data-code="${r.destination.code}">
        <div><span class="nm">${absurd ? "🔥 " : ""}${esc(r.destination.name)}</span> <span class="muted">${esc(r.destination.country)} · ${MODE_ICON[b.transport.mode]} · ${fmtD(b.dep)} → ${fmtD(b.ret)} · ${b.nights} ноч.</span></div>
        <b>${eur(b.total)}</b>
        <div class="bar ${absurd ? "absurd" : ""}" style="width:${(b.total / max) * 100}%"></div>
      </div>`;
    }).join("")}</div></div>`;
  out.querySelectorAll(".item").forEach((el) => (el.onclick = () => {
    const c = d.results.find((r) => r.destination.code === el.dataset.code).destination;
    $("#to").value = c.name; run(); window.scrollTo({ top: 0, behavior: "smooth" });
  }));
}

// ═════════════════════════ СПРАВЖНІ ЦІНИ ═════════════════════════
const DEMO_BANNER = `<div class="banner demo">⚠️ ДЕМО-режим: ціни й рейтинги нижче ВИГАДАНІ моделлю. Для справжніх — додай ключ SerpApi (вгорі, «🔑 Ключ»).</div>`;
const LEVEL = { low: "🟢 низька для цього маршруту", typical: "🟡 звичайна", high: "🔴 висока" };
const KIND = { hotel: "🏨 Готель", apartment: "🏠 Апартаменти" };
const time = (s) => (s ? s.slice(11, 16) : "");
const errBlock = (e) => (e && e.length ? `<div class="card err-list">Не все вдалося перевірити: ${e.map(esc).join(" · ")}</div>` : "");

function hotelRow(h, main) {
  const src = h.sources && h.sources[0] ? `найдешевше на ${esc(h.sources[0].source)}` : "";
  return `<div class="hotel">
    ${h.photo ? `<img src="${esc(h.photo)}" alt="" loading="lazy">` : "<span></span>"}
    <div><div class="nm">${KIND[h.kind] || ""} ${esc(h.name)}${h.stars ? " · " + "★".repeat(h.stars) : ""}</div>
      <div><span class="stars">${h.rating}/5</span> <span class="muted">(${h.reviews.toLocaleString("uk-UA")} відгуків · ≈${h.rating10}/10)</span></div>
      <div class="muted">${eur(h.per_night)}/ніч ${src ? "· " + src : ""}</div>
      <div class="links">${h.sources && h.sources[0] && h.sources[0].link ? `<a href="${esc(h.sources[0].link)}" target="_blank" rel="noopener">${esc(h.sources[0].source)} ↗</a>` : ""}
        <a href="${esc(h.booking)}" target="_blank" rel="noopener">Booking ↗</a>
        ${h.link ? `<a href="${esc(h.link)}" target="_blank" rel="noopener">Сайт ↗</a>` : ""}</div>
    </div>
    <b>${main ? "" : eur(h.price)}</b>
  </div>`;
}

function realCard(c, d) {
  const f = c.flight, h = c.stay, L = c.links || {};
  const tag = c.deal === "absurd" ? `<span class="tag">🔥 аж смішно дешево</span>` : c.deal === "great" ? `<span class="tag great">👍 нижче звичайного</span>` : "";
  const ins = c.insights || {};
  return `<div class="deal ${c.deal === "absurd" ? "absurd" : ""}">
    ${tag}
    <div class="price">${c.total != null ? eur(c.total) : "—"} <small>${d.adults > 1 ? `на ${d.adults} ос.` : "все разом"}</small></div>
    <div class="dates">${fmtD(c.dep)} → ${fmtD(c.ret)} · ${c.nights} ноч.</div>
    ${c.ground_only ? `<div class="line"><div>🚌🚆 Автобус / потяг туди-назад<div class="m">близько — літак не потрібен; ціна квитка зазвичай 5–30 €, точну дивись на сайті</div></div><b>+ квиток</b></div>
      <div class="muted">Туди:</div>${linkRow(L.ground)}<div class="muted">Назад:</div>${linkRow(L.ground_back)}` : `
    <div class="line"><div>✈️ Літак туди-назад
      ${f ? `<div class="m">${f.logo ? `<img src="${esc(f.logo)}" alt="" style="width:18px;height:18px;vertical-align:middle"> ` : ""}${esc(f.airlines.join(", "))} · ${f.from}→${f.to} · виліт ${time(f.depart)} · ${f.stops ? f.stops + " перес." : "прямий"}${f.duration ? " · " + f.duration : ""}</div>` : `<div class="m">рейсів не знайдено</div>`}
      ${ins.level ? `<div class="m">Ціна за Google: ${LEVEL[ins.level] || esc(ins.level)}${ins.typical ? ` (зазвичай ${ins.typical[0]}–${ins.typical[1]} €)` : ""}</div>` : ""}
      </div><b>${f ? eur(f.price) : "—"}</b></div>
    ${linkRow(L.flight)}
    ${L.ground && L.ground.length ? `<div class="muted">Автобус / потяг (ціну дивись на сайті):</div>${linkRow(L.ground)}` : ""}`}
    <div class="line"><div>Житло ★${d.min_rating / 2}+ з 5 (≥ 30 відгуків)</div><b>${h ? eur(h.price) : "—"}</b></div>
    ${h ? hotelRow(h, true) : `<div class="muted">Нічого не знайдено з таким рейтингом — спробуй нижчий.</div>`}
    ${c.stay_alts && c.stay_alts.length ? `<div class="alts">Інші варіанти:</div>${c.stay_alts.map((x) => hotelRow(x, false)).join("")}` : ""}
    <div class="muted">Більше житла:</div>${linkRow([{ site: "Google Hotels", url: c.hotels_url }, ...(L.stay || [])])}
    <div class="src">✅ справжні ціни на момент пошуку (Google Flights / Google Hotels)</div>
  </div>`;
}

function renderReal(d) {
  const o = d.origin, ds = d.destination, best = d.best[0];
  const all = d.checked;
  const withIns = all.find((c) => c.insights && c.insights.history && c.insights.history.length) || {};
  out.innerHTML = `<div class="banner real">✅ Справжні ціни з Google Flights і Google Hotels · перевірено ${all.length} варіантів дат</div>
  ${errBlock(d.errors)}
  <div class="card">
    <h2>${esc(o.name)} → ${esc(ds.name)} <span class="muted">${d.distance_km} км · ${d.adults} ос. · житло від ${d.min_rating / 2}/5</span></h2>
    ${best ? `<div class="stats">
      <div class="stat"><span>Найдешевша поїздка (${best.flight ? "переліт + житло" : "житло, без квитка"})</span><b>${eur(best.total)}</b><span>${fmtD(best.dep)} → ${fmtD(best.ret)}</span></div>
      ${best.flight ? `<div class="stat"><span>Переліт туди-назад</span><b>${eur(best.flight.price)}</b><span>${esc(best.flight.airlines.join(", "))}</span></div>` : `<div class="stat"><span>Дорога</span><b>автобус/потяг</b><span>ціна на сайті перевізника</span></div>`}
      <div class="stat"><span>Житло</span><b>${eur(best.stay.price)}</b><span>${esc(best.stay.name)} · ${best.stay.rating}/5</span></div>
      ${best.insights && best.insights.typical ? `<div class="stat"><span>Звичайна ціна квитка (Google)</span><b>${best.insights.typical[0]}–${best.insights.typical[1]} €</b><span>${LEVEL[best.insights.level] || ""}</span></div>` : ""}
    </div>` : `<p class="err">Не знайшлося жодної повної комбінації (переліт + житло). Спробуй інші дати або нижчий рейтинг.</p>`}
  </div>
  <div class="card"><h2>📊 Повна ціна за перевіреними датами</h2><div id="bars"></div></div>
  ${withIns.insights ? `<div class="card"><h2>📈 Як змінювалась ціна квитка (${fmtD(withIns.dep)} → ${fmtD(withIns.ret)}), дані Google</h2><div id="hist"></div></div>` : ""}
  <h2 style="margin-top:22px">🏆 Найвигідніші варіанти</h2>
  <div class="deals">${d.best.map((c) => realCard(c, d)).join("")}</div>`;
  drawBars(all);
  if (withIns.insights) drawHistory(withIns.insights);
}

function drawBars(all) {
  const rows = all.map((c) => ({ c, f: c.flight ? c.flight.price : 0, h: c.stay ? c.stay.price : 0 }));
  const max = Math.max(1, ...rows.map((r) => r.f + r.h));
  const W = Math.max(320, $("#bars").clientWidth), rowH = 34, L = 150, H = rows.length * rowH + 30;
  const x = (v) => (v / max) * (W - L - 70);
  $("#bars").innerHTML = `<svg viewBox="0 0 ${W} ${H}" width="100%" role="img" aria-label="Ціни за датами">
    ${rows.map((r, i) => {
      const y = i * rowH + 6;
      return `<text x="0" y="${y + 17}">${fmtD(r.c.dep)} → ${fmtD(r.c.ret).split(",")[1] || ""}</text>
      <rect x="${L}" y="${y}" width="${x(r.f)}" height="22" rx="4" fill="var(--accent)"><title>Переліт ${r.f} €</title></rect>
      <rect x="${L + x(r.f) + 2}" y="${y}" width="${Math.max(0, x(r.h) - 2)}" height="22" rx="4" fill="var(--good)"><title>Житло ${r.h} €</title></rect>
      <text x="${L + x(r.f + r.h) + 8}" y="${y + 16}" style="fill:var(--text);font-weight:700">${r.h && (r.f || r.c.ground_only) ? r.f + r.h + " €" : "немає даних"}${r.c.deal === "absurd" ? " 🔥" : ""}</text>`;
    }).join("")}
    <rect x="${L}" y="${H - 16}" width="10" height="10" fill="var(--accent)"/><text x="${L + 14}" y="${H - 7}">переліт</text>
    <rect x="${L + 80}" y="${H - 16}" width="10" height="10" fill="var(--good)"/><text x="${L + 94}" y="${H - 7}">житло</text>
  </svg>`;
}

function drawHistory(ins) {
  const pts = ins.history.filter((p) => Array.isArray(p) && p.length === 2);
  if (pts.length < 2) { $("#hist").innerHTML = `<p class="muted">Google не дав історії ціни.</p>`; return; }
  const W = Math.max(320, $("#hist").clientWidth), H = 200, P = { l: 44, r: 10, t: 10, b: 24 };
  const v = pts.map((p) => p[1]), lo = Math.min(...v, ...(ins.typical || [])) * 0.9, hi = Math.max(...v, ...(ins.typical || [])) * 1.05;
  const x = (i) => P.l + (i / (pts.length - 1)) * (W - P.l - P.r);
  const y = (val) => P.t + (1 - (val - lo) / (hi - lo)) * (H - P.t - P.b);
  const d0 = (ts) => new Date(ts * 1000).toLocaleDateString("uk-UA", { day: "numeric", month: "short" });
  $("#hist").innerHTML = `<svg viewBox="0 0 ${W} ${H}" width="100%" role="img" aria-label="Історія ціни">
    ${ins.typical ? `<rect x="${P.l}" y="${y(ins.typical[1])}" width="${W - P.l - P.r}" height="${y(ins.typical[0]) - y(ins.typical[1])}" fill="var(--accent-2)"/>
      <text x="${W - P.r}" y="${y(ins.typical[1]) - 3}" text-anchor="end">звичайний діапазон ${ins.typical[0]}–${ins.typical[1]} €</text>` : ""}
    <polyline points="${pts.map((p, i) => `${x(i)},${y(p[1])}`).join(" ")}" fill="none" stroke="var(--accent)" stroke-width="2.5"/>
    <text x="${P.l - 6}" y="${y(hi) + 10}" text-anchor="end">${Math.round(hi)}€</text><text x="${P.l - 6}" y="${y(lo)}" text-anchor="end">${Math.round(lo)}€</text>
    <text x="${P.l}" y="${H - 6}">${d0(pts[0][0])}</text><text x="${W - P.r}" y="${H - 6}" text-anchor="end">сьогодні: ${pts[pts.length - 1][1]} €</text>
  </svg>`;
}

function renderAnywhereReal(d) {
  const max = Math.max(1, ...d.results.map((r) => r.best.total));
  out.innerHTML = `<div class="banner real">✅ Справжні ціни · перевірено ${d.results.length} напрямків, які зазвичай найдешевші з міста ${esc(d.origin.name)}</div>
  ${errBlock(d.errors)}
  <div class="card"><h2>🌍 Куди дешево з міста ${esc(d.origin.name)}</h2>
    <p class="muted">Переліт туди-назад + житло ★${d.min_rating / 2}+/5. Клікни — перевірю більше дат для цього міста.</p>
    <div class="dest-list">${d.results.map((r) => {
      const b = r.best;
      return `<div class="item" data-code="${r.destination.code}">
        <div><span class="nm">${b.deal === "absurd" ? "🔥 " : ""}${esc(r.destination.name)}</span> <span class="muted">${esc(r.destination.country)} · ✈️ ${esc(b.flight.airlines.join(", "))} ${eur(b.flight.price)} · ${esc(b.stay.name)} ${b.stay.rating}/5 ${eur(b.stay.price)} · ${fmtD(b.dep)} → ${fmtD(b.ret)}</span></div>
        <b>${eur(b.total)}</b>
        <div class="bar ${b.deal === "absurd" ? "absurd" : ""}" style="width:${(b.total / max) * 100}%"></div>
      </div>`;
    }).join("") || `<p class="muted">Нічого не знайшлося.</p>`}</div></div>`;
  out.querySelectorAll(".item").forEach((el) => (el.onclick = () => {
    $("#to").value = d.results.find((r) => r.destination.code === el.dataset.code).destination.name; run();
    window.scrollTo({ top: 0, behavior: "smooth" });
  }));
}
