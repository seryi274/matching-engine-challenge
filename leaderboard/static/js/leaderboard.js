/* ==========================================================
   leaderboard.js -- Main leaderboard data fetching & rendering
   ========================================================== */

(function () {
  'use strict';

  // ---- Configuration --------------------------------------
  var POLL_INTERVAL = 10000;          // 10 seconds
  var API_URL = '/api/leaderboard';
  var SSE_URL = '/api/stream';

  // ---- State ----------------------------------------------
  var currentData = null;
  var previousDataMap = {};           // team_name -> previous row data (for diff)
  var activityLog = [];               // recent submission events for ticker
  var MAX_ACTIVITY = 50;
  var sseRetryDelay = 1000;
  var sseSource = null;
  var pollTimer = null;

  // ---- DOM refs -------------------------------------------
  var tableBody = null;
  var totalTeamsEl = null;
  var totalSubmissionsEl = null;
  var lastRefreshEl = null;
  var tickerContent = null;
  var connectionDot = null;
  var loadingEl = null;

  // ---- Initialization -------------------------------------
  document.addEventListener('DOMContentLoaded', function () {
    tableBody = document.getElementById('leaderboard-body');
    totalTeamsEl = document.getElementById('stat-teams');
    totalSubmissionsEl = document.getElementById('stat-submissions');
    lastRefreshEl = document.getElementById('stat-refresh');
    tickerContent = document.getElementById('ticker-content');
    connectionDot = document.getElementById('connection-dot');
    loadingEl = document.getElementById('loading-state');

    fetchData();
    startPolling();
    connectSSE();
  });

  // ---- Polling --------------------------------------------
  function startPolling() {
    if (pollTimer) clearInterval(pollTimer);
    pollTimer = setInterval(fetchData, POLL_INTERVAL);
  }

  function fetchData() {
    fetch(API_URL)
      .then(function (res) {
        if (!res.ok) throw new Error('HTTP ' + res.status);
        return res.json();
      })
      .then(function (data) {
        handleData(data);
      })
      .catch(function (err) {
        console.warn('[leaderboard] fetch error:', err);
      });
  }

  // ---- Server-Sent Events ---------------------------------
  function connectSSE() {
    if (sseSource) {
      sseSource.close();
    }

    sseSource = new EventSource(SSE_URL);

    sseSource.onopen = function () {
      sseRetryDelay = 1000;
      setConnected(true);
    };

    sseSource.onmessage = function (event) {
      try {
        var data = JSON.parse(event.data);
        handleData(data);
      } catch (e) {
        console.warn('[leaderboard] SSE parse error:', e);
      }
    };

    sseSource.addEventListener('submission', function (event) {
      try {
        var submission = JSON.parse(event.data);
        addActivity(submission);
      } catch (e) {
        // ignore
      }
    });

    sseSource.onerror = function () {
      setConnected(false);
      sseSource.close();
      // Exponential backoff reconnect
      setTimeout(function () {
        sseRetryDelay = Math.min(sseRetryDelay * 2, 30000);
        connectSSE();
      }, sseRetryDelay);
    };
  }

  function setConnected(connected) {
    if (connectionDot) {
      connectionDot.className = 'connection-dot' +
        (connected ? ' connection-dot--connected' : ' connection-dot--disconnected');
    }
  }

  // ---- Data handling --------------------------------------
  function handleData(data) {
    if (!data) return;

    // Normalize: support both { teams: [...] } and raw array
    if (Array.isArray(data)) {
      data = { teams: data };
    }
    if (!data.teams) return;

    // Sort by throughput descending (primary ranking)
    data.teams.sort(function (a, b) {
      return b.throughput - a.throughput;
    });

    // Assign ranks
    data.teams.forEach(function (team, idx) {
      team._rank = idx + 1;
    });

    // Detect changes for animation
    var changedTeams = {};
    data.teams.forEach(function (team) {
      var prev = previousDataMap[team.team_name];
      if (!prev) {
        changedTeams[team.team_name] = 'new';
      } else if (
        prev.throughput !== team.throughput ||
        prev.avg_latency !== team.avg_latency ||
        prev.p99_latency !== team.p99_latency ||
        prev.submissions !== team.submissions
      ) {
        changedTeams[team.team_name] = 'updated';
        // Add to activity log
        addActivity({
          team_name: team.team_name,
          throughput: team.throughput,
          timestamp: team.last_updated || new Date().toISOString()
        });
      }
    });

    // Update previous data map
    previousDataMap = {};
    data.teams.forEach(function (team) {
      previousDataMap[team.team_name] = Object.assign({}, team);
    });

    currentData = data;
    renderTable(data, changedTeams);
    renderStats(data);
    renderTicker();

    // Update charts (from charts.js)
    if (typeof window.updateCharts === 'function') {
      window.updateCharts(data);
    }

    // Hide loading state
    if (loadingEl) {
      loadingEl.style.display = 'none';
    }
  }

  // ---- Render Table ---------------------------------------
  function renderTable(data, changedTeams) {
    if (!tableBody) return;

    var html = '';

    if (data.teams.length === 0) {
      html = '<tr><td colspan="7" class="empty-message">' +
        'No submissions yet. Waiting for teams...' +
        '</td></tr>';
      tableBody.innerHTML = html;
      return;
    }

    data.teams.forEach(function (team) {
      var rank = team._rank;
      var rankClass = rank <= 3 ? ' rank-' + rank : '';
      var changeClass = '';
      if (changedTeams[team.team_name] === 'new') {
        changeClass = ' row-new';
      } else if (changedTeams[team.team_name] === 'updated') {
        changeClass = ' row-updated';
      }

      var badgeClass = rank <= 3 ? 'rank-badge--' + rank : 'rank-badge--other';

      html += '<tr class="' + rankClass + changeClass + '" data-team="' + escapeHtml(team.team_name) + '">';

      // Rank
      html += '<td><span class="rank-badge ' + badgeClass + '">' + rank + '</span></td>';

      // Team name
      html += '<td><span class="team-name">' + escapeHtml(team.team_name) + '</span></td>';

      // Throughput
      html += '<td class="col-number"><span class="metric-value metric-value--throughput">' +
        formatNumber(team.throughput) + '</span></td>';

      // Avg Latency
      html += '<td class="col-number"><span class="metric-value metric-value--latency">' +
        formatNumber(team.avg_latency) + '</span></td>';

      // P99 Latency
      html += '<td class="col-number"><span class="metric-value metric-value--p99">' +
        formatNumber(team.p99_latency) + '</span></td>';

      // Submissions
      html += '<td class="col-number"><span class="metric-value metric-value--submissions">' +
        (team.submissions || 0) + '</span></td>';

      // Last Updated
      html += '<td><span class="metric-value metric-value--timestamp">' +
        formatTimestamp(team.last_updated) + '</span></td>';

      html += '</tr>';
    });

    tableBody.innerHTML = html;

    // Remove animation classes after animation completes
    setTimeout(function () {
      var animated = tableBody.querySelectorAll('.row-updated, .row-new');
      for (var i = 0; i < animated.length; i++) {
        animated[i].classList.remove('row-updated', 'row-new');
      }
    }, 2000);
  }

  // ---- Render Header Stats --------------------------------
  function renderStats(data) {
    var totalTeams = data.teams.length;
    var totalSubmissions = 0;
    data.teams.forEach(function (t) {
      totalSubmissions += (t.submissions || 0);
    });

    if (totalTeamsEl) totalTeamsEl.textContent = totalTeams;
    if (totalSubmissionsEl) totalSubmissionsEl.textContent = totalSubmissions;
    if (lastRefreshEl) lastRefreshEl.textContent = new Date().toLocaleTimeString();
  }

  // ---- Activity Ticker ------------------------------------
  function addActivity(item) {
    if (!item || !item.team_name) return;
    activityLog.unshift({
      team_name: item.team_name,
      throughput: item.throughput || 0,
      timestamp: item.timestamp || new Date().toISOString()
    });
    if (activityLog.length > MAX_ACTIVITY) {
      activityLog.length = MAX_ACTIVITY;
    }
  }

  function renderTicker() {
    if (!tickerContent) return;
    if (activityLog.length === 0) {
      tickerContent.innerHTML =
        '<span class="ticker-item">Waiting for submissions...</span>';
      return;
    }

    var html = '';
    activityLog.forEach(function (item) {
      html += '<span class="ticker-item">' +
        '<span class="ticker-item__team">' + escapeHtml(item.team_name) + '</span>' +
        ' submitted ' +
        '<span class="ticker-item__metric">' + formatNumber(item.throughput) + ' ops/s</span>' +
        ' <span class="ticker-item__time">' + formatTimestamp(item.timestamp) + '</span>' +
        '</span>';
    });

    // Duplicate for seamless scroll
    tickerContent.innerHTML = html + html;
  }

  // ---- Formatting helpers ---------------------------------
  function formatNumber(n) {
    if (n === undefined || n === null) return '-';
    return Number(n).toLocaleString(undefined, { maximumFractionDigits: 0 });
  }

  function formatTimestamp(ts) {
    if (!ts) return '-';
    try {
      var d = new Date(ts);
      if (isNaN(d.getTime())) return ts;
      var now = new Date();
      var diffMs = now - d;
      var diffSec = Math.floor(diffMs / 1000);
      if (diffSec < 60) return diffSec + 's ago';
      var diffMin = Math.floor(diffSec / 60);
      if (diffMin < 60) return diffMin + 'm ago';
      var diffHr = Math.floor(diffMin / 60);
      if (diffHr < 24) return diffHr + 'h ago';
      return d.toLocaleDateString();
    } catch (e) {
      return ts;
    }
  }

  function escapeHtml(str) {
    if (!str) return '';
    var div = document.createElement('div');
    div.appendChild(document.createTextNode(str));
    return div.innerHTML;
  }

})();
