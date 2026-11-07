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
$("#mode").textContent = "≈ ціни — оцінка, точні — на сайтах за посиланнями";

$("#swap").onclick = () => { const a = $("#from").value; $("#from").value = $("#to").value; $("#to").value = a; };
$("#anywhere").onclick = () => { $("#to").value = "Куди завгодно"; run(); };
$("#f").onsubmit = (e) => { e.preventDefault(); run(); };

function params(extra = {}) {
  return new URLSearchParams({
    from: $("#from").value || "Відень", to: $("#to").value,
    start: $("#start").value, end: $("#end").value,
    nmin: $("#nmin").value, nmax: $("#nmax").value, adults: $("#adults").value,
    rating: $("#rating").value, nohostel: $("#nohostel").checked ? "1" : "0", ...extra,
  });
}

async function run() {
  msg.innerHTML = `<p class="loading">⏳ Шукаю серед сотень комбінацій дат…</p>`;
  out.innerHTML = "";
  try {
    const r = await fetch("/api/search?" + params());
    const data = await r.json();
    if (data.error) { msg.innerHTML = `<p class="err">${esc(data.error)}</p>`; return; }
    msg.innerHTML = "";
    data.results ? renderAnywhere(data) : renderRoute(data);
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
  out.innerHTML = `
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
    const r = await fetch("/api/search?" + params({ to: d.destination.code, start: td.dataset.dep, end: td.dataset.dep, nmin: td.dataset.n, nmax: td.dataset.n }));
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
  out.innerHTML = `<div class="card"><h2>🌍 Найдешевші напрямки з міста ${esc(d.origin.name)}</h2>
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
