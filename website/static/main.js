(function () {
  "use strict";

  function detectPlatform() {
    var ua = navigator.userAgent || "";
    var p = (navigator.userAgentData && navigator.userAgentData.platform) ||
            navigator.platform || "";
    var s = (ua + " " + p).toLowerCase();

    if (/android/.test(s)) return "android";
    if (/iphone|ipad|ipod/.test(s)) return null;
    if (/win/.test(s)) return "windows";
    if (/mac/.test(s)) return "mac";
    if (/linux|x11|cros/.test(s)) return "linux";
    return null;
  }

  var PLATFORM_LABEL = {
    windows: "Windows",
    mac: "macOS",
    linux: "Linux",
    android: "Android"
  };

  var platform = detectPlatform();

  if (platform) {
    var card = document.querySelector('.card[data-platform="' + platform + '"]');
    if (card) card.classList.add("card--recommended");

    var heroBtn = document.getElementById("hero-download");
    if (heroBtn && PLATFORM_LABEL[platform]) {
      heroBtn.textContent = "Pobierz dla " + PLATFORM_LABEL[platform];
    }
  }

  var burger = document.querySelector(".nav__burger");
  var links = document.querySelector(".nav__links");
  if (burger && links) {
    burger.addEventListener("click", function () {
      var open = links.classList.toggle("open");
      burger.setAttribute("aria-expanded", open ? "true" : "false");
    });
    links.addEventListener("click", function (e) {
      if (e.target.tagName === "A") {
        links.classList.remove("open");
        burger.setAttribute("aria-expanded", "false");
      }
    });
  }

  var clock = document.getElementById("clock");
  if (clock) {
    var tick = function () {
      var d = new Date();
      var hh = String(d.getHours()).padStart(2, "0");
      var mm = String(d.getMinutes()).padStart(2, "0");
      clock.textContent = hh + ":" + mm;
    };
    tick();
    setInterval(tick, 15000);
  }

  var toTop = document.querySelector(".totop");
  if (toTop) {
    var onScroll = function () {
      if (window.scrollY > 600) toTop.classList.add("show");
      else toTop.classList.remove("show");
    };
    window.addEventListener("scroll", onScroll, { passive: true });
    onScroll();
  }
})();

(function () {
  "use strict";
  if (!window.matchMedia || !window.matchMedia("(hover: hover) and (pointer: fine)").matches) return;

  var img = new Image();
  img.onload = init;
  img.src = "assets/cursor-pencil-sheet.png";

  function init() {
    var cur = document.createElement("div");
    cur.className = "cursor";
    cur.setAttribute("aria-hidden", "true");
    document.body.appendChild(cur);
    document.body.classList.add("has-cursor");

    var INTERACTIVE = "a, button, summary, .btn, .card, label, [role='button']";
    var shown = false;

    window.addEventListener("pointermove", function (e) {
      if (e.pointerType && e.pointerType !== "mouse") return;
      cur.style.transform = "translate(" + e.clientX + "px," + e.clientY + "px)";
      if (!shown) { cur.style.display = "block"; shown = true; }
      var hot = e.target && e.target.closest && e.target.closest(INTERACTIVE);
      cur.classList.toggle("is-anim", !!hot);
    }, { passive: true });

    document.addEventListener("mouseleave", hide);
    window.addEventListener("blur", hide);
    function hide() { cur.style.display = "none"; shown = false; }
  }
})();
