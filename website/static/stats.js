(function () {
  "use strict";

  // Live download stats, pulled straight from the GitHub REST API in the
  // browser. The API allows unauthenticated CORS requests (60/h per IP),
  // which is plenty for a fan stats page. We cache the last good result in
  // localStorage so a refresh is instant and a rate-limit/offline hiccup
  // still shows numbers instead of an error.

  var REPO = "mszula/wacki";
  var API = "https://api.github.com/repos/" + REPO + "/releases?per_page=100";
  var CACHE_KEY = "wacki-stats-v1";
  var CACHE_TTL_MS = 10 * 60 * 1000; // 10 min — stale cache still beats nothing

  // name-substring → platform bucket. Robust to future filename tweaks.
  var PLATFORMS = [
    { key: "windows",    match: ["windows", "win64", "win32"], icon: "🪟", label: "Windows",            kind: "pc",       color: "#2b6fb0" },
    { key: "mac",        match: ["macos", "darwin", "osx"],    icon: "🍎", label: "macOS",              kind: "pc",       color: "#e0431a" },
    { key: "linux",      match: ["linux"],                     icon: "🐧", label: "Linux",              kind: "pc",       color: "#ff7b08" },
    { key: "miyoo",      match: ["miyoo", "onion"],            icon: "🕹️", label: "Miyoo",              kind: "handheld", color: "#14a84a" },
    { key: "portmaster", match: ["portmaster"],                icon: "🎮", label: "Anbernic / PortMaster", kind: "handheld", color: "#b8330f" },
    { key: "ps2",        match: ["ps2", "playstation"],         icon: "📀", label: "PlayStation 2",         kind: "console",  color: "#1d3b8b" },
    { key: "android",    match: ["android", ".apk"],            icon: "🤖", label: "Android",            kind: "mobile",   color: "#3ddc84" }
  ];
  var OTHER = { key: "other", icon: "📦", label: "Inne", kind: "pc", color: "#6b5a3e" };

  function platformFor(assetName) {
    var n = (assetName || "").toLowerCase();
    for (var i = 0; i < PLATFORMS.length; i++) {
      var p = PLATFORMS[i];
      for (var j = 0; j < p.match.length; j++) {
        if (n.indexOf(p.match[j]) !== -1) return p;
      }
    }
    return OTHER;
  }

  function el(id) { return document.getElementById(id); }

  function fmt(n) {
    // Polish thousands separator (non-breaking space).
    return String(n).replace(/\B(?=(\d{3})+(?!\d))/g, " ");
  }

  function aggregate(releases) {
    var perPlatform = {};
    var byKind = { pc: 0, handheld: 0, console: 0, mobile: 0 };
    var perRelease = [];
    var total = 0;
    var firstAssetDate = null;

    releases.forEach(function (rel) {
      if (rel.draft) return;
      var relTotal = 0;
      (rel.assets || []).forEach(function (a) {
        var c = a.download_count || 0;
        var p = platformFor(a.name);
        perPlatform[p.key] = perPlatform[p.key] || { meta: p, count: 0 };
        perPlatform[p.key].count += c;
        byKind[p.kind] += c;
        relTotal += c;
        total += c;
        if (a.created_at && (!firstAssetDate || a.created_at < firstAssetDate)) {
          firstAssetDate = a.created_at;
        }
      });
      perRelease.push({
        tag: rel.tag_name,
        url: rel.html_url,
        published: rel.published_at,
        prerelease: !!rel.prerelease,
        count: relTotal
      });
    });

    var platforms = [];
    [OTHER].concat(PLATFORMS).forEach(function (p) {
      var e = perPlatform[p.key];
      if (e && e.count > 0) platforms.push(e);
    });
    platforms.sort(function (a, b) { return b.count - a.count; });

    return {
      total: total,
      platforms: platforms,
      byKind: byKind,
      perRelease: perRelease,
      releaseCount: perRelease.length,
      latest: perRelease.length ? perRelease[0] : null,
      firstAssetDate: firstAssetDate
    };
  }

  // ---- rendering -----------------------------------------------------------

  var reduceMotion =
    window.matchMedia &&
    window.matchMedia("(prefers-reduced-motion: reduce)").matches;

  function countUp(node, to) {
    if (reduceMotion || to <= 0 || document.hidden) { node.textContent = fmt(to); return; }
    var dur = 900;
    var start = null;
    function frame(ts) {
      if (start === null) start = ts;
      var t = Math.min((ts - start) / dur, 1);
      var eased = 1 - Math.pow(1 - t, 3); // easeOutCubic
      node.textContent = fmt(Math.round(to * eased));
      if (t < 1) requestAnimationFrame(frame);
    }
    requestAnimationFrame(frame);
    // Safety net: rAF is throttled/paused in background tabs, so guarantee
    // the final value lands even if the animation never gets to run.
    setTimeout(function () { node.textContent = fmt(to); }, dur + 250);
  }

  // Apply on the next tick so the CSS width transition has a 0 → target
  // edge to animate. setTimeout (unlike rAF) still fires in background tabs.
  function applySoon(fn) { setTimeout(fn, 30); }

  function plReleases(n) {
    var t = n % 10, h = n % 100;
    if (n === 1) return "wydanie";
    if (t >= 2 && t <= 4 && (h < 10 || h >= 20)) return "wydania";
    return "wydań";
  }

  // `total` is the GRAND total — both the bar width and the % label are a
  // share of all downloads, so the platform/release bars add up to 100%.
  function bar(label, icon, count, total, color, href) {
    var pct = total > 0 ? (count / total) * 100 : 0;
    var pctTxt = count > 0 && pct < 1 ? "<1%" : Math.round(pct) + "%";
    // Floor non-zero shares to a small nub so tiny ones stay visible.
    var width = count > 0 ? Math.max(pct, 1.2) : 0;
    var row = document.createElement(href ? "a" : "div");
    row.className = "bar";
    if (href) { row.href = href; row.target = "_blank"; row.rel = "noopener"; }
    row.innerHTML =
      '<div class="bar__head">' +
        '<span class="bar__label">' +
          (icon ? '<span class="bar__icon">' + icon + "</span>" : "") +
          '<span class="bar__name"></span>' +
        "</span>" +
        '<span class="bar__val"><strong></strong> <span class="bar__pct"></span></span>' +
      "</div>" +
      '<div class="bar__track"><span class="bar__fill"></span></div>';
    row.querySelector(".bar__name").textContent = label;
    row.querySelector(".bar__val strong").textContent = fmt(count);
    row.querySelector(".bar__pct").textContent = "(" + pctTxt + ")";
    var fill = row.querySelector(".bar__fill");
    fill.style.background = color;
    applySoon(function () { fill.style.width = width + "%"; });
    return row;
  }

  function render(data) {
    el("stats-state").hidden = true;
    el("stats-body").hidden = false;

    // hero total
    countUp(el("total-num"), data.total);

    // quick stat cards
    el("sc-total").textContent = fmt(data.total);
    el("sc-releases-num").textContent = fmt(data.releaseCount);
    el("sc-releases-word").textContent = plReleases(data.releaseCount);
    el("sc-latest").textContent = data.latest ? data.latest.tag : "—";
    if (data.platforms.length) {
      var top = data.platforms[0];
      el("sc-top-icon").textContent = top.meta.icon;
      el("sc-top").textContent = top.meta.label;
    }

    // platform bars — width & % are a share of the grand total
    var pHost = el("platform-bars");
    pHost.innerHTML = "";
    data.platforms.forEach(function (p) {
      pHost.appendChild(
        bar(p.meta.label, p.meta.icon, p.count, data.total, p.meta.color, null)
      );
    });

    // desktop vs handheld split (other kinds — consoles like PS2, Android —
    // show in the per-platform bars + grand total, but not this binary
    // PC↔handheld widget)
    var pc = data.byKind.pc, hh = data.byKind.handheld, sum = pc + hh;
    var pcPct = sum ? Math.round((pc / sum) * 100) : 0;
    var hhPct = sum ? 100 - pcPct : 0;
    el("split-pc-num").textContent = fmt(pc);
    el("split-hh-num").textContent = fmt(hh);
    el("split-pc-pct").textContent = pcPct + "%";
    el("split-hh-pct").textContent = hhPct + "%";
    applySoon(function () {
      el("split-pc-fill").style.width = pcPct + "%";
      el("split-hh-fill").style.width = hhPct + "%";
    });

    // per-release bars (already latest-first from API) — share of grand total
    var rHost = el("release-bars");
    rHost.innerHTML = "";
    data.perRelease.forEach(function (r) {
      var label = r.tag + (r.prerelease ? "  ·  pre" : "");
      rHost.appendChild(bar(label, null, r.count, data.total, "#1d1608", r.url));
    });

    // footer note timestamp
    var stamp = new Date();
    var hh2 = String(stamp.getHours()).padStart(2, "0");
    var mm2 = String(stamp.getMinutes()).padStart(2, "0");
    el("stats-stamp").textContent = hh2 + ":" + mm2;
  }

  function showError() {
    var s = el("stats-state");
    s.hidden = false;
    s.className = "stats-state stats-state--err";
    s.innerHTML =
      '<p class="stats-state__big">🛰️ Brak połączenia z GitHubem</p>' +
      "<p>Nie udało się pobrać statystyk (możliwy limit zapytań GitHub API — " +
      "spróbuj za chwilę). Pełne dane zawsze są na " +
      '<a href="https://github.com/' + REPO + '/releases" target="_blank" rel="noopener">' +
      "stronie wydań</a>.</p>";
  }

  function readCache() {
    try {
      var raw = localStorage.getItem(CACHE_KEY);
      if (!raw) return null;
      return JSON.parse(raw);
    } catch (e) { return null; }
  }
  function writeCache(data) {
    try {
      localStorage.setItem(CACHE_KEY, JSON.stringify({ t: Date.now(), data: data }));
    } catch (e) { /* private mode / quota — ignore */ }
  }

  function load() {
    // Instant paint from fresh-enough cache, then refresh in background.
    var cached = readCache();
    if (cached && cached.data) {
      render(cached.data);
      if (Date.now() - cached.t < CACHE_TTL_MS) return; // still fresh, skip refetch
    }

    fetch(API, { headers: { Accept: "application/vnd.github+json" } })
      .then(function (r) {
        if (!r.ok) throw new Error("HTTP " + r.status);
        return r.json();
      })
      .then(function (releases) {
        if (!Array.isArray(releases)) throw new Error("bad payload");
        var data = aggregate(releases);
        writeCache(data);
        render(data);
      })
      .catch(function () {
        if (!(cached && cached.data)) showError();
      });
  }

  load();
})();
