/* ==========================================================
   charts.js -- Chart.js visualizations for the leaderboard
   Uses Chart.js 4.x API
   ========================================================== */

(function () {
  'use strict';

  // ---- Shared dark-theme defaults -------------------------
  const GRID_COLOR = 'rgba(255, 255, 255, 0.06)';
  const LABEL_COLOR = '#9ca3ab';
  const FONT_FAMILY = "'Indivisible', 'Inter', sans-serif";
  const MONO_FONT = "'Indivisible', 'JetBrains Mono', monospace";

  Chart.defaults.color = LABEL_COLOR;
  Chart.defaults.font.family = FONT_FAMILY;
  Chart.defaults.font.size = 13;

  // ---- Color palette (G-Research themed) -------------------
  function barColor(index, total) {
    if (index === 0) return '#ffd700';         // gold
    if (index === 1) return '#c0c0c0';         // silver
    if (index === 2) return '#cd7f32';         // bronze
    // Gradient from white to muted gray for the rest
    var t = total > 3 ? (index - 3) / (total - 3) : 0;
    var v = Math.round(220 - t * 100);
    return 'rgb(' + v + ',' + v + ',' + v + ')';
  }

  // ---- Chart instances (will be created lazily) -----------
  var throughputChart = null;
  var latencyChart = null;

  // ---- Create / update throughput chart -------------------
  function buildThroughputChart(ctx, teams) {
    var labels = teams.map(function (t) { return t.team_name; });
    var data = teams.map(function (t) { return t.throughput; });
    var colors = teams.map(function (_, i) { return barColor(i, teams.length); });

    return new Chart(ctx, {
      type: 'bar',
      data: {
        labels: labels,
        datasets: [{
          label: 'Throughput (ops/sec)',
          data: data,
          backgroundColor: colors,
          borderColor: colors.map(function (c) { return c; }),
          borderWidth: 1,
          borderRadius: 4,
          barPercentage: 0.7
        }]
      },
      options: {
        indexAxis: 'y',
        responsive: true,
        maintainAspectRatio: false,
        plugins: {
          legend: { display: false },
          tooltip: {
            backgroundColor: '#2a2d31',
            titleColor: '#f0f2f4',
            bodyColor: '#f0f2f4',
            borderColor: '#383c42',
            borderWidth: 1,
            titleFont: { family: FONT_FAMILY, weight: '600' },
            bodyFont: { family: MONO_FONT },
            callbacks: {
              label: function (ctx) {
                return '  ' + Number(ctx.raw).toLocaleString() + ' ops/sec';
              }
            }
          }
        },
        scales: {
          x: {
            grid: { color: GRID_COLOR },
            ticks: {
              color: LABEL_COLOR,
              font: { family: MONO_FONT, size: 12 },
              callback: function (v) {
                if (v >= 1000000) return (v / 1000000).toFixed(1) + 'M';
                if (v >= 1000) return (v / 1000).toFixed(0) + 'K';
                return v;
              }
            },
            title: {
              display: true,
              text: 'Operations / second',
              color: LABEL_COLOR,
              font: { size: 12 }
            }
          },
          y: {
            grid: { display: false },
            ticks: {
              color: '#e6edf3',
              font: { family: FONT_FAMILY, size: 13, weight: '500' }
            }
          }
        },
        animation: {
          duration: 800,
          easing: 'easeOutQuart'
        }
      }
    });
  }

  function buildLatencyChart(ctx, teams) {
    var labels = teams.map(function (t) { return t.team_name; });
    var p50Data = teams.map(function (t) { return t.avg_latency; });
    var p99Data = teams.map(function (t) { return t.p99_latency; });

    return new Chart(ctx, {
      type: 'bar',
      data: {
        labels: labels,
        datasets: [
          {
            label: 'Avg Latency (ns)',
            data: p50Data,
            backgroundColor: 'rgba(255, 255, 255, 0.75)',
            borderColor: '#ffffff',
            borderWidth: 1,
            borderRadius: 3,
            barPercentage: 0.8,
            categoryPercentage: 0.6
          },
          {
            label: 'P99 Latency (ns)',
            data: p99Data,
            backgroundColor: 'rgba(156, 163, 171, 0.5)',
            borderColor: '#9ca3ab',
            borderWidth: 1,
            borderRadius: 3,
            barPercentage: 0.8,
            categoryPercentage: 0.6
          }
        ]
      },
      options: {
        responsive: true,
        maintainAspectRatio: false,
        plugins: {
          legend: {
            position: 'top',
            labels: {
              color: LABEL_COLOR,
              usePointStyle: true,
              pointStyle: 'rectRounded',
              padding: 16,
              font: { size: 12 }
            }
          },
          tooltip: {
            backgroundColor: '#2a2d31',
            titleColor: '#f0f2f4',
            bodyColor: '#f0f2f4',
            borderColor: '#383c42',
            borderWidth: 1,
            titleFont: { family: FONT_FAMILY, weight: '600' },
            bodyFont: { family: MONO_FONT },
            callbacks: {
              label: function (ctx) {
                return '  ' + ctx.dataset.label + ': ' + Number(ctx.raw).toLocaleString() + ' ns';
              }
            }
          }
        },
        scales: {
          x: {
            grid: { display: false },
            ticks: {
              color: '#e6edf3',
              font: { family: FONT_FAMILY, size: 12 },
              maxRotation: 45,
              minRotation: 0
            }
          },
          y: {
            grid: { color: GRID_COLOR },
            ticks: {
              color: LABEL_COLOR,
              font: { family: MONO_FONT, size: 12 },
              callback: function (v) {
                if (v >= 1000000) return (v / 1000000).toFixed(1) + 'M';
                if (v >= 1000) return (v / 1000).toFixed(0) + 'K';
                return v;
              }
            },
            title: {
              display: true,
              text: 'Latency (nanoseconds)',
              color: LABEL_COLOR,
              font: { size: 12 }
            }
          }
        },
        animation: {
          duration: 800,
          easing: 'easeOutQuart'
        }
      }
    });
  }

  // ---- Public API -----------------------------------------

  /**
   * updateCharts(data)
   * @param {Object} data - Leaderboard API response
   *   data.teams: Array of { team_name, throughput, avg_latency, p99_latency, ... }
   *
   * Creates charts on first call, updates data on subsequent calls.
   */
  function updateCharts(data) {
    if (!data || !data.teams || data.teams.length === 0) return;

    // Sort by throughput descending for the throughput chart
    var teamsByThroughput = data.teams.slice().sort(function (a, b) {
      return b.throughput - a.throughput;
    });

    // Sort by avg latency ascending for the latency chart (best first)
    var teamsByLatency = data.teams.slice().sort(function (a, b) {
      return a.avg_latency - b.avg_latency;
    });

    var throughputCtx = document.getElementById('chart-throughput');
    var latencyCtx = document.getElementById('chart-latency');

    if (!throughputCtx || !latencyCtx) return;

    // Throughput chart
    if (throughputChart) {
      var tLabels = teamsByThroughput.map(function (t) { return t.team_name; });
      var tData = teamsByThroughput.map(function (t) { return t.throughput; });
      var tColors = teamsByThroughput.map(function (_, i) {
        return barColor(i, teamsByThroughput.length);
      });

      throughputChart.data.labels = tLabels;
      throughputChart.data.datasets[0].data = tData;
      throughputChart.data.datasets[0].backgroundColor = tColors;
      throughputChart.data.datasets[0].borderColor = tColors;
      throughputChart.update('active');
    } else {
      throughputChart = buildThroughputChart(
        throughputCtx.getContext('2d'),
        teamsByThroughput
      );
    }

    // Latency chart
    if (latencyChart) {
      var lLabels = teamsByLatency.map(function (t) { return t.team_name; });
      var lP50 = teamsByLatency.map(function (t) { return t.avg_latency; });
      var lP99 = teamsByLatency.map(function (t) { return t.p99_latency; });

      latencyChart.data.labels = lLabels;
      latencyChart.data.datasets[0].data = lP50;
      latencyChart.data.datasets[1].data = lP99;
      latencyChart.update('active');
    } else {
      latencyChart = buildLatencyChart(
        latencyCtx.getContext('2d'),
        teamsByLatency
      );
    }
  }

  // Expose globally
  window.updateCharts = updateCharts;
})();
