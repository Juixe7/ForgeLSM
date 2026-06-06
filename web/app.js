/* ═══════════════════════════════════════════════════════════════
   ForgeLSM Engine Introspector — Dashboard JavaScript
   Real-time metrics polling · LSM visualization · Console · Benchmarks
   ═══════════════════════════════════════════════════════════════ */

'use strict';

// ── Constants ─────────────────────────────────────────────────────
const POLL_INTERVAL_MS = 1200;
const CYAN   = '#00d4ff';
const PURPLE = '#7c3aed';
const GREEN  = '#10b981';
const AMBER  = '#f59e0b';
const RED    = '#ef4444';
const MUTED  = '#4b5563';

// ── State ─────────────────────────────────────────────────────────
let isConnected    = false;
let metricsCache   = null;
let lsmStateCache  = null;
let lastVerification = null;

// ── Utility helpers ───────────────────────────────────────────────

function fmtBytes(n) {
  if (n === 0) return '0 B';
  const units = ['B','KB','MB','GB'];
  const i = Math.floor(Math.log(n) / Math.log(1024));
  return (n / Math.pow(1024, i)).toFixed(i > 0 ? 1 : 0) + ' ' + units[i];
}

function fmtNum(n) {
  if (n >= 1_000_000) return (n / 1_000_000).toFixed(1) + 'M';
  if (n >= 1_000)     return (n / 1_000).toFixed(1)     + 'K';
  return String(n);
}

function fmtPct(v) {
  return v.toFixed(1) + '%';
}

function fmtStatus(s) {
  if (!s) return 'Not run';
  return s.toUpperCase();
}

// ── Status pill ───────────────────────────────────────────────────

function setConnected(ok) {
  isConnected = ok;
  const pill = document.getElementById('status-pill');
  const text = document.getElementById('status-text');
  if (ok) {
    pill.classList.add('connected');
    text.textContent = 'Engine connected';
  } else {
    pill.classList.remove('connected');
    text.textContent = 'Reconnecting...';
  }
}

// ── Arc Gauge (canvas) ────────────────────────────────────────────
// Draws a half-ring arc gauge for write/read amplification.
// value: current reading   max: scale ceiling   color: arc color

function drawArcGauge(canvasId, value, max, color) {
  const canvas = document.getElementById(canvasId);
  if (!canvas) return;
  const ctx = canvas.getContext('2d');
  const W = canvas.width;
  const H = canvas.height;
  const cx = W / 2;
  const cy = H - 10;
  const R  = Math.min(cx, cy) - 12;

  ctx.clearRect(0, 0, W, H);

  const startAngle = Math.PI;
  const endAngle   = 0;  // left to right (180° → 360°)

  // Track background
  ctx.beginPath();
  ctx.arc(cx, cy, R, startAngle, endAngle, false);
  ctx.strokeStyle = 'rgba(255,255,255,0.06)';
  ctx.lineWidth = 10;
  ctx.lineCap = 'round';
  ctx.stroke();

  // Filled arc
  const ratio = Math.min(value / max, 1);
  const fillEnd = startAngle + ratio * Math.PI;

  // Gradient glow
  const grad = ctx.createLinearGradient(cx - R, cy, cx + R, cy);
  grad.addColorStop(0, color);
  grad.addColorStop(1, color + '88');

  ctx.beginPath();
  ctx.arc(cx, cy, R, startAngle, fillEnd, false);
  ctx.strokeStyle = grad;
  ctx.lineWidth = 10;
  ctx.lineCap = 'round';
  ctx.stroke();

  // Glow shadow
  ctx.shadowColor  = color;
  ctx.shadowBlur   = 16;
  ctx.beginPath();
  ctx.arc(cx, cy, R, startAngle, fillEnd, false);
  ctx.strokeStyle  = color + '40';
  ctx.lineWidth    = 18;
  ctx.lineCap      = 'round';
  ctx.stroke();
  ctx.shadowBlur = 0;

  // Tick marks at 0, 0.5, 1.0, 1.5, 2.0, 2.5, 3.0 of max
  const ticks = 7;
  ctx.strokeStyle = 'rgba(255,255,255,0.12)';
  ctx.lineWidth = 1;
  for (let i = 0; i <= ticks; i++) {
    const angle = startAngle + (i / ticks) * Math.PI;
    const ir = R - 14;
    const or = R + 2;
    ctx.beginPath();
    ctx.moveTo(cx + ir * Math.cos(angle), cy + ir * Math.sin(angle));
    ctx.lineTo(cx + or * Math.cos(angle), cy + or * Math.sin(angle));
    ctx.stroke();
  }
}

