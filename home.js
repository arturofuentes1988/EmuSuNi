// EmuSuNi's home page: the latest release (GitHub's API), three live numbers (stats.json), the consoles' grid and
// (v00.23) each console's features.
(function () {
  "use strict";
  var E = EmuSuNi;
  var latest = null, apiDownloads = null, stats = null;

  // (v00.23) What each console has in the app, as the code has it (checked against v00.23's src/):
  //  on: online play - "friends" EmuSuNi's own rollback through its server (Netplay::RulesFor), "link" the Game Boy's
  //      link cable through it (src/GbLink.h), "wfc" the DS's Wi-Fi (nds_wifi: Wiimmfi, WiiLink WFC), "psp" PSP Online
  //      (psp_online, psp_adhoc_server), "dcnet" Dreamcast Online (dc_online), "" none;
  //  ra: RetroAchievements (Achievements::ConsoleId; the 3DS has none in the app); st: save states (all but OpenBOR,
  //  IngameMenu.cpp); ch: cheat codes (IngameMenu.cpp: the Dreamcast, NAOMI, Atomiswave, 3DS, 2600, 7800, Arcade and
  //  OpenBOR take none); bios: "no", "yes" (the player's own, needed), "opt" (optional: used when it is there), "neogeo"
  //  (only the Neo Geo games), "fds" (only the Famicom Disk System); pl: players on one PS4 (the PS4 lets 4 users in);
  //  files: src/Systems.cpp's extensions, zip: a .zip of one too (not by_path); note: a key of strings.js.
  var FEATURES = {
    snes: { on: "friends", ra: 1, st: 1, ch: 1, bios: "no", pl: 4, files: ".sfc .smc .swc .fig .bs", zip: 1 },
    nes: { on: "friends", ra: 1, st: 1, ch: 1, bios: "fds", pl: 4, files: ".nes .fds .unf .unif", zip: 1, note: "n_nes" },
    n64: { on: "", ra: 1, st: 1, ch: 1, bios: "no", pl: 4, files: ".z64 .n64 .v64", zip: 1, note: "n_hd4" },
    gb: { on: "link", ra: 1, st: 1, ch: 1, bios: "no", pl: 1, files: ".gb .sgb .dmg", zip: 1 },
    gbc: { on: "link", ra: 1, st: 1, ch: 1, bios: "no", pl: 1, files: ".gbc .cgb", zip: 1 },
    gba: { on: "", ra: 1, st: 1, ch: 1, bios: "no", pl: 1, files: ".gba .agb", zip: 1 },
    ps1: { on: "", ra: 1, st: 1, ch: 1, bios: "opt", pl: 4, files: ".cue .chd .pbp .m3u .iso .img .ecm .mds", note: "n_hd4" },
    nds: { on: "wfc", ra: 1, st: 1, ch: 1, bios: "opt", pl: 1, files: ".nds .ids" },
    "3ds": { on: "", ra: 0, st: 1, ch: 0, bios: "no", pl: 1, files: ".3ds .cci .cxi .3dsx", note: "n_3ds" },
    psp: { on: "psp", ra: 1, st: 1, ch: 1, bios: "no", pl: 1, files: ".iso .cso .chd .pbp .elf", note: "n_hd4" },
    dc: { on: "dcnet", ra: 1, st: 1, ch: 0, bios: "opt", pl: 4, files: ".gdi .cdi .chd .cue .m3u", note: "n_hd3" },
    naomi: { on: "", ra: 1, st: 1, ch: 0, bios: "yes", pl: 4, files: ".zip .7z .dat .bin", note: "n_hd3" },
    atomiswave: { on: "", ra: 1, st: 1, ch: 0, bios: "yes", pl: 4, files: ".zip .7z", note: "n_hd3" },
    sg1000: { on: "friends", ra: 1, st: 1, ch: 1, bios: "no", pl: 2, files: ".sg .sc", zip: 1 },
    sms: { on: "friends", ra: 1, st: 1, ch: 1, bios: "no", pl: 2, files: ".sms .bms", zip: 1 },
    gg: { on: "friends", ra: 1, st: 1, ch: 1, bios: "no", pl: 1, files: ".gg", zip: 1 },
    md: { on: "friends", ra: 1, st: 1, ch: 1, bios: "no", pl: 4, files: ".md .gen .smd .bin .mdx .68k .sgd", zip: 1 },
    saturn: { on: "", ra: 1, st: 1, ch: 1, bios: "yes", pl: 4, files: ".cue .chd .iso .ccd .mds", note: "n_saturn" },
    pce: { on: "friends", ra: 1, st: 1, ch: 1, bios: "no", pl: 4, files: ".pce", zip: 1, note: "n_pce" },
    atari2600: { on: "friends", ra: 1, st: 1, ch: 0, bios: "no", pl: 2, files: ".a26 .bin", zip: 1 },
    atari7800: { on: "friends", ra: 1, st: 1, ch: 0, bios: "opt", pl: 2, files: ".a78", zip: 1 },
    atarijaguar: { on: "", ra: 1, st: 1, ch: 1, bios: "opt", pl: 2, files: ".j64 .jag .abs .cof", zip: 1, note: "n_jaguar" },
    arcade: { on: "friends", ra: 1, st: 1, ch: 0, bios: "neogeo", pl: 4, files: ".zip .7z", note: "n_arcade" },
    openbor: { on: "", ra: 0, st: 0, ch: 0, bios: "no", pl: 4, files: ".pak", note: "n_openbor" }
  };
  var ONLINE = { friends: "on_friends", link: "on_link", wfc: "on_wfc", psp: "on_psp", dcnet: "on_dcnet" };
  // (v2) The online badge's server, short; "own" EmuSuNi's server, "comm" the community's.
  var SERVER = { friends: ["srv_emusuni", "own"], link: ["srv_emusuni", "own"], wfc: ["srv_wfc", "comm"],
                 psp: ["srv_psp", "comm"], dcnet: ["srv_dcnet", "comm"] };
  var BIOS = { no: "bios_no", yes: "bios_yes", opt: "bios_opt", neogeo: "bios_neogeo", fds: "bios_fds" };
  // (v2) The filters: each console's maker.
  var MAKER = { snes: "nintendo", nes: "nintendo", n64: "nintendo", gb: "nintendo", gbc: "nintendo", gba: "nintendo",
                nds: "nintendo", "3ds": "nintendo", ps1: "sony", psp: "sony", dc: "sega", naomi: "sega",
                atomiswave: "sega", sg1000: "sega", sms: "sega", gg: "sega", md: "sega", saturn: "sega",
                atari2600: "atari", atari7800: "atari", atarijaguar: "atari", pce: "other", arcade: "other",
                openbor: "other" };
  var FILTERS = ["all", "online", "nintendo", "sega", "sony", "atari", "other"];
  var filter = "all";

  function shown(key, f) {
    if (filter === "all") return true;
    if (filter === "online") return !!f.on;
    return MAKER[key] === filter;
  }

  function renderFilters() {
    var bar = E.byId("systems-filters");
    if (!bar) {
      var list = E.byId("systems-list");
      if (!list) return;
      bar = E.el("div", "sys-filters");
      bar.id = "systems-filters";
      bar.setAttribute("role", "group");
      list.parentNode.insertBefore(bar, list);
    }
    bar.setAttribute("aria-label", E.t("sf_label"));
    E.clear(bar);
    FILTERS.forEach(function (name) {
      var count = 0;
      E.CONSOLES.forEach(function (c) {
        var f = FEATURES[c[0]];
        if (f && (name === "all" || (name === "online" ? !!f.on : MAKER[c[0]] === name))) count++;
      });
      var b = E.el("button", "sys-filter" + (filter === name ? " on" : ""));
      b.type = "button";
      b.setAttribute("aria-pressed", filter === name ? "true" : "false");
      b.appendChild(E.el("span", null, E.t("sf_" + name)));
      b.appendChild(E.el("span", "n", String(count)));
      b.addEventListener("click", function () { filter = name; renderFilters(); renderSystems(); });
      bar.appendChild(b);
    });
  }

  function mark(yes) {
    var s = E.el("span", yes ? "yes" : "no", yes ? "✓" : "✕");
    s.setAttribute("aria-hidden", "true");
    var wrap = E.el("span", "mark");
    wrap.appendChild(s);
    wrap.appendChild(E.el("span", null, E.t(yes ? "sy_yes" : "sy_no")));
    return wrap;
  }

  function renderSystems() {
    var list = E.byId("systems-list");
    if (!list) return;
    E.clear(list);
    E.CONSOLES.forEach(function (c) {
      var f = FEATURES[c[0]];
      if (!f || !shown(c[0], f)) return;
      var card = E.el("article", "card sys2" + (f.on ? " is-on" : ""));
      // The head: the name and emulator, and the online badge.
      var head = E.el("header", "sys2-head");
      var names = E.el("div");
      names.appendChild(E.el("h3", null, c[1]));
      names.appendChild(E.el("p", "emu", c[2]));
      head.appendChild(names);
      var badge = E.el("span", "sys2-badge" + (f.on ? " on" : ""), E.t(f.on ? "sy_badge_on" : "sy_badge_off"));
      head.appendChild(badge);
      card.appendChild(head);
      // Online: how, through which server.
      if (f.on) {
        var box = E.el("div", "sys2-online " + SERVER[f.on][1]);
        var top = E.el("p", "srv");
        top.appendChild(E.el("b", null, E.t(SERVER[f.on][0])));
        top.appendChild(E.el("span", "kind", E.t(SERVER[f.on][1] === "own" ? "srv_kind_own" : "srv_kind_comm")));
        box.appendChild(top);
        box.appendChild(E.el("p", "how", E.t(ONLINE[f.on])));
        card.appendChild(box);
      }
      // The table.
      var table = E.el("table", "sys2-t");
      var body = E.el("tbody");
      function row(label, value) {
        var tr = E.el("tr");
        tr.appendChild(E.el("th", null, E.t(label)));
        var td = E.el("td");
        if (typeof value === "string") td.textContent = value; else td.appendChild(value);
        tr.appendChild(td);
        body.appendChild(tr);
      }
      row("sy_ra", mark(!!f.ra));
      row("sy_states", mark(!!f.st));
      row("sy_cheats", mark(!!f.ch));
      row("sy_bios", E.t(BIOS[f.bios]));
      var players = E.el("span", "pips");
      for (var i = 1; i <= 4; i++) {
        var pip = E.el("i", i <= f.pl ? "full" : null);
        pip.setAttribute("aria-hidden", "true");
        players.appendChild(pip);
      }
      players.appendChild(E.el("span", null, String(f.pl)));
      row("sy_players", players);
      var files = E.el("span", "files");
      f.files.split(" ").forEach(function (ext) { files.appendChild(E.el("code", null, ext)); });
      if (f.zip) files.appendChild(E.el("span", "zip", E.t("sy_zip")));
      row("sy_files", files);
      table.appendChild(body);
      card.appendChild(table);
      if (f.note) card.appendChild(E.el("p", "sys-note", E.t(f.note)));
      list.appendChild(card);
    });
  }

  // (v3) Every feature: each line as a small card (its words before ": " in bold), a count per group, and chips that
  // show one group. Runs after each language change: the lines' texts come back from their data-i18n keys.
  var evGroup = "all";
  function renderEvery() {
    var list = E.byId("every-list");
    if (!list) return;
    var groups = list.querySelectorAll(".ev-group");
    var total = 0;
    Array.prototype.forEach.call(groups, function (g) {
      var items = g.querySelectorAll(".ev-list li");
      total += items.length;
      Array.prototype.forEach.call(items, function (li) {
        var key = li.getAttribute("data-i18n");
        var text = E.t(key), title = E.t(key + "_t");
        if (title === key + "_t") title = "";
        // The text's own "Title: ..." start is left out when it only repeats the title.
        var cut = text.indexOf(": ");
        if (title && cut > 0 && text.slice(0, cut).toLowerCase() === title.toLowerCase())
          text = text.charAt(cut + 2).toUpperCase() + text.slice(cut + 3);
        E.clear(li);
        var ck = E.el("span", "ck", "✓");
        ck.setAttribute("aria-hidden", "true");
        li.appendChild(ck);
        var words = E.el("span", "tx");
        if (title) words.appendChild(E.el("b", null, title));
        words.appendChild(E.el("span", null, text));
        li.appendChild(words);
      });
      var count = g.querySelector(".ev-count");
      if (count) count.textContent = E.fmt("ev_n", items.length);
      g.hidden = evGroup !== "all" && g.getAttribute("data-group") !== evGroup;
    });
    var bar = E.byId("every-filters");
    if (!bar) {
      bar = E.el("div", "sys-filters");
      bar.id = "every-filters";
      bar.setAttribute("role", "group");
      list.parentNode.insertBefore(bar, list);
    }
    E.clear(bar);
    var chips = [["all", E.t("ev_all"), total]];
    Array.prototype.forEach.call(groups, function (g) {
      var h = g.querySelector("h3");
      chips.push([g.getAttribute("data-group"), h ? h.textContent : "", g.querySelectorAll(".ev-list li").length]);
    });
    chips.forEach(function (c) {
      var b = E.el("button", "sys-filter" + (evGroup === c[0] ? " on" : ""));
      b.type = "button";
      b.setAttribute("aria-pressed", evGroup === c[0] ? "true" : "false");
      b.appendChild(E.el("span", null, c[1]));
      b.appendChild(E.el("span", "n", String(c[2])));
      b.addEventListener("click", function () { evGroup = c[0]; renderEvery(); });
      bar.appendChild(b);
    });
  }

  function render() {
    renderEvery();
    var tag = E.byId("hero-version-tag");
    if (tag && latest && latest.tag) tag.textContent = latest.tag;
    // (v4) The online card's chips: the consoles on EmuSuNi's server, and those on the community's (with theirs).
    var own = E.byId("online-own"), comm = E.byId("online-comm");
    if (own && comm) {
      E.clear(own);
      E.clear(comm);
      E.CONSOLES.forEach(function (c) {
        var f = FEATURES[c[0]];
        if (!f || !f.on) return;
        var chip = E.el("span", "ft-chip");
        chip.appendChild(E.el("b", null, c[1]));
        if (SERVER[f.on][1] === "comm") chip.appendChild(E.el("span", null, E.t(SERVER[f.on][0])));
        (SERVER[f.on][1] === "own" ? own : comm).appendChild(chip);
      });
    }
    var more = E.byId("online-more");
    if (more && !more.dataset.ready) {
      more.dataset.ready = "1";
      more.addEventListener("click", function () { filter = "online"; renderFilters(); renderSystems(); });
    }
    // (v4) The consoles, by maker: a row of chips for each.
    var list = E.byId("consoles");
    E.clear(list);
    ["nintendo", "sega", "sony", "atari", "other"].forEach(function (m) {
      var group = E.el("div", "maker");
      var items = E.CONSOLES.filter(function (c) { return MAKER[c[0]] === m; });
      var head = E.el("p", "maker-head");
      head.appendChild(E.el("b", null, E.t("sf_" + m)));
      head.appendChild(E.el("span", null, String(items.length)));
      group.appendChild(head);
      var ul = E.el("ul", "maker-list");
      items.forEach(function (c) {
        var li = E.el("li");
        if (FEATURES[c[0]] && FEATURES[c[0]].on) li.className = "on";
        li.appendChild(E.el("b", null, c[1]));
        li.appendChild(E.el("span", null, c[2]));
        ul.appendChild(li);
      });
      group.appendChild(ul);
      list.appendChild(group);
    });
    renderFilters();
    renderSystems();
    if (latest && latest.url) E.byId("download").href = latest.url;
    E.byId("version").textContent = latest && latest.tag ? latest.tag : "";
    var total = stats && stats.downloads && stats.downloads.total ? stats.downloads.total : apiDownloads;
    E.byId("downloads-total").textContent = total ? E.fmt("downloads_n", E.num(total)) : "";
    E.byId("h-downloads").textContent = total ? E.num(total) : "–";
    E.byId("h-online").textContent = stats && stats.online ? E.num(stats.online.now || 0) : "–";
    E.byId("h-active").textContent = stats && stats.active ? E.num(stats.active.last_30_days || 0) : "–";
    // (00.95) No players counted yet (the published versions only): a note, the downloads alone.
    var noPlayers = !!stats && !E.hasPlayers(stats);
    E.byId("h-online").parentNode.hidden = noPlayers;
    E.byId("h-active").parentNode.hidden = noPlayers;
    E.byId("h-tiles").classList.toggle("solo", noPlayers);
    var note = E.byId("h-note");
    note.hidden = !noPlayers;
    note.textContent = noPlayers ? E.t("players_soon") : "";
  }

  E.start(STRINGS, render);
  fetch("https://api.github.com/repos/" + E.REPO + "/releases?per_page=100", { headers: { Accept: "application/vnd.github+json" } })
    .then(function (r) { if (!r.ok) throw new Error(r.status); return r.json(); })
    .then(function (releases) {
      var sum = 0;
      releases.forEach(function (rel) {
        if (rel.draft) return;
        var pkg = null;
        (rel.assets || []).forEach(function (a) {
          if (/\.pkg$/i.test(a.name || "")) { sum += a.download_count || 0; if (!pkg) pkg = a; }
        });
        if (!latest && !rel.prerelease && pkg) latest = { tag: rel.tag_name, url: pkg.browser_download_url };
      });
      apiDownloads = sum;
      render();
    })
    .catch(function () { /* the button keeps its link to the releases page */ });
  E.getStats().then(function (data) { stats = data; render(); }).catch(function () { /* the numbers stay "–" */ });
})();
