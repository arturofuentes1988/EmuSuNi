// EmuSuNi's compatibility page: the games played on a PS4 with EmuSuNi, one table a console. The team's tests
// (compat.json: a rating, the region, the speed, online and the recommended settings) and the speed in the players'
// own logs (compat-reportes.json: the logs, the typical speed and the time at full speed) in the same rows; a game in
// both is one row. A search and filters by console and rating; the legend under the tables (no summary tiles).
(function () {
  "use strict";
  var E = EmuSuNi;
  // The team's ratings, then (v00.24) the two the players' logs give a game the team hasn't rated.
  var RATINGS = ["perfect", "playable", "boots", "broken", "logfast", "logslow"];
  var data = null, failed = false, users = null, usersDone = false;
  var consoleFilter = "all", ratingFilter = "all", query = "";

  function local(v) {
    if (!v) return "";
    if (typeof v === "string") return v;
    return v[document.documentElement.lang === "es" ? "es" : "en"] || v.en || v.es || "";
  }
  function norm(text) {
    return String(text || "").toLowerCase().normalize("NFD").replace(/[̀-ͯ]/g, "");
  }
  // A title as a key: no accents, no punctuation, no "The" in front ("The Legend of Zelda: X" = "Legend of Zelda - X").
  function titleKey(text) {
    return norm(text).replace(/[^a-z0-9]+/g, " ").trim().replace(/^the /, "");
  }
  function emulator(key) {
    for (var i = 0; i < E.CONSOLES.length; i++) if (E.CONSOLES[i][0] === key) return E.CONSOLES[i][2];
    return "";
  }
  function badge(rating) {
    return E.el("span", "cp-badge " + rating, E.t("cp_r_" + rating));
  }
  // A bar of the ratings' shares, and its words for screen readers.
  function bar(counts, total) {
    var b = E.el("div", "cp-bar");
    b.setAttribute("role", "img");
    var words = [];
    RATINGS.forEach(function (r) {
      if (!counts[r]) return;
      var seg = E.el("span", r);
      seg.style.flexGrow = String(counts[r]);
      b.appendChild(seg);
      words.push(counts[r] + " " + E.t("cp_r_" + r));
    });
    b.setAttribute("aria-label", words.join(", ") + " / " + total);
    return b;
  }
  function matches(g) {
    if (consoleFilter !== "all" && g.console !== consoleFilter) return false;
    if (ratingFilter !== "all" && g.rating !== ratingFilter) return false;
    if (query && norm(g.title).indexOf(query) < 0 && norm(g.aka || "").indexOf(query) < 0) return false;
    return true;
  }
  function chip(label, count, on, extra, click) {
    var b = E.el("button", "sys-filter" + (on ? " on" : "") + (extra ? " " + extra : ""));
    b.type = "button";
    b.setAttribute("aria-pressed", on ? "true" : "false");
    b.appendChild(E.el("span", null, label));
    b.appendChild(E.el("span", "n", String(count)));
    b.addEventListener("click", click);
    return b;
  }

  // (v00.24) The two lists in one: each of the team's games with its logs' numbers (the newest version that has the
  // game), and the games only the logs have, rated by them (logfast / logslow).
  function verNum(v) {
    var m = /([0-9]+)\.([0-9]+)/.exec(v || "");
    return m ? parseInt(m[1], 10) * 1000 + parseInt(m[2], 10) : 0;
  }
  function merged() {
    var team = ((data && data.games) || []).filter(function (g) {
      return g && g.title && RATINGS.indexOf(g.rating) >= 0 && RATINGS.indexOf(g.rating) < 4;
    });
    var logs = {};
    ((users && users.games) || []).forEach(function (r) {
      if (!r || !r.title || (r.verdict !== "good" && r.verdict !== "slow")) return;
      var k = r.console + "|" + titleKey(r.title);
      if (!logs[k] || verNum(r.version) > verNum(logs[k].version)) logs[k] = r;
    });
    var used = {};
    var rows = team.map(function (g) {
      var row = Object.assign({}, g);
      var keys = [titleKey(g.title)];
      Object.keys(logs).forEach(function (k) {
        var r = logs[k];
        if (r.console !== g.console || used[k]) return;
        if (keys.indexOf(titleKey(r.title)) >= 0 || (r.aka && keys.indexOf(titleKey(r.aka)) >= 0)) {
          row.logs = r;
          used[k] = 1;
        }
      });
      return row;
    });
    var only = Object.keys(logs).filter(function (k) { return !used[k]; }).map(function (k) {
      var r = logs[k];
      return { console: r.console, title: r.title, aka: r.aka, rating: r.verdict === "good" ? "logfast" : "logslow",
        logs: r };
    });
    only.sort(function (a, b) {
      return (a.rating === b.rating ? 0 : a.rating === "logfast" ? -1 : 1) || b.logs.reports - a.logs.reports ||
        a.title.localeCompare(b.title);
    });
    return rows.concat(only);
  }

  function renderTools(games) {
    var q = E.byId("cp-q");
    if (q) q.placeholder = E.t("cp_search");
    var cons = E.byId("cp-consoles"), rats = E.byId("cp-ratings");
    E.clear(cons);
    E.clear(rats);
    cons.setAttribute("aria-label", E.t("sf_label"));
    rats.setAttribute("aria-label", E.t("cp_h_rating"));
    // (v2) No "All" chips: a chip toggles its filter, and pressing it again (or Clear filters) shows everything.
    E.CONSOLES.forEach(function (c) {
      var n = games.filter(function (g) { return g.console === c[0]; }).length;
      if (!n) return;
      cons.appendChild(chip(c[1], n, consoleFilter === c[0], null, function () {
        consoleFilter = consoleFilter === c[0] ? "all" : c[0]; render();
      }));
    });
    RATINGS.forEach(function (r) {
      var n = games.filter(function (g) { return g.rating === r; }).length;
      if (!n) return;
      rats.appendChild(chip(E.t("cp_r_" + r), n, ratingFilter === r, "r-" + r, function () {
        ratingFilter = ratingFilter === r ? "all" : r; render();
      }));
    });
    var clear = E.byId("cp-clear");
    clear.hidden = consoleFilter === "all" && ratingFilter === "all";
    if (!clear.dataset.ready) {
      clear.dataset.ready = "1";
      clear.addEventListener("click", function () { consoleFilter = "all"; ratingFilter = "all"; render(); });
    }
  }

  // (v2) How a game plays online: its own words, else what its console has. A rollback console (EmuSuNi's server)
  // plays every game with a friend; on the community's servers it depends on the game, so "–" without its words.
  var ONLINE_BY_CONSOLE = { snes: "own", nes: "own", sg1000: "own", sms: "own", gg: "own", md: "own", pce: "own",
    arcade: "own", atari2600: "own", atari7800: "own", gb: "link", gbc: "link", gba: "link", nds: "comm", psp: "comm",
    dc: "comm" };  // ((v00.24) the Game Boy Advance's link cable online too)
  function onlineCell(g) {
    var own = local(g.online);
    var mode = ONLINE_BY_CONSOLE[g.console];
    var box = E.el("span", "cp-online" + (own || mode === "own" || mode === "link" ? " yes" : ""));
    if (own) box.textContent = own;
    else if (mode === "own") box.textContent = E.t("cp_on_own");
    else if (mode === "link") box.textContent = E.t("cp_on_link");
    else if (mode === "comm") box.textContent = "–";
    else box.textContent = E.t("cp_on_no");
    return box;
  }

  // The logs' numbers under the legend: each version's logs and day, from compat-reportes.json.
  function renderLogsNote() {
    var p = E.byId("cp-logs-note");
    if (!p) return;
    var parts = ((users && users.versions) || []).map(function (v) {
      var day = v.updated ? E.date(v.updated + "T12:00:00") : "";
      return v.players ? E.fmt("cp_logs_ver_p", v.version, E.num(v.reports), E.num(v.players), day)
        : E.fmt("cp_logs_ver", v.version, E.num(v.reports), day);
    });
    // (v00.24) And the team's own logs (its tests' versions), apart.
    var team = users && users.team;
    var ours = team ? E.fmt("cp_logs_team", E.num(team.runs), team.from || "", team.to || "") : "";
    p.hidden = !parts.length && !ours;
    p.textContent = parts.length || ours ? E.fmt("cp_logs_note", parts.join("; "), ours) : "";
  }

  var HEADS = ["cp_h_game", "cp_h_region", "cp_h_rating", "cp_h_speed", "cu_h_logs", "cu_h_median", "cu_h_full",
    "cp_h_online", "cp_h_settings", "cp_h_version"];
  function renderList(games) {
    var list = E.byId("cp-list");
    E.clear(list);
    var shown = 0;
    E.CONSOLES.forEach(function (c) {
      var all = games.filter(function (g) { return g.console === c[0]; });
      var rows = all.filter(matches);
      if (!rows.length) return;
      shown += rows.length;
      var group = E.el("section", "cp-group");
      var head = E.el("header", "cp-head");
      var names = E.el("div", "cp-names");
      names.appendChild(E.el("h2", null, c[1]));
      names.appendChild(E.el("span", "emu", emulator(c[0])));
      head.appendChild(names);
      var counts = {};
      all.forEach(function (g) { counts[g.rating] = (counts[g.rating] || 0) + 1; });
      var meta = E.el("div", "cp-meta");
      meta.appendChild(E.el("span", "cp-count", E.fmt(all.length === 1 ? "cp_n_tested_one" : "cp_n_tested", all.length)));
      meta.appendChild(bar(counts, all.length));
      head.appendChild(meta);
      group.appendChild(head);

      var scroll = E.el("div", "cp-scroll");
      var table = E.el("table", "cp-t");
      var thead = E.el("thead");
      var hr = E.el("tr");
      HEADS.forEach(function (k) {
        var th = E.el("th", null, E.t(k));
        th.scope = "col";
        if (k.indexOf("cu_") === 0) th.className = "cp-num";
        hr.appendChild(th);
      });
      thead.appendChild(hr);
      table.appendChild(thead);
      var body = E.el("tbody");
      rows.forEach(function (g) {
        var team = g.rating.indexOf("log") !== 0;
        var tr = E.el("tr", "r-" + g.rating + (team ? "" : " cp-fromlogs"));
        var title = E.el("td", "cp-game");
        var phone = badge(g.rating);  // the rating's copy shown beside the title on phones (style.css)
        phone.className += " cp-badge-m";
        title.appendChild(phone);
        title.appendChild(E.el("b", null, g.title));
        var note = local(g.notes);
        if (note) title.appendChild(E.el("span", "cp-note", note));
        tr.appendChild(title);
        // A cell with its column's name (the label on phones); "cp-none" when it has nothing (hidden on phones).
        function cell(label, content, cls) {
          var empty = content === "" || content === null || content === undefined;
          var td = E.el("td", (cls || "") + (empty ? " cp-none" : ""));
          td.setAttribute("data-label", E.t(label));
          if (empty) td.textContent = "–";
          else if (typeof content === "string") td.textContent = content;
          else td.appendChild(content);
          tr.appendChild(td);
        }
        var r = g.logs;
        cell("cp_h_region", g.region || "", "cp-region");
        cell("cp_h_rating", badge(g.rating), "cp-rating");
        // The speed breaks only at its " · " ("60 FPS · 100 %": never a "%" alone on a line).
        cell("cp_h_speed", (g.speed || "").split(" · ").map(function (p) { return p.replace(/ /g, "\u00a0"); })
          .join(" · "), "cp-speed");
        cell("cu_h_logs", r ? E.num(r.reports) : "", "cp-speed cp-num cp-logs cp-logs1");
        cell("cu_h_median", r ? E.num(r.median) + "\u00a0%" : "", "cp-speed cp-num cp-logs");
        cell("cu_h_full", r ? E.num(r.full) + "\u00a0%" : "", "cp-speed cp-num cp-logs cp-logs3");
        cell("cp_h_online", onlineCell(g), "cp-on");
        cell("cp_h_settings", team ? (local(g.settings) || E.t("cp_default")) : "", "cp-settings");
        // The version: the team's test (its version, who, the day), and the logs' version when it is another one.
        var ver = E.el("span", "cp-vers");
        if (team) {
          var tested = E.el("span", "cp-tested");
          tested.appendChild(E.el("span", "cp-ver", g.version || ""));
          tested.appendChild(E.el("span", null, E.t(g.by === "user" ? "cp_by_user" : "cp_by_team")));
          if (g.date) tested.appendChild(E.el("span", "cp-date", E.date(g.date + "T12:00:00")));
          ver.appendChild(tested);
        }
        if (r) {
          var fromLogs = E.el("span", "cp-tested");
          fromLogs.appendChild(E.el("span", "cp-ver", r.version || ""));
          fromLogs.appendChild(E.el("span", null, E.t(r.source === "team" ? "cp_by_teamlogs" : "cp_by_logs")));
          ver.appendChild(fromLogs);
        }
        cell("cp_h_version", ver, "cp-when");
        body.appendChild(tr);
      });
      table.appendChild(body);
      scroll.appendChild(table);
      group.appendChild(scroll);
      list.appendChild(group);
    });
    if (!shown) list.appendChild(E.el("p", "note cp-empty", E.t("cp_none")));
  }

  function render() {
    renderLogsNote();
    var state = E.byId("cp-state");
    // The team's list and the logs' come together (the logs may be missing: then only the team's).
    if (!data || !usersDone) {
      state.hidden = false;
      state.textContent = E.t(failed ? "cp_error" : "cp_loading");
      return;
    }
    state.hidden = true;
    var games = merged();
    renderTools(games);
    renderList(games);
    var day = data.updated || "";
    if (users && users.updated && users.updated > day) day = users.updated;
    E.byId("cp-updated").textContent = day ? E.fmt("cp_updated", E.date(day + "T12:00:00")) : "";
  }

  E.start(STRINGS, render);
  var input = E.byId("cp-q");
  if (input) input.addEventListener("input", function () {
    query = norm(input.value.trim());
    if (data && usersDone) renderList(merged());
  });
  fetch("compat.json", { cache: "no-store" })
    .then(function (r) { if (!r.ok) throw new Error(r.status); return r.json(); })
    .then(function (json) { data = json; render(); })
    .catch(function () { failed = true; render(); });
  fetch("compat-reportes.json", { cache: "no-store" })
    .then(function (r) { if (!r.ok) throw new Error(r.status); return r.json(); })
    .then(function (json) { users = json; })
    .catch(function () { users = null; })
    .then(function () { usersDone = true; render(); });
})();