// ── KPI cards ────────────────────────────────────────────────────

function updateKPIs(m) {
  document.getElementById('kpi-user-bytes').textContent = fmtBytes(m.user_bytes_written);
  document.getElementById('kpi-get-calls').textContent  = fmtNum(m.get_calls);
  document.getElementById('kpi-bloom').textContent      = fmtPct(m.bloom_effectiveness);
  document.getElementById('kpi-vlog').textContent       = fmtNum(m.vlog_reads);
}

// ── Amplification gauges ─────────────────────────────────────────

function updateGauges(m) {
  const wa = m.write_amplification;
  const ra = m.read_amplification;

  document.getElementById('write-amp-val').textContent = wa.toFixed(2) + '×';
  document.getElementById('read-amp-val').textContent  = ra.toFixed(2) + '×';

  // Write amp gauge: expected range 1.0 – 4.0× (2.0× is the WiscKey floor)
  drawArcGauge('write-amp-canvas', wa, 4.0, CYAN);
  // Read amp gauge: expected range 0 – 3.0×
  drawArcGauge('read-amp-canvas', ra, 3.0, PURPLE);
}

// ── LSM Tree visualization ────────────────────────────────────────

function buildSSTBlocks(count, cssClass) {
  if (count === 0) return '<span style="color:var(--text-muted);font-size:11px">none</span>';
  let html = '';
  const show = Math.min(count, 20);
  for (let i = 0; i < show; i++) {
    html += `<div class="sst-block ${cssClass}" title="SSTable ${i + 1}"></div>`;
  }
  if (count > 20) {
    html += `<span style="font-size:10px;color:var(--text-muted);margin-left:2px">+${count - 20}</span>`;
  }
  return html;
}

function updateLSM(s) {
  // Memtable fill bar
  const fillPct = Math.min(s.memtable_fill_pct, 100);
  document.getElementById('memtable-fill-bar').style.width = fillPct + '%';
  document.getElementById('memtable-meta').textContent =
    `${fmtNum(s.memtable_entries)} entries · ${fmtBytes(s.memtable_bytes)} / ${fmtBytes(s.flush_threshold)} (${fillPct.toFixed(1)}%)`;

  // L0 files
  document.getElementById('lsm-l0-files').innerHTML = buildSSTBlocks(s.l0_count, 'sst-l0');
  document.getElementById('l0-meta').textContent =
    `${s.l0_count} / ${s.l0_limit} files · ${s.l0_pressure_pct.toFixed(1)}% compaction pressure`;

  // L1 files
  document.getElementById('lsm-l1-files').innerHTML = buildSSTBlocks(s.l1_count, 'sst-l1');
  document.getElementById('l1-meta').textContent =
    `${s.l1_count} non-overlapping files`;

  // Health badge
  const badge = document.getElementById('lsm-health-badge');
  if (s.wal_tainted) {
    badge.textContent = '⚠ WAL tainted';
    badge.className = 'panel-badge';
    badge.style.cssText = 'background:rgba(239,68,68,0.12);color:#ef4444;border:1px solid rgba(239,68,68,0.25)';
  } else if (s.l0_pressure_pct > 80) {
    badge.textContent = '⚡ compaction imminent';
    badge.className = 'panel-badge';
    badge.style.cssText = 'background:rgba(245,158,11,0.12);color:#f59e0b;border:1px solid rgba(245,158,11,0.25)';
  } else {
    badge.textContent = '✓ healthy';
    badge.className = 'panel-badge';
    badge.style.cssText = 'background:rgba(16,185,129,0.1);color:#10b981;border:1px solid rgba(16,185,129,0.2)';
  }
}

// ── Bloom + health detail panels ──────────────────────────────────

