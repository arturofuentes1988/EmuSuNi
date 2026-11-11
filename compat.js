// EmuSuNi's compatibility page (v00.23): the games played on a PS4 with EmuSuNi (compat.json), by console, with a
// rating, the region, the speed and the recommended settings; a search and filters by console and rating.
(function () {
  "use strict";
  var E = EmuSuNi;
  var RATINGS = ["perfect", "playable", "boots", "broken"];
  var data = null, failed = false;
  var consoleFilter = "all", ratingFilter = "all", query = "";

  function local(v) {
    if (!v) return "";
    if (typeof v === "string") return v;
    return v[document.documentElement.lang === "es" ? "es" : "en"] || v.en || v.es || "";
  }
  function norm(text) {
    return String(text || "").toLowerCase().normalize("NFD").replace(/[̀-ͯ]/g, "");
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
    if (query && norm(g.title).indexOf(query) < 0) return false;
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
    arcade: "own", atari2600: "own", atari7800: "own", gb: "link", gbc: "link", nds: "comm", psp: "comm", dc: "comm" };
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

  function renderSummary(games) {
    var box = E.byId("cp-summary");
    E.clear(box);
    var counts = {};
    games.forEach(function (g) { counts[g.rating] = (counts[g.rating] || 0) + 1; });
    var consoles = {};
    games.forEach(function (g) { consoles[g.console] = 1; });
    var tiles = E.el("div", "cp-tiles");
    var first = E.el("div", "cp-tile total");
    first.appendChild(E.el("b", null, E.num(games.length)));
    first.appendChild(E.el("span", null, E.fmt("cp_games_in", Object.keys(consoles).length)));
    tiles.appendChild(first);
    RATINGS.forEach(function (r) {
      var t = E.el("div", "cp-tile " + r);
      t.appendChild(E.el("b", null, E.num(counts[r] || 0)));
      t.appendChild(E.el("span", null, E.t("cp_r_" + r)));
      tiles.appendChild(t);
    });
    box.appendChild(tiles);
    box.appendChild(bar(counts, games.length));
  }

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

      var table = E.el("table", "cp-t");
      var thead = E.el("thead");
      var hr = E.el("tr");
      ["cp_h_game", "cp_h_region", "cp_h_rating", "cp_h_speed", "cp_h_online", "cp_h_settings", "cp_h_tested"].forEach(function (k) {
        var th = E.el("th", null, E.t(k));
        th.scope = "col";
        hr.appendChild(th);
      });
      thead.appendChild(hr);
      table.appendChild(thead);
      var body = E.el("tbody");
      rows.forEach(function (g) {
        var tr = E.el("tr", "r-" + g.rating);
        var title = E.el("td", "cp-game");
        title.appendChild(E.el("b", null, g.title));
        var note = local(g.notes);
        if (note) title.appendChild(E.el("span", "cp-note", note));
        tr.appendChild(title);
        function cell(label, content, cls) {
          var td = E.el("td", cls || null);
          td.setAttribute("data-label", E.t(label));
          if (typeof content === "string") td.textContent = content || "–";
          else td.appendChild(content);
          tr.appendChild(td);
        }
        cell("cp_h_region", g.region || "–", "cp-region");
        cell("cp_h_rating", badge(g.rating), "cp-rating");
        cell("cp_h_speed", g.speed || "–", "cp-speed");
        cell("cp_h_online", onlineCell(g), "cp-on");
        cell("cp_h_settings", local(g.settings) || E.t("cp_default"), "cp-settings");
        var tested = E.el("span", "cp-tested");
        tested.appendChild(E.el("span", "cp-ver", g.version || ""));
        tested.appendChild(E.el("span", null, E.t(g.by === "user" ? "cp_by_user" : "cp_by_team")));
        if (g.date) tested.appendChild(E.el("span", "cp-date", E.date(g.date + "T12:00:00")));
        cell("cp_h_tested", tested, "cp-when");
        body.appendChild(tr);
      });
      table.appendChild(body);
      group.appendChild(table);
      list.appendChild(group);
    });
    if (!shown) list.appendChild(E.el("p", "note cp-empty", E.t("cp_none")));
  }

  // «From players' reports» (compat-reportes.json): the speed the players' own logs measured, apart from our tests.
  // Only the game, its console and numbers: never a report code, a country or a file name.
  var users = null;
  function consoleName(key) {
    for (var i = 0; i < E.CONSOLES.length; i++) if (E.CONSOLES[i][0] === key) return E.CONSOLES[i][1];
    return key;
  }
  function usersGroup(titleKey, descKey, games) {
    var group = E.el("section", "cp-group");
    var head = E.el("header", "cp-head");
    var names = E.el("div", "cp-names");
    names.appendChild(E.el("h3", null, E.t(titleKey)));
    names.appendChild(E.el("span", "emu", E.t(descKey)));
    head.appendChild(names);
    group.appendChild(head);
    var table = E.el("table", "cp-t cu-t");
    var hr = E.el("tr");
    ["cp_h_game", "cu_h_logs", "cu_h_median", "cu_h_full"].forEach(function (k) {
      var th = E.el("th", null, E.t(k));
      th.scope = "col";
      hr.appendChild(th);
    });
    var thead = E.el("thead");
    thead.appendChild(hr);
    table.appendChild(thead);
    var body = E.el("tbody");
    games.forEach(function (g) {
      var tr = E.el("tr");
      var title = E.el("td", "cp-game");
      title.appendChild(E.el("b", null, g.title));
      title.appendChild(E.el("span", "cp-note", consoleName(g.console)));
      tr.appendChild(title);
      [["cu_h_logs", E.num(g.reports)], ["cu_h_median", g.median + " %"], ["cu_h_full", g.full + " %"]].forEach(function (c) {
        var td = E.el("td", "cp-speed", c[1]);
        td.setAttribute("data-label", E.t(c[0]));
        tr.appendChild(td);
      });
      body.appendChild(tr);
    });
    table.appendChild(body);
    group.appendChild(table);
    return group;
  }
  function renderUsers() {
    var box = E.byId("cp-users");
    if (!box || !users) return;
    var good = users.good || [], slow = users.slow || [];
    box.hidden = !good.length && !slow.length;
    E.byId("cu-sub").textContent = E.fmt("cu_sub", users.version || "", E.num(users.reports || 0),
      users.updated ? E.date(users.updated + "T12:00:00") : "");
    var lists = E.byId("cu-lists");
    E.clear(lists);
    if (good.length) lists.appendChild(usersGroup("cu_good", "cu_good_d", good));
    if (slow.length) lists.appendChild(usersGroup("cu_slow", "cu_slow_d", slow));
  }

  function render() {
    renderUsers();
    var state = E.byId("cp-state");
    if (!data) {
      state.hidden = false;
      state.textContent = E.t(failed ? "cp_error" : "cp_loading");
      return;
    }
    state.hidden = true;
    var games = (data.games || []).filter(function (g) { return g && g.title && RATINGS.indexOf(g.rating) >= 0; });
    renderTools(games);
    renderSummary(games);
    renderList(games);
    E.byId("cp-updated").textContent = data.updated ? E.fmt("cp_updated", E.date(data.updated + "T12:00:00")) : "";
  }

  E.start(STRINGS, render);
  var input = E.byId("cp-q");
  if (input) input.addEventListener("input", function () { query = norm(input.value.trim()); if (data) renderList((data.games || []).filter(function (g) { return g && g.title && RATINGS.indexOf(g.rating) >= 0; })); });
  fetch("compat.json", { cache: "no-store" })
    .then(function (r) { if (!r.ok) throw new Error(r.status); return r.json(); })
    .then(function (json) { data = json; render(); })
    .catch(function () { failed = true; render(); });
  fetch("compat-reportes.json", { cache: "no-store" })
    .then(function (r) { if (!r.ok) throw new Error(r.status); return r.json(); })
    .then(function (json) { users = json; renderUsers(); })
    .catch(function () { /* the section stays hidden */ });
})();
