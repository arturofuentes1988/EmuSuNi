// EmuSuNi's statistics page: stats.json (aggregates), world.json (the map's outlines), rankings.json (the public
// profiles) and a profile's own file (p/<name>.bin: found and read with its code only - SHA-256 of it, in the browser).
(function () {
  "use strict";
  var E = EmuSuNi;
  var stats = null, world = null, rankings = null, profile = null;
  var failed = false, gamesDays = 7, rankMode = "all", rankChoice = "", showAllCountries = false;

  // (v00.24) A day of stats.json ("2026-11-08", UTC) as its day and month, in the page's language - read as the
  // calendar day it is, not as a moment (new Date("2026-11-08") would be the 7th west of Greenwich).
  function dayText(text) {
    var m = /^(\d{4})-(\d{2})-(\d{2})$/.exec(text || "");
    if (!m) return text || "";
    var d = new Date(+m[1], +m[2] - 1, +m[3]);
    try { return d.toLocaleDateString(E.lang() === "es" ? "es-ES" : "en-US", { day: "numeric", month: "long" }); }
    catch (e) { return text; }
  }
  function dayNumber(text) {  // (days since 1970, for the gaps between two days)
    var m = /^(\d{4})-(\d{2})-(\d{2})$/.exec(text || "");
    return m ? Date.UTC(+m[1], +m[2] - 1, +m[3]) / 86400000 : NaN;
  }
  // (v00.24) Each day's downloads: a day's total less the day before's (downloads.history), only between two days in a
  // row - a gap, or a total that went down (a file taken off GitHub), gives no bar.
  function downloadsADay(history) {
    var out = [];
    for (var i = 1; i < history.length; i++) {
      var n = (history[i].total || 0) - (history[i - 1].total || 0);
      if (dayNumber(history[i].date) - dayNumber(history[i - 1].date) === 1 && n >= 0)
        out.push({ x: history[i].date, y: n });
    }
    return out;
  }

  // ---- The page ----
  function render() {
    var state = E.byId("stats-state");
    // (00.95) No players counted yet (the published versions only): a note, the players' numbers and sections hidden -
    // the downloads stay.
    var noPlayers = !!stats && !E.hasPlayers(stats);
    if (!stats) {
      state.hidden = false;
      state.textContent = failed ? E.t("stats_none") : E.t("stats_loading");
    } else if (noPlayers) {
      state.hidden = false;
      state.textContent = E.t("players_soon");
    } else {
      var current = !stats.online || stats.online.current !== false;
      state.hidden = current;
      state.textContent = current ? "" : E.t("stats_stale");
    }
    ["t-online", "t-peak", "t-today", "t-week", "t-active", "t-new", "t-countries"].forEach(function (id) {
      E.byId(id).parentNode.hidden = noPlayers;
    });
    E.byId("stats-tiles").classList.toggle("solo", noPlayers);
    ["now-section", "countries", "games", "rooms-section", "rankings", "profile"].forEach(function (id) {
      E.byId(id).hidden = noPlayers;
    });
    document.querySelectorAll('.nav a[href="#rankings"], .nav a[href="#profile"]').forEach(function (a) {
      a.hidden = noPlayers;  // (their sections are hidden too)
    });
    var s = stats || {};
    var online = s.online || {};
    var active = s.active || {};
    var downloads = s.downloads || {};
    var daily = s.daily || [];
    E.byId("t-online").textContent = stats ? E.num(online.now || 0) : "–";
    // (v00.24) The most online at once today (daily's last day: the server's today), the players of the last 7 days
    // and the countries of the last 30.
    E.byId("t-peak").textContent = stats && daily.length ? E.num(daily[daily.length - 1].peak || 0) : "–";
    E.byId("t-today").textContent = stats ? E.num(active.today || 0) : "–";
    E.byId("t-week").textContent = stats ? E.num(active.last_7_days || 0) : "–";
    E.byId("t-active").textContent = stats ? E.num(active.last_30_days || 0) : "–";
    E.byId("t-countries").textContent = stats ? E.num((s.countries_30d || []).length) : "–";
    E.byId("t-new").textContent = stats ? E.num(s.new_installs_30d || 0) : "–";
    E.byId("t-downloads").textContent = downloads.total ? E.num(downloads.total) : "–";
    // Now: by console, by country.
    var byConsole = Object.keys(online.by_console || {}).map(function (k) { return { k: k, n: online.by_console[k] }; });
    byConsole.sort(function (a, b) { return b.n - a.n; });
    E.bars(E.byId("now-consoles"), byConsole.slice(0, 10), function (r) { return E.consoleName(r.k); },
           function (r) { return r.n; });
    var byCountry = Object.keys(online.by_country || {}).map(function (k) { return { k: k, n: online.by_country[k] }; });
    byCountry.sort(function (a, b) { return b.n - a.n; });
    E.bars(E.byId("now-countries"), byCountry.slice(0, 10), function (r) { return E.flag(r.k) + " " + E.countryName(r.k); },
           function (r) { return r.n; });
    // (v00.24) How many countries that is (the list shows 10).
    E.byId("now-countries-n").textContent = byCountry.length ?
      E.fmt(byCountry.length === 1 ? "now_countries_one" : "now_countries_n", E.num(byCountry.length)) : "";
    // The days.
    E.chart(E.byId("chart-daily"), daily.map(function (d) { return { x: d.date, y: d.active || 0 }; }), "line", E.t("p_daily"));
    // (v00.24) Each day's most online at once (daily's peak), and the highest of them.
    E.chart(E.byId("chart-peak"), daily.map(function (d) { return { x: d.date, y: d.peak || 0 }; }), "line", E.t("p_peak"));
    var best = null;
    daily.forEach(function (d) { if ((d.peak || 0) > 0 && (!best || d.peak > best.peak)) best = d; });
    E.byId("peak-record").textContent = best ? E.fmt("peak_record", E.num(best.peak), dayText(best.date)) : "";
    E.chart(E.byId("chart-new"), daily.map(function (d) { return { x: d.date, y: d.new || 0 }; }), "bars", E.t("p_new"));
    // Countries: the map and the list.
    var countries = s.countries_30d || [];
    drawMap(countries);
    E.bars(E.byId("list-countries"), showAllCountries ? countries : countries.slice(0, 12),
           function (r) { return E.flag(r.country) + " " + E.countryName(r.country); }, function (r) { return r.installs || 0; });
    var more = E.byId("more-countries");
    more.hidden = countries.length <= 12;
    more.textContent = showAllCountries ? E.t("show_less") : E.t("show_all");
    E.bars(E.byId("list-versions"), (s.versions_30d || []).slice(0, 8), function (r) { return r.version; },
           function (r) { return r.installs || 0; });
    E.bars(E.byId("list-consoles"), (s.consoles_30d || []).slice(0, 12), function (r) { return E.consoleName(r.console); },
           function (r) { return r.installs || 0; }, function (r) { return E.hours(r.hours || 0); });
    // (v00.24) The hours of every console together (consoles_30d: each console's, all of them listed).
    var hoursAll = 0;
    (s.consoles_30d || []).forEach(function (r) { hoursAll += r.hours || 0; });
    E.byId("consoles-hours").textContent = hoursAll > 0 ? E.fmt("consoles_hours", E.hours(hoursAll)) : "";
    // Games.
    E.byId("games-7").classList.toggle("on", gamesDays === 7);
    E.byId("games-30").classList.toggle("on", gamesDays === 30);
    var games = E.byId("list-games");
    E.clear(games);
    var top = (gamesDays === 7 ? s.top_games_7d : s.top_games_30d) || [];
    if (!top.length) games.appendChild(E.el("li", "empty", E.t("no_data")));
    top.slice(0, 20).forEach(function (g) {
      var li = E.el("li");
      li.appendChild(E.el("span", "title", g.title));
      // ((v00.24) and how many played it: an estimate of the installs, 3 at least for its title to show)
      li.appendChild(E.el("span", "console", E.consoleName(g.console) +
                          (g.installs ? " · " + E.fmt(g.installs === 1 ? "g_players_one" : "g_players", E.num(g.installs)) : "")));
      li.appendChild(E.el("span", "hours", E.hours(g.hours || 0)));
      games.appendChild(li);
    });
    // Downloads.
    E.chart(E.byId("chart-downloads"), (downloads.history || []).map(function (d) { return { x: d.date, y: d.total || 0 }; }),
            "line", E.t("p_downloads"));
    // (v00.24) The downloads of each day, and of the last 7 (the last day's total less the one 7 days before it).
    var history = downloads.history || [];
    E.chart(E.byId("chart-dl-daily"), downloadsADay(history), "bars", E.t("p_dl_daily"));
    var week = "";
    if (history.length) {
      var last = history[history.length - 1];
      history.forEach(function (d) {
        if (dayNumber(last.date) - dayNumber(d.date) === 7 && (last.total || 0) >= (d.total || 0))
          week = E.fmt("dl_week", E.num((last.total || 0) - (d.total || 0)));
      });
    }
    E.byId("dl-week").textContent = week;
    E.bars(E.byId("list-downloads"), (downloads.by_version || []).slice(0, 10), function (r) { return r.tag; },
           function (r) { return r.total || 0; });
    renderRooms(s);
    renderRankings();
    renderProfile();
    E.byId("stats-updated").textContent = s.generated ? E.fmt("stats_updated", E.date(s.generated, true)) : "";
  }

  function renderRooms(s) {
    var box = E.byId("rooms");
    E.clear(box);
    // (v00.24) The rooms open now in one line: public and private, those with a match on, the public ones' players
    // (the private ones' players aren't published).
    var all = E.rooms(stats);
    E.byId("rooms-summary").textContent = all && all.total ?
      E.fmt(all.total === 1 ? "rooms_summary_one" : "rooms_summary", E.num(all.total), E.num(all.playing), E.num(all.players)) : "";
    var list = s.public_rooms || [];
    if (!list.length) {
      box.appendChild(E.el("p", "empty", E.t("no_rooms")));
    } else {
      var table = E.el("table", "rooms");
      var head = E.el("tr");
      ["room_console", "room_game", "room_region", "room_players", "room_state"].forEach(function (k) {
        head.appendChild(E.el("th", null, E.t(k)));
      });
      table.appendChild(head);
      list.slice(0, 50).forEach(function (r) {
        var tr = E.el("tr");
        tr.appendChild(E.el("td", null, r.together ? E.t("together") : E.consoleName(r.console)));
        tr.appendChild(E.el("td", null, r.game || (r.together ? "–" : E.t("a_game"))));
        tr.appendChild(E.el("td", null, r.region || "–"));
        tr.appendChild(E.el("td", null, (r.players || 0) + "/" + (r.max || 2)));
        var td = E.el("td");
        td.appendChild(E.el("span", "chip" + (r.playing ? " playing" : ""), E.t(r.playing ? "room_playing" : "room_waiting")));
        tr.appendChild(td);
        table.appendChild(tr);
      });
      box.appendChild(table);
    }
    var p = s.private_rooms || {};
    E.byId("private-rooms").textContent = stats ? E.fmt("private_rooms", E.num(p.open || 0), E.num(p.playing || 0)) : "";
  }

  // ---- The map: each country filled by its players (one hue, lighter to darker; none: the neutral) ----
  function drawMap(countries) {
    var box = E.byId("map");
    E.clear(box);
    if (!world || !world.paths) return;
    var counts = {};
    var max = 0;
    countries.forEach(function (c) { counts[c.country] = c.installs || 0; max = Math.max(max, c.installs || 0); });
    var svg = document.createElementNS(E.NS, "svg");
    svg.setAttribute("viewBox", "0 0 " + world.width + " " + world.height);
    svg.setAttribute("role", "img");
    svg.setAttribute("aria-label", E.t("p_map"));
    var tip = E.byId("tooltip");
    Object.keys(world.paths).forEach(function (cc) {
      var path = document.createElementNS(E.NS, "path");
      path.setAttribute("d", world.paths[cc]);
      var n = counts[cc] || 0;
      if (n > 0) {
        // (A logarithmic step 1-5: a country of 1 player still shows, the biggest doesn't hide the rest.)
        var step = max <= 1 ? 5 : 1 + Math.round(4 * Math.log(n) / Math.log(max));
        path.setAttribute("class", "country on s" + step);
      } else {
        path.setAttribute("class", "country");
      }
      function show(ev) {
        if (!tip) return;
        tip.textContent = E.flag(cc) + " " + E.countryName(cc) + " · " + (n ? E.num(n) : E.t("map_none"));
        tip.hidden = false;
        tip.style.left = Math.min(window.innerWidth - tip.offsetWidth - 8, ev.clientX + 12) + "px";
        tip.style.top = (ev.clientY - 36) + "px";
      }
      path.addEventListener("pointermove", show);
      path.addEventListener("pointerdown", show);
      path.addEventListener("pointerleave", function () { if (tip) tip.hidden = true; });
      svg.appendChild(path);
    });
    box.appendChild(svg);
  }

  // ---- Rankings (public profiles only) ----
  function renderRankings() {
    ["all", "country", "console"].forEach(function (m) { E.byId("rank-" + m).classList.toggle("on", rankMode === m); });
    var select = E.byId("rank-choice");
    var box = E.byId("ranking");
    E.clear(box);
    var r = rankings || {};
    var rows = [];
    if (rankMode === "all") {
      select.hidden = true;
      rows = r.top || [];
    } else {
      var groups = (rankMode === "country" ? r.by_country : r.by_console) || {};
      var keys = Object.keys(groups);
      keys.sort(function (a, b) { return (groups[b] || []).length - (groups[a] || []).length || (a < b ? -1 : 1); });
      E.clear(select);
      keys.forEach(function (k) {
        var o = E.el("option", null, rankMode === "country" ? E.flag(k) + " " + E.countryName(k) : E.consoleName(k));
        o.value = k;
        select.appendChild(o);
      });
      select.hidden = !keys.length;
      if (keys.indexOf(rankChoice) < 0) rankChoice = keys[0] || "";
      select.value = rankChoice;
      rows = groups[rankChoice] || [];
    }
    // (v00.24) How many players made their profile public (rankings.json's public_profiles).
    var count = typeof r.public_profiles === "number" ? r.public_profiles : (r.top || []).length;
    E.byId("rank-profiles").textContent = count > 0 ?
      E.fmt(count === 1 ? "rank_profiles_one" : "rank_profiles", E.num(count)) : "";
    if (!rows.length) { box.appendChild(E.el("p", "empty", E.t("r_none"))); return; }
    var table = E.el("table", "rooms ranking");
    var head = E.el("tr");
    ["#", E.t("r_player"), E.t("r_hours"), E.t("r_top_game")].forEach(function (h) { head.appendChild(E.el("th", null, h)); });
    table.appendChild(head);
    rows.slice(0, 50).forEach(function (p, i) {
      var tr = E.el("tr");
      tr.appendChild(E.el("td", "rank", i + 1));
      tr.appendChild(E.el("td", null, (p.country ? E.flag(p.country) + " " : "") + p.nick));
      tr.appendChild(E.el("td", null, E.hours(p.hours || 0)));
      tr.appendChild(E.el("td", null, p.top_game ? p.top_game.title + " · " + E.consoleName(p.top_game.console) : "–"));
      table.appendChild(tr);
    });
    box.appendChild(table);
  }

  // ---- A profile: its file's name and its bytes from its code (SHA-256), nothing sent anywhere but the fetch ----
  var ALPHABET = "23456789ABCDEFGHJKMNPQRSTUVWXYZ";
  function normalizeCode(text) {
    var letters = (text || "").toUpperCase().replace(/[\s-]/g, "");
    if (letters.length !== 16) return "";
    for (var i = 0; i < 16; i++) if (ALPHABET.indexOf(letters[i]) < 0) return "";
    return letters.slice(0, 4) + "-" + letters.slice(4, 8) + "-" + letters.slice(8, 12) + "-" + letters.slice(12);
  }
  function sha256(text) {
    return crypto.subtle.digest("SHA-256", new TextEncoder().encode(text)).then(function (d) { return new Uint8Array(d); });
  }
  function hex(bytes, n) {
    var out = "";
    for (var i = 0; i < n; i++) out += (bytes[i] < 16 ? "0" : "") + bytes[i].toString(16);
    return out;
  }
  function openProfile(code) {
    return sha256("emusuni-profile-file:" + code).then(function (name) {
      return fetch(E.BASE() + "p/" + hex(name, 16) + ".bin?t=" + Math.floor(Date.now() / 60000), { cache: "no-store" });
    }).then(function (r) {
      if (r.status === 404) return null;
      if (!r.ok) throw new Error(r.status);
      return r.arrayBuffer();
    }).then(function (buffer) {
      if (!buffer) return null;
      var bytes = new Uint8Array(buffer);
      if (bytes.length < 4 || String.fromCharCode(bytes[0], bytes[1], bytes[2], bytes[3]) !== "ESP1") throw new Error("format");
      var data = bytes.slice(4);
      var blocks = [];
      for (var i = 0; i * 32 < data.length; i++) blocks.push(sha256("emusuni-profile-key:" + code + ":" + i));
      return Promise.all(blocks).then(function (keys) {
        for (var k = 0; k < data.length; k++) data[k] ^= keys[k >> 5][k & 31];
        return JSON.parse(new TextDecoder().decode(data));
      });
    });
  }
  function renderProfile() {
    var box = E.byId("profile-view");
    E.clear(box);
    if (!profile) return;
    if (profile.message) { box.appendChild(E.el("p", "empty", E.t(profile.message))); return; }
    var p = profile.data;
    var head = E.el("div", "profile-head");
    head.appendChild(E.el("strong", null, (p.country ? E.flag(p.country) + " " : "") + (p.nick || E.t("pr_private"))));
    head.appendChild(E.el("span", "note small", (p.public ? E.t("pr_public") : E.t("pr_private")) + " · " +
                       E.fmt("pr_since", E.date(p.since)) + " · " + E.fmt("pr_last", E.date(p.last_seen))));
    box.appendChild(head);
    var tiles = E.el("div", "tiles");
    [["hours", "pr_hours"], ["sessions", "pr_sessions"], ["online_matches", "pr_matches"]].forEach(function (f) {
      var tile = E.el("div", "tile");
      tile.appendChild(E.el("span", "tile-value", f[0] === "hours" ? (Math.round((p.hours || 0) * 10) / 10).toLocaleString() : E.num(p[f[0]] || 0)));
      tile.appendChild(E.el("span", "tile-label", E.t(f[1])));
      tiles.appendChild(tile);
    });
    box.appendChild(tiles);
    box.appendChild(E.el("h3", null, E.t("pr_consoles")));
    var consoles = E.el("ul", "bars");
    E.bars(consoles, (p.consoles || []).slice(0, 10), function (r) { return E.consoleName(r.console); },
           function (r) { return r.hours || 0; });
    box.appendChild(consoles);
    box.appendChild(E.el("h3", null, E.t("pr_games")));
    var games = E.el("ol", "games");
    (p.games || []).slice(0, 30).forEach(function (g) {
      var li = E.el("li");
      li.appendChild(E.el("span", "title", g.title || E.t("pr_unknown")));
      li.appendChild(E.el("span", "console", E.consoleName(g.console) + " · " + E.fmt("pr_last_played", E.date(g.last_played))));
      li.appendChild(E.el("span", "hours", E.hours(g.hours || 0)));
      games.appendChild(li);
    });
    if (!(p.games || []).length) games.appendChild(E.el("li", "empty", E.t("no_data")));
    box.appendChild(games);
  }

  // ---- Start ----
  E.start(STRINGS, render);
  E.byId("more-countries").addEventListener("click", function () { showAllCountries = !showAllCountries; render(); });
  E.byId("games-7").addEventListener("click", function () { gamesDays = 7; render(); });
  E.byId("games-30").addEventListener("click", function () { gamesDays = 30; render(); });
  ["all", "country", "console"].forEach(function (m) {
    E.byId("rank-" + m).addEventListener("click", function () { rankMode = m; rankChoice = ""; renderRankings(); });
  });
  E.byId("rank-choice").addEventListener("change", function (ev) { rankChoice = ev.target.value; renderRankings(); });
  E.byId("profile-form").addEventListener("submit", function (ev) {
    ev.preventDefault();
    var code = normalizeCode(E.byId("profile-code").value);
    if (!code) { profile = { message: "profile_bad" }; renderProfile(); return; }
    E.byId("profile-code").value = code;
    profile = { message: "stats_loading" };
    renderProfile();
    try { history.replaceState(null, "", "#profile"); } catch (e) {}
    openProfile(code).then(function (data) {
      profile = data ? { data: data } : { message: "profile_none" };
      renderProfile();
    }).catch(function () { profile = { message: "profile_error" }; renderProfile(); });
  });
  function load() {
    E.getStats().then(function (data) { stats = data; failed = false; render(); })
      .catch(function () { failed = true; render(); });
    E.getJson("rankings.json").then(function (data) { rankings = data; renderRankings(); }).catch(function () {});
  }
  fetch("world.json").then(function (r) { return r.json(); }).then(function (data) { world = data; render(); })
    .catch(function () { /* no map: the list stays */ });
  load();
  setInterval(load, 5 * 60 * 1000);
  var lastWidth = window.innerWidth, timer = 0;
  window.addEventListener("resize", function () {
    if (window.innerWidth === lastWidth) return;
    lastWidth = window.innerWidth;
    clearTimeout(timer);
    timer = setTimeout(render, 200);
  });
})();