function updateDetails(m, s) {
  // Bloom
  document.getElementById('stat-sst-considered').textContent = fmtNum(m.sst_considered);
  document.getElementById('stat-bloom-skips').textContent    = fmtNum(m.bloom_skips);
  document.getElementById('stat-sst-searches').textContent   = fmtNum(m.sst_searches);
  document.getElementById('stat-bloom-pct').textContent      = fmtPct(m.bloom_effectiveness);
  document.getElementById('bloom-bar-fill').style.width      = Math.min(m.bloom_effectiveness, 100) + '%';

  // Health panel
  const walEl = document.getElementById('stat-wal-status');
  walEl.textContent = s.wal_tainted ? '⚠ TAINTED' : '✓ Clean';
  walEl.style.color = s.wal_tainted ? RED : GREEN;

  document.getElementById('stat-mem-fill').textContent      = fmtPct(s.memtable_fill_pct);
  document.getElementById('stat-l0-pressure').textContent   = fmtPct(s.l0_pressure_pct);
  document.getElementById('stat-mem-entries').textContent   = fmtNum(s.memtable_entries);

  // Health indicator
  const dot   = document.getElementById('health-dot');
  const label = document.getElementById('health-label');
  dot.className = 'health-dot';

  if (s.wal_tainted) {
    dot.classList.add('red');
    label.textContent = 'WAL corruption detected — engine in tainted state';
  } else if (s.l0_pressure_pct > 85) {
    dot.classList.add('amber');
    label.textContent = 'High L0 pressure — compaction will trigger on next write';
  } else if (s.memtable_fill_pct > 85) {
    dot.classList.add('amber');
    label.textContent = 'Memtable near flush threshold — flush imminent';
  } else {
    dot.classList.add('green');
    label.textContent = 'All systems nominal — engine operating normally';
  }
}

// ── Polling loop ──────────────────────────────────────────────────

async function fetchMetrics() {
  try {
    const [mRes, sRes] = await Promise.all([
      fetch('/api/metrics'),
      fetch('/api/lsm-state'),
    ]);
    if (!mRes.ok || !sRes.ok) throw new Error('fetch failed');

    metricsCache  = await mRes.json();
    lsmStateCache = await sRes.json();

    setConnected(true);
    updateKPIs(metricsCache);
    updateGauges(metricsCache);
    updateLSM(lsmStateCache);
    updateDetails(metricsCache, lsmStateCache);
  } catch (err) {
    setConnected(false);
  }
}

// ── Verification Console ─────────────────────────────────────────

const verifyBtn = document.getElementById('verify-run');
const verifyOps = document.getElementById('verify-ops');
const verifyMatrix = document.getElementById('verification-matrix');

function setVerificationLoading(loading, testName = 'full') {
  if (!verifyBtn) return;
  verifyBtn.disabled = loading;
  document.querySelectorAll('.verify-test-btn').forEach((btn) => { btn.disabled = loading; });
  verifyBtn.textContent = loading ? `Running ${testName}...` : 'Run Full Verification';
}

function renderVerification(result) {
  lastVerification = result;
  const statusEl = document.getElementById('verify-status');
  const recoveredEl = document.getElementById('verify-recovered');
  const mismatchEl = document.getElementById('verify-mismatches');
  const bloomEl = document.getElementById('verify-bloom');
  const stepsEl = document.getElementById('verify-steps');

  const mismatchCount = Array.isArray(result.mismatches) ? result.mismatches.length : 0;

  statusEl.textContent = fmtStatus(result.status);
  statusEl.className = 'verification-value ' + (result.status === 'pass' ? 'pass' : 'fail');
  recoveredEl.textContent = fmtNum(result.post_recovery_checked || 0);
  mismatchEl.textContent = String(mismatchCount);
  mismatchEl.className = 'verification-value ' + (mismatchCount === 0 ? 'pass' : 'fail');
  bloomEl.textContent = fmtNum(result.bloom_skips || 0);

  document.getElementById('ev-flush').textContent =
    `L0 ${result.l0_before_flush ?? 0} -> ${result.l0_after_flush ?? 0}`;
  document.getElementById('ev-compaction').textContent =
    `L0 ${result.l0_before_compaction ?? 0} -> ${result.l0_after_compaction ?? 0}, ` +
    `L1 ${result.l1_before_compaction ?? 0} -> ${result.l1_after_compaction ?? 0}`;
  document.getElementById('ev-deletes').textContent =
    `${result.deleted_checked || 0} checked, ${result.deleted_found || 0} incorrectly found`;
  document.getElementById('ev-model').textContent =
    `${fmtNum(result.model_live_keys || 0)} live keys in reference model`;
  document.getElementById('verify-raw').textContent = JSON.stringify(result, null, 2);

  if (result.test) {
    const matrixCell = document.getElementById(`matrix-${result.test}`);
    if (matrixCell) {
      matrixCell.textContent = result.status === 'pass' ? 'PASS' : 'FAIL';
      matrixCell.className = result.status === 'pass' ? 'pass' : 'fail';
    }
  }

  if (!Array.isArray(result.steps) || result.steps.length === 0) {
    stepsEl.innerHTML = '<div class="evidence-empty">No steps returned.</div>';
    return;
  }

  stepsEl.innerHTML = result.steps.map((step) => `
    <div class="evidence-step ${step.status === 'pass' ? 'pass' : 'fail'}">
      <div class="evidence-step-status">${step.status === 'pass' ? 'PASS' : 'FAIL'}</div>
      <div>
        <div class="evidence-step-name">${step.name}</div>
        <div class="evidence-step-detail">${step.detail}</div>
      </div>
    </div>
  `).join('');
}

