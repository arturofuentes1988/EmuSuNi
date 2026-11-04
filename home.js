// EmuSuNi's home page: the latest release (GitHub's API), three live numbers (stats.json), the consoles' grid.
(function () {
  "use strict";
  var E = EmuSuNi;
  var latest = null, apiDownloads = null, stats = null;

  function render() {
    var list = E.byId("consoles");
    E.clear(list);
    E.CONSOLES.forEach(function (c) {
      var li = E.el("li");
      li.appendChild(E.el("b", null, c[1]));
      li.appendChild(E.el("span", null, c[2]));
      list.appendChild(li);
    });
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
