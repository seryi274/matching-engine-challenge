/* ==========================================================
   reveal.js -- Final ceremony reveal page
   Keyboard-controlled presentation for the awards ceremony
   ========================================================== */

(function () {
  'use strict';

  // ---- Configuration --------------------------------------
  var API_URL = '/api/leaderboard';

  // ---- State ----------------------------------------------
  var slides = [];          // Array of slide objects
  var currentSlide = -1;    // Current slide index (-1 = not started)
  var leaderboardData = null;
  var isAnimating = false;

  // ---- DOM refs -------------------------------------------
  var slideContainer = null;
  var progressText = null;
  var progressFill = null;
  var progressHint = null;

  // ---- Initialization -------------------------------------
  document.addEventListener('DOMContentLoaded', function () {
    slideContainer = document.getElementById('reveal-content');
    progressText = document.getElementById('reveal-progress-text');
    progressFill = document.getElementById('reveal-progress-fill');
    progressHint = document.getElementById('reveal-progress-hint');

    loadData();
    bindKeys();
  });

  // ---- Load data ------------------------------------------
  function loadData() {
    showMessage('Loading leaderboard data...');

    fetch(API_URL)
      .then(function (res) {
        if (!res.ok) throw new Error('HTTP ' + res.status);
        return res.json();
      })
      .then(function (data) {
        if (Array.isArray(data)) data = { teams: data };
        if (!data.teams || data.teams.length === 0) {
          showMessage('No teams found. Submit some benchmarks first!');
          return;
        }

        // Sort by throughput descending
        data.teams.sort(function (a, b) {
          return b.throughput - a.throughput;
        });

        leaderboardData = data;
        buildSlides(data);
        showMessage('Ready. Press SPACE to begin.');
        updateProgress();
      })
      .catch(function (err) {
        showMessage('Failed to load data: ' + err.message);
      });
  }

  // ---- Build slides array ---------------------------------
  function buildSlides(data) {
    slides = [];
    var teams = data.teams;
    var totalSubmissions = 0;
    var totalOrders = 0;
    teams.forEach(function (t) {
      totalSubmissions += (t.submissions || 0);
      totalOrders += (t.throughput || 0) * (t.submissions || 1);
    });

    // Slide 0: Opening stats
    slides.push({
      type: 'opening',
      totalTeams: teams.length,
      totalSubmissions: totalSubmissions,
      totalOrders: Math.round(totalOrders)
    });

    // Slides 1..N-3: Teams from last to 4th place
    for (var i = teams.length - 1; i >= 3; i--) {
      slides.push({
        type: 'team',
        team: teams[i],
        rank: i + 1,
        totalTeams: teams.length,
        isTop3: false
      });
    }

    // Top 3 (bronze, silver, gold) -- if enough teams
    if (teams.length >= 3) {
      slides.push({
        type: 'team',
        team: teams[2],
        rank: 3,
        totalTeams: teams.length,
        isTop3: true,
        medal: 'bronze'
      });
    }

    if (teams.length >= 2) {
      slides.push({
        type: 'team',
        team: teams[1],
        rank: 2,
        totalTeams: teams.length,
        isTop3: true,
        medal: 'silver'
      });
    }

    if (teams.length >= 1) {
      slides.push({
        type: 'team',
        team: teams[0],
        rank: 1,
        totalTeams: teams.length,
        isTop3: true,
        medal: 'gold'
      });
    }
  }

  // ---- Keyboard controls ----------------------------------
  function bindKeys() {
    document.addEventListener('keydown', function (e) {
      if (isAnimating) return;

      switch (e.key) {
        case ' ':
        case 'ArrowRight':
        case 'Enter':
          e.preventDefault();
          advanceSlide();
          break;
        case 'ArrowLeft':
        case 'Backspace':
          e.preventDefault();
          retreatSlide();
          break;
        case 'Home':
          e.preventDefault();
          goToSlide(0);
          break;
        case 'End':
          e.preventDefault();
          goToSlide(slides.length - 1);
          break;
      }
    });
  }

  function advanceSlide() {
    if (currentSlide < slides.length - 1) {
      goToSlide(currentSlide + 1);
    }
  }

  function retreatSlide() {
    if (currentSlide > 0) {
      goToSlide(currentSlide - 1);
    }
  }

  function goToSlide(index) {
    if (index < 0 || index >= slides.length) return;
    isAnimating = true;
    currentSlide = index;

    var slide = slides[index];
    renderSlide(slide);
    updateProgress();

    setTimeout(function () {
      isAnimating = false;
    }, 700);
  }

  // ---- Render slides --------------------------------------
  function renderSlide(slide) {
    if (!slideContainer) return;

    switch (slide.type) {
      case 'opening':
        renderOpening(slide);
        break;
      case 'team':
        renderTeam(slide);
        break;
    }
  }

  function renderOpening(slide) {
    var html = '<div class="reveal-slide animate-fade-in">' +
      '<h1 class="reveal-heading">Matching Engine Challenge</h1>' +
      '<p class="reveal-subheading">Final Results</p>' +
      '<div class="reveal-stats-grid">' +
        '<div class="reveal-stat delay-1 animate-scale-in">' +
          '<div class="reveal-stat__value" data-countup="' + slide.totalTeams + '">0</div>' +
          '<div class="reveal-stat__label">Teams Competed</div>' +
        '</div>' +
        '<div class="reveal-stat delay-2 animate-scale-in">' +
          '<div class="reveal-stat__value" data-countup="' + slide.totalSubmissions + '">0</div>' +
          '<div class="reveal-stat__label">Total Submissions</div>' +
        '</div>' +
        '<div class="reveal-stat delay-3 animate-scale-in">' +
          '<div class="reveal-stat__value" data-countup="' + slide.totalOrders + '">0</div>' +
          '<div class="reveal-stat__label">Orders Processed</div>' +
        '</div>' +
      '</div>' +
    '</div>';

    slideContainer.innerHTML = html;
    startCountUpAnimations();
  }

  function renderTeam(slide) {
    var team = slide.team;
    var rank = slide.rank;
    var topClass = '';
    var trophyIcon = '';

    if (slide.isTop3) {
      topClass = ' top-3 top-' + rank;
      if (rank === 1) trophyIcon = '\uD83C\uDFC6';       // trophy
      else if (rank === 2) trophyIcon = '\uD83E\uDD48';   // 2nd place medal
      else if (rank === 3) trophyIcon = '\uD83E\uDD49';   // 3rd place medal
    }

    var ordinalSuffix = getOrdinalSuffix(rank);

    var html = '<div class="reveal-slide">' +
      '<div class="reveal-team-card' + topClass + '">' +
        '<div class="reveal-trophy">' + trophyIcon + '</div>' +
        '<div class="reveal-team-rank animate-fade-in">' + rank + ordinalSuffix + ' Place</div>' +
        '<div class="reveal-team-name animate-slide-in">' + escapeHtml(team.team_name) + '</div>' +
        '<div class="reveal-team-metrics">' +
          '<div class="reveal-metric reveal-metric--throughput delay-1 animate-scale-in">' +
            '<div class="reveal-metric__value" data-countup="' + Math.round(team.throughput) + '">0</div>' +
            '<div class="reveal-metric__label">Throughput (ops/sec)</div>' +
          '</div>' +
          '<div class="reveal-metric reveal-metric--latency delay-2 animate-scale-in">' +
            '<div class="reveal-metric__value" data-countup="' + Math.round(team.avg_latency) + '">0</div>' +
            '<div class="reveal-metric__label">Avg Latency (ns)</div>' +
          '</div>' +
          '<div class="reveal-metric reveal-metric--latency delay-3 animate-scale-in">' +
            '<div class="reveal-metric__value" data-countup="' + Math.round(team.p99_latency) + '">0</div>' +
            '<div class="reveal-metric__label">P99 Latency (ns)</div>' +
          '</div>' +
        '</div>' +
        '<div class="reveal-submissions delay-4 animate-fade-in">' +
          '<span>' + (team.submissions || 0) + '</span> submissions' +
        '</div>' +
      '</div>' +
    '</div>';

    slideContainer.innerHTML = html;

    // Trigger the active state for card animation
    requestAnimationFrame(function () {
      var card = slideContainer.querySelector('.reveal-team-card');
      if (card) card.classList.add('active');
    });

    startCountUpAnimations();

    // Fire confetti for winner
    if (rank === 1 && typeof confetti === 'function') {
      setTimeout(function () {
        fireWinnerConfetti();
      }, 600);
    }
  }

  // ---- Count-up animation ---------------------------------
  function startCountUpAnimations() {
    var elements = slideContainer.querySelectorAll('[data-countup]');
    for (var i = 0; i < elements.length; i++) {
      animateCountUp(elements[i]);
    }
  }

  function animateCountUp(el) {
    var target = parseInt(el.getAttribute('data-countup'), 10);
    if (isNaN(target)) {
      el.textContent = '0';
      return;
    }

    var duration = 1500; // ms
    var startTime = null;
    var startVal = 0;

    // Delay based on CSS animation delay class
    var delay = 0;
    var parent = el.closest('[class*="delay-"]');
    if (parent) {
      if (parent.classList.contains('delay-1')) delay = 150;
      if (parent.classList.contains('delay-2')) delay = 300;
      if (parent.classList.contains('delay-3')) delay = 450;
      if (parent.classList.contains('delay-4')) delay = 600;
    }

    setTimeout(function () {
      requestAnimationFrame(step);
    }, delay);

    function step(timestamp) {
      if (!startTime) startTime = timestamp;
      var elapsed = timestamp - startTime;
      var progress = Math.min(elapsed / duration, 1);

      // Ease out cubic
      var eased = 1 - Math.pow(1 - progress, 3);
      var current = Math.round(startVal + (target - startVal) * eased);

      el.textContent = current.toLocaleString();

      if (progress < 1) {
        requestAnimationFrame(step);
      } else {
        el.textContent = target.toLocaleString();
      }
    }
  }

  // ---- Confetti -------------------------------------------
  function fireWinnerConfetti() {
    if (typeof confetti !== 'function') return;

    // Initial burst
    confetti({
      particleCount: 150,
      spread: 100,
      origin: { y: 0.6 },
      colors: ['#ffd700', '#ffb300', '#fff4b0', '#ffffff']
    });

    // Side cannons
    setTimeout(function () {
      confetti({
        particleCount: 80,
        angle: 60,
        spread: 55,
        origin: { x: 0, y: 0.65 },
        colors: ['#ffd700', '#ffb300', '#fff4b0']
      });
      confetti({
        particleCount: 80,
        angle: 120,
        spread: 55,
        origin: { x: 1, y: 0.65 },
        colors: ['#ffd700', '#ffb300', '#fff4b0']
      });
    }, 300);

    // Sustained gentle rain
    var end = Date.now() + 4000;
    (function frame() {
      confetti({
        particleCount: 3,
        angle: 60,
        spread: 55,
        origin: { x: 0, y: 0.5 },
        colors: ['#ffd700', '#c0c0c0', '#cd7f32']
      });
      confetti({
        particleCount: 3,
        angle: 120,
        spread: 55,
        origin: { x: 1, y: 0.5 },
        colors: ['#ffd700', '#c0c0c0', '#cd7f32']
      });
      if (Date.now() < end) {
        requestAnimationFrame(frame);
      }
    })();
  }

  // ---- Progress indicator ---------------------------------
  function updateProgress() {
    if (!progressText || !progressFill) return;

    if (slides.length === 0) {
      progressText.textContent = '';
      progressFill.style.width = '0%';
      return;
    }

    if (currentSlide < 0) {
      progressText.textContent = 'Press SPACE to begin';
      progressFill.style.width = '0%';
      return;
    }

    var current = currentSlide + 1;
    var total = slides.length;

    var slide = slides[currentSlide];
    if (slide.type === 'opening') {
      progressText.textContent = 'Opening';
    } else if (slide.type === 'team') {
      var teamsTotal = leaderboardData ? leaderboardData.teams.length : '?';
      // Count how many team slides we've shown
      var teamSlideIndex = 0;
      for (var i = 0; i <= currentSlide; i++) {
        if (slides[i].type === 'team') teamSlideIndex++;
      }
      var totalTeamSlides = 0;
      for (var j = 0; j < slides.length; j++) {
        if (slides[j].type === 'team') totalTeamSlides++;
      }
      progressText.textContent = 'Team ' + teamSlideIndex + ' of ' + totalTeamSlides;
    }

    var pct = (current / total) * 100;
    progressFill.style.width = pct + '%';

    if (progressHint) {
      if (currentSlide >= slides.length - 1) {
        progressHint.textContent = 'End of presentation';
      } else {
        progressHint.textContent = 'SPACE / \u2192 to advance';
      }
    }
  }

  // ---- Helpers --------------------------------------------
  function showMessage(msg) {
    if (slideContainer) {
      slideContainer.innerHTML =
        '<div class="reveal-slide animate-fade-in">' +
        '<p class="reveal-subheading">' + escapeHtml(msg) + '</p>' +
        '</div>';
    }
  }

  function getOrdinalSuffix(n) {
    var s = ['th', 'st', 'nd', 'rd'];
    var v = n % 100;
    return (s[(v - 20) % 10] || s[v] || s[0]);
  }

  function escapeHtml(str) {
    if (!str) return '';
    var div = document.createElement('div');
    div.appendChild(document.createTextNode(str));
    return div.innerHTML;
  }

})();