async function runVerification(test) {
  setVerificationLoading(true, test);
  try {
    const ops = parseInt(verifyOps.value, 10);
    const r = await fetch(`/api/verify/${test}`, {
      method: 'POST',
      headers: {'Content-Type':'application/json'},
      body: JSON.stringify({ops}),
    });
    const result = await r.json();
    renderVerification(result);
    await fetchMetrics();
  } catch (err) {
    renderVerification({
      test,
      status: 'fail',
      post_recovery_checked: 0,
      bloom_skips: 0,
      mismatches: [`network error: ${err.message}`],
      steps: [{
        name: 'Verification request',
        status: 'fail',
        detail: err.message,
      }],
    });
  } finally {
    setVerificationLoading(false);
  }
}

if (verifyBtn) {
  verifyBtn.addEventListener('click', async () => {
    await runVerification('full');
  });
}

if (verifyMatrix) {
  verifyMatrix.addEventListener('click', async (event) => {
    const btn = event.target.closest('.verify-test-btn');
    if (!btn) return;
    await runVerification(btn.dataset.test);
  });
}

// ── Interactive Console ───────────────────────────────────────────

const consoleOutput = document.getElementById('console-output');
const consoleForm   = document.getElementById('console-form');
const consoleInput  = document.getElementById('console-input');

function appendConsoleLine(prefix, text, prefixClass, textClass) {
  const line = document.createElement('div');
  line.className = 'console-line';

  const p = document.createElement('span');
  p.className = `console-prefix ${prefixClass}`;
  p.textContent = prefix;

  const t = document.createElement('span');
  t.className = `console-resp ${textClass}`;
  t.textContent = text;

  line.appendChild(p);
  line.appendChild(t);
  consoleOutput.appendChild(line);
  consoleOutput.scrollTop = consoleOutput.scrollHeight;
}

function appendConsoleCmd(text) {
  const line = document.createElement('div');
  line.className = 'console-line';

  const p = document.createElement('span');
  p.className = 'console-prefix';
  p.style.color = '#7c3aed';
  p.textContent = 'ForgeLSM>';

  const t = document.createElement('span');
  t.className = 'console-cmd';
  t.textContent = ' ' + text;

  line.appendChild(p);
  line.appendChild(t);
  consoleOutput.appendChild(line);
  consoleOutput.scrollTop = consoleOutput.scrollHeight;
}

