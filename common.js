// EmuSuNi's pages: what both share - the language (English / Spanish), the theme, the numbers' formats, the
// statistics' address, bars and charts (inline SVG). Plain JavaScript, no library. Every text that comes from the
// data is put in with textContent: nothing of it is read as HTML.
var EmuSuNi = (function () {
  "use strict";

  var REPO = "arturofuentes1988/EmuSuNi";
  // The public statistics' repository (stats.json, rankings.json, p/: published by the online server every 5
  // minutes). ?base=<url>: another place, for tests.
  var BASE = "https://raw.githubusercontent.com/arturofuentes1988/emusuni-stats/main/";
  try {
    var asked = new URLSearchParams(location.search).get("base");
    if (asked) BASE = asked.replace(/\/?$/, "/");
  } catch (e) {}

  var CONSOLES = [
    ["snes", "Super Nintendo", "Snes9x"], ["nes", "NES", "FCEUmm"], ["n64", "Nintendo 64", "Mupen64Plus-Next"],
    ["gb", "Game Boy", "mGBA"], ["gbc", "Game Boy Color", "mGBA"], ["gba", "Game Boy Advance", "mGBA"],
    ["ps1", "PlayStation", "SwanStation"], ["nds", "Nintendo DS", "melonDS DS"], ["3ds", "Nintendo 3DS", "Azahar"],
    ["psp", "PSP", "PPSSPP"], ["dc", "Dreamcast", "Flycast"], ["naomi", "NAOMI", "Flycast"],
    ["atomiswave", "Atomiswave", "Flycast"], ["sg1000", "SG-1000", "Genesis Plus GX"],
    ["sms", "Master System", "Genesis Plus GX"], ["gg", "Game Gear", "Genesis Plus GX"],
    ["md", "Genesis / Mega Drive", "Genesis Plus GX"], ["saturn", "Saturn", "YabaSanshiro"],
    ["pce", "PC Engine", "Beetle PCE Fast"], ["arcade", "Arcade", "FinalBurn Neo"]
  ];
  var CONSOLE_NAME = {};
  CONSOLES.forEach(function (c) { CONSOLE_NAME[c[0]] = c[1]; });

  var T = { en: {}, es: {} };
  var lang = "en";
  var listeners = [];

  function store(key, value) { try { localStorage.setItem(key, value); } catch (e) {} }
  function stored(key) { try { return localStorage.getItem(key); } catch (e) { return null; } }
  function t(key) { return (T[lang] && T[lang][key]) || T.en[key] || key; }
  function fmt(key) {
    var text = t(key);
    for (var i = 1; i < arguments.length; i++) text = text.replace("{}", arguments[i]);
    return text;
  }
  function locale() { return lang === "es" ? "es-ES" : "en-US"; }
  function num(n) { try { return new Intl.NumberFormat(locale()).format(n); } catch (e) { return String(n); } }
  function hours(h) {
    return fmt("hours_n", h < 10 ? (Math.round(h * 10) / 10).toLocaleString(locale()) : num(Math.round(h)));
  }
  function el(tag, cls, text) {
    var e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text !== undefined && text !== null) e.textContent = String(text);
    return e;
  }
  function byId(id) { return document.getElementById(id); }
  function clear(node) { while (node && node.firstChild) node.removeChild(node.firstChild); }
  function flag(cc) {
    if (!/^[A-Z]{2}$/.test(cc || "")) return "";
    return String.fromCodePoint(0x1F1E6 + cc.charCodeAt(0) - 65, 0x1F1E6 + cc.charCodeAt(1) - 65);
  }
  function countryName(cc) {
    try { return new Intl.DisplayNames([lang], { type: "region" }).of(cc) || cc; } catch (e) { return cc; }
  }
  function consoleName(key) { return CONSOLE_NAME[key] || key || "?"; }
  function date(text, withTime) {
    var d = new Date(text);
    if (isNaN(d)) return text || "";
    return withTime ? d.toLocaleString(locale(), { dateStyle: "medium", timeStyle: "short" }) :
                      d.toLocaleDateString(locale(), { dateStyle: "medium" });
  }

  // ---- Language and theme ----
  function applyLanguage() {
    document.documentElement.lang = lang;
    var nodes = document.querySelectorAll("[data-i18n]");
    for (var i = 0; i < nodes.length; i++) {
      var key = nodes[i].getAttribute("data-i18n");
      if (nodes[i].hasAttribute("data-html")) nodes[i].innerHTML = t(key);  // (our own strings, with <b> and <code>)
      else nodes[i].textContent = t(key);
    }
    var button = byId("lang");
    if (button) button.textContent = t("lang_button");
    listeners.forEach(function (f) { f(); });
  }
  function applyTheme(theme) {
    if (theme === "light" || theme === "dark") document.documentElement.setAttribute("data-theme", theme);
    else document.documentElement.removeAttribute("data-theme");
  }
  function currentTheme() {
    var set = document.documentElement.getAttribute("data-theme");
    if (set) return set;
    return window.matchMedia && window.matchMedia("(prefers-color-scheme: dark)").matches ? "dark" : "light";
  }
  function start(strings, onLanguage) {
    Object.keys(strings.en).forEach(function (k) { T.en[k] = strings.en[k]; });
    Object.keys(strings.es).forEach(function (k) { T.es[k] = strings.es[k]; });
    if (onLanguage) listeners.push(onLanguage);
    var chosen = stored("emusuni-lang");
    lang = chosen === "es" || chosen === "en" ? chosen :
      ((navigator.languages && navigator.languages[0]) || navigator.language || "en").toLowerCase().indexOf("es") === 0 ? "es" : "en";
    var theme = stored("emusuni-theme");
    if (theme) applyTheme(theme);
    var langButton = byId("lang");
    if (langButton) langButton.addEventListener("click", function () {
      lang = lang === "es" ? "en" : "es";
      store("emusuni-lang", lang);
      applyLanguage();
    });
    var themeButton = byId("theme");
    if (themeButton) themeButton.addEventListener("click", function () {
      var next = currentTheme() === "dark" ? "light" : "dark";
      applyTheme(next);
      store("emusuni-theme", next);
    });
    applyLanguage();
  }

  // ---- Data ----
  function getJson(file) {
    var url = BASE + file + (file.indexOf("?") < 0 ? "?" : "&") + "t=" + Math.floor(Date.now() / 60000);
    return fetch(url, { cache: "no-store" }).then(function (r) {
      if (!r.ok) throw new Error(r.status);
      return r.json();
    });
  }
  function getStats() { return getJson("stats.json"); }

  // ---- Bars: a label, its number, a bar (one series: the accent) ----
  function bars(list, rows, labelOf, valueOf, extraOf) {
    clear(list);
    if (!rows || !rows.length) { list.appendChild(el("li", "empty", t("no_data"))); return; }
    var max = 0;
    rows.forEach(function (r) { max = Math.max(max, valueOf(r)); });
    rows.forEach(function (r) {
      var li = el("li");
      var text = num(valueOf(r)) + (extraOf ? " · " + extraOf(r) : "");
      li.appendChild(el("span", "label", labelOf(r)));
      li.appendChild(el("span", "value", text));
      var track = el("span", "track");
      var fill = el("span", "fill");
      fill.style.width = (max > 0 ? Math.max(1, 100 * valueOf(r) / max) : 0) + "%";
      track.appendChild(fill);
      li.appendChild(track);
      li.title = labelOf(r) + ": " + text;
      list.appendChild(li);
    });
  }

  // ---- Charts (inline SVG, one series each): a line over time, or bars over time; a recessive grid, the first and
  // last dates, a crosshair and a tooltip. ----
  var NS = "http://www.w3.org/2000/svg";
  function niceTop(v) {
    var p = Math.pow(10, Math.floor(Math.log10(v)));
    var steps = [1, 1.5, 2, 3, 5, 7.5, 10];
    for (var i = 0; i < steps.length; i++) if (steps[i] * p >= v) return steps[i] * p;
    return 10 * p;
  }
  function compact(v) {
    if (v >= 1e6) return (v / 1e6).toFixed(v % 1e6 ? 1 : 0) + "M";
    if (v >= 1e3) return (v / 1e3).toFixed(v % 1e3 ? 1 : 0) + "k";
    return String(Math.round(v));
  }
  function chart(box, points, kind, label) {
    clear(box);
    if (!points.length) { box.appendChild(el("p", "empty", t("no_data"))); return; }
    var W = Math.max(300, Math.min(1000, Math.round(box.clientWidth || 640))), H = 200, L = 48, R = 12, Tp = 10, B = 24;
    var max = 0;
    points.forEach(function (p) { max = Math.max(max, p.y); });
    var top = max <= 0 ? 1 : niceTop(max);
    var n = points.length;
    var step = (W - L - R) / Math.max(1, kind === "bars" ? n : n - 1);
    function x(i) { return kind === "bars" ? L + step * (i + 0.5) : (n === 1 ? L + (W - L - R) / 2 : L + i * step); }
    function y(v) { return Tp + (H - Tp - B) * (1 - v / top); }
    var svg = document.createElementNS(NS, "svg");
    svg.setAttribute("viewBox", "0 0 " + W + " " + H);
    svg.setAttribute("role", "img");
    svg.setAttribute("aria-label", label + ": " + num(points[n - 1].y));
    function add(tag, attrs, text) {
      var e = document.createElementNS(NS, tag);
      for (var k in attrs) e.setAttribute(k, attrs[k]);
      if (text !== undefined) e.textContent = text;
      svg.appendChild(e);
      return e;
    }
    for (var g = 0; g <= 3; g++) {
      var v = top * g / 3;
      add("line", { x1: L, x2: W - R, y1: y(v), y2: y(v), "class": "grid-line" });
      add("text", { x: L - 6, y: y(v) + 4, "text-anchor": "end", "class": "axis-text" }, compact(v));
    }
    add("text", { x: L, y: H - 6, "text-anchor": "start", "class": "axis-text" }, points[0].x);
    if (n > 1) add("text", { x: W - R, y: H - 6, "text-anchor": "end", "class": "axis-text" }, points[n - 1].x);
    if (kind === "bars") {
      var w = Math.max(1, step - 2);  // (a 2px gap between bars)
      points.forEach(function (p, i) {
        var h = Math.max(0, y(0) - y(p.y));
        if (h > 0) add("rect", { x: x(i) - w / 2, y: y(p.y), width: w, height: h, rx: Math.min(4, w / 2), "class": "bar" });
      });
    } else {
      var d = "";
      points.forEach(function (p, i) { d += (i ? "L" : "M") + x(i).toFixed(1) + " " + y(p.y).toFixed(1); });
      add("path", { d: d + "L" + x(n - 1).toFixed(1) + " " + y(0) + "L" + x(0).toFixed(1) + " " + y(0) + "Z", "class": "area" });
      add("path", { d: d, "class": "line" });
      add("circle", { cx: x(n - 1), cy: y(points[n - 1].y), r: 4, "class": "dot" });
    }
    var cross = add("line", { x1: 0, x2: 0, y1: Tp, y2: H - B, "class": "cross", visibility: "hidden" });
    var dot = add("circle", { cx: 0, cy: 0, r: 4, "class": "dot", visibility: "hidden" });
    var hit = add("rect", { x: L, y: 0, width: W - L - R, height: H, fill: "transparent" });
    var tip = byId("tooltip");
    function move(ev) {
      var rect = svg.getBoundingClientRect();
      var px = (ev.clientX - rect.left) * W / rect.width;
      var i = kind === "bars" ? Math.floor((px - L) / step) : (n === 1 ? 0 : Math.round((px - L) / step));
      i = Math.max(0, Math.min(n - 1, i));
      cross.setAttribute("x1", x(i)); cross.setAttribute("x2", x(i)); cross.setAttribute("visibility", "visible");
      dot.setAttribute("cx", x(i)); dot.setAttribute("cy", y(points[i].y)); dot.setAttribute("visibility", "visible");
      if (tip) {
        tip.textContent = points[i].x + " · " + num(points[i].y);
        tip.hidden = false;
        tip.style.left = Math.min(window.innerWidth - tip.offsetWidth - 8, ev.clientX + 12) + "px";
        tip.style.top = (ev.clientY - 36) + "px";
      }
    }
    function leave() {
      cross.setAttribute("visibility", "hidden");
      dot.setAttribute("visibility", "hidden");
      if (tip) tip.hidden = true;
    }
    hit.addEventListener("pointermove", move);
    hit.addEventListener("pointerdown", move);
    hit.addEventListener("pointerleave", leave);
    box.appendChild(svg);
  }

  return {
    REPO: REPO, BASE: function () { return BASE; }, CONSOLES: CONSOLES, start: start, t: t, fmt: fmt, num: num,
    hours: hours, el: el, byId: byId, clear: clear, flag: flag, countryName: countryName, consoleName: consoleName,
    date: date, getJson: getJson, getStats: getStats, bars: bars, chart: chart, lang: function () { return lang; },
    NS: NS
  };
})();