consoleForm.addEventListener('submit', async (e) => {
  e.preventDefault();
  const raw = consoleInput.value.trim();
  if (!raw) return;
  consoleInput.value = '';
  appendConsoleCmd(raw);

  const parts = raw.split(/\s+/);
  const cmd   = parts[0].toLowerCase();

  try {
    if (cmd === 'put') {
      const key   = parts[1] || '';
      const value = parts.length > 2 ? parts.slice(2).join(' ').replace(/^["']|["']$/g, '') : undefined;
      if (!key || value === undefined) { appendConsoleLine('>', 'usage: put <key> <value>', '', 'err'); return; }
      const r = await fetch('/api/put', {
        method: 'POST',
        headers: {'Content-Type':'application/json'},
        body: JSON.stringify({key, value}),
      });
      const d = await r.json();
      appendConsoleLine('>', d.ok ? 'OK' : (d.error || 'error'), '', d.ok ? 'ok' : 'err');
    }
    else if (cmd === 'get') {
      const key = parts[1] || '';
      if (!key) { appendConsoleLine('>', 'usage: get <key>', '', 'err'); return; }
      const r = await fetch('/api/get', {
        method: 'POST',
        headers: {'Content-Type':'application/json'},
        body: JSON.stringify({key}),
      });
      const d = await r.json();
      if (d.found) {
        appendConsoleLine('>', `"${d.value}"`, '', 'found');
      } else {
        appendConsoleLine('>', '(not found)', '', 'miss');
      }
    }
    else if (cmd === 'del' || cmd === 'delete') {
      const key = parts[1] || '';
      if (!key) { appendConsoleLine('>', 'usage: del <key>', '', 'err'); return; }
      const r = await fetch('/api/delete', {
        method: 'POST',
        headers: {'Content-Type':'application/json'},
        body: JSON.stringify({key}),
      });
      const d = await r.json();
      appendConsoleLine('>', d.ok ? 'OK (tombstone written)' : (d.error || 'error'), '', d.ok ? 'ok' : 'err');
    }
    else if (cmd === 'help') {
      appendConsoleLine('>', 'put <key> <value>  · write a key-value pair', '', 'found');
      appendConsoleLine('>', 'get <key>          · read a value by key', '', 'found');
      appendConsoleLine('>', 'del <key>          · delete a key (tombstone)', '', 'found');
    }
    else {
      appendConsoleLine('>', `unknown command: ${cmd}. type help.`, '', 'err');
    }
  } catch (err) {
    appendConsoleLine('>', `network error: ${err.message}`, '', 'err');
  }

  // Refresh metrics immediately after console action
  await fetchMetrics();
});

// ── Benchmark Runner ──────────────────────────────────────────────

const benchBtn     = document.getElementById('bench-run');
const benchResults = document.getElementById('bench-results');

benchBtn.addEventListener('click', async () => {
  const type = document.getElementById('bench-type').value;
  const ops  = parseInt(document.getElementById('bench-ops').value, 10);

  benchBtn.disabled = true;
  document.getElementById('bench-btn-text').innerHTML =
    '<span class="spinner"></span>Running...';
  benchResults.innerHTML = `<div class="bench-placeholder">Running ${ops} ${type.replace('_',' ')} operations…</div>`;

  try {
    const r = await fetch('/api/bench', {
      method: 'POST',
      headers: {'Content-Type':'application/json'},
      body: JSON.stringify({type, ops}),
    });
    const d = await r.json();

    if (d.error) {
      benchResults.innerHTML = `<div class="bench-placeholder" style="color:var(--red)">${d.error}</div>`;
      return;
    }

    const tput  = d.throughput.toFixed(0);
    const wa    = d.write_amp.toFixed(2);
    const ra    = d.read_amp.toFixed(2);
    const etime = d.elapsed_s.toFixed(2);

    benchResults.innerHTML = `
      <div class="bench-result-grid">
        <div class="bench-metric">
          <div class="bench-metric-label">Throughput</div>
          <div class="bench-metric-value cyan">${Number(tput).toLocaleString()}</div>
          <div class="bench-metric-sub">ops / second</div>
        </div>
        <div class="bench-metric">
          <div class="bench-metric-label">Elapsed Time</div>
          <div class="bench-metric-value">${etime}</div>
          <div class="bench-metric-sub">seconds · ${d.ops.toLocaleString()} ops</div>
        </div>
        <div class="bench-metric">
          <div class="bench-metric-label">Write Amplification</div>
          <div class="bench-metric-value ${wa > 3 ? '' : 'cyan'}">${wa}×</div>
          <div class="bench-metric-sub">WiscKey target: ~2.0×</div>
        </div>
        <div class="bench-metric">
          <div class="bench-metric-label">Read Amplification</div>
          <div class="bench-metric-value purple">${ra}×</div>
          <div class="bench-metric-sub">Bloom-filtered path</div>
        </div>
      </div>
      <div style="margin-top:10px;font-size:11px;color:var(--text-muted);font-family:var(--font-mono)">
        workload: ${type} · isolated bench dir · cold + warm runs
      </div>
    `;

    // Refresh metrics after benchmark
    await fetchMetrics();
  } catch (err) {
    benchResults.innerHTML =
      `<div class="bench-placeholder" style="color:var(--red)">Network error: ${err.message}</div>`;
  } finally {
    benchBtn.disabled = false;
    document.getElementById('bench-btn-text').textContent = '▶ Run Benchmark';
  }
});

// ── Boot ──────────────────────────────────────────────────────────
(async function init() {
  await fetchMetrics();
  setInterval(fetchMetrics, POLL_INTERVAL_MS);
})();
