'use strict';

const POLL_INTERVAL_MS = 2500;

const els = {
  statusPill: document.getElementById('status-pill'),
  statusText: document.getElementById('status-text'),
  refreshState: document.getElementById('refresh-state'),
  verifyRun: document.getElementById('verify-run'),
  verifyOps: document.getElementById('verify-ops'),
  matrix: document.getElementById('verification-matrix'),
  raw: document.getElementById('verify-raw'),
};

function byId(id) {
  return document.getElementById(id);
}

function fmtNum(value) {
  const n = Number(value || 0);
  return n.toLocaleString('en-IN');
}

function fmtBytes(value) {
  const n = Number(value || 0);
  if (n === 0) return '0 B';
  const units = ['B', 'KB', 'MB', 'GB'];
  const idx = Math.min(Math.floor(Math.log(n) / Math.log(1024)), units.length - 1);
  const scaled = n / Math.pow(1024, idx);
  return `${scaled.toFixed(idx === 0 ? 0 : 1)} ${units[idx]}`;
}

function fmtAmp(value) {
  return `${Number(value || 0).toFixed(2)}x`;
}

function setText(id, value) {
  const el = byId(id);
  if (el) el.textContent = value;
}

function setConnected(ok, message) {
  els.statusPill.classList.toggle('connected', ok);
  els.statusPill.classList.toggle('disconnected', !ok);
  els.statusText.textContent = message || (ok ? 'Engine connected' : 'Engine unavailable');
}

async function getJson(path) {
  const response = await fetch(path);
  if (!response.ok) throw new Error(`${path} returned ${response.status}`);
  return response.json();
}

async function postJson(path, body) {
  const response = await fetch(path, {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify(body || {}),
  });
  const data = await response.json();
  if (!response.ok) throw new Error(data.error || `${path} returned ${response.status}`);
  return data;
}

function renderSystem(metrics, lsm, debug) {
  setText('state-mem-entries', fmtNum(lsm.memtable_entries));
  setText('state-mem-bytes', `${fmtBytes(lsm.memtable_bytes)} active`);
  setText('state-flush-threshold', fmtBytes(lsm.flush_threshold));
  setText('state-l0', `${fmtNum(lsm.l0_count)} / ${fmtNum(lsm.l0_limit)} files`);
  setText('state-l1', `${fmtNum(lsm.l1_count)} files`);
  setText('state-wal', debug.wal_tainted ? 'TAINTED' : `Clean, ${fmtNum(debug.wal_files)} file(s), ${fmtBytes(debug.wal_bytes)}`);
  setText('state-write-amp', fmtAmp(metrics.write_amplification));
  setText('state-read-amp', fmtAmp(metrics.read_amplification));
}

function renderStorage(files) {
  const fileRows = [];
  fileRows.push(`<tr><th>Manifest</th><td>${files.manifest_loaded ? 'loaded' : 'missing'}</td><td>${fmtBytes(files.manifest_bytes)}</td></tr>`);

  if (Array.isArray(files.wal_files) && files.wal_files.length) {
    files.wal_files.forEach((wal) => {
      fileRows.push(`<tr><th>WAL</th><td>${escapeHtml(wal.file)}</td><td>${fmtBytes(wal.bytes)}</td></tr>`);
    });
  } else {
    fileRows.push('<tr><th>WAL</th><td>none</td><td>0 B</td></tr>');
  }

  const vlog = files.vlog || {};
  fileRows.push(`<tr><th>VLog</th><td>${vlog.exists ? escapeHtml(vlog.file) : 'missing'}</td><td>${fmtBytes(vlog.bytes)}</td></tr>`);
  byId('file-log-body').innerHTML = fileRows.join('');

  const sstableRows = [];
  const sstables = Array.isArray(files.sstables) ? files.sstables : [];
  const unreferenced = Array.isArray(files.unreferenced_sstables) ? files.unreferenced_sstables : [];

  sstables.forEach((sst) => {
    sstableRows.push(
      `<tr><td>${escapeHtml(sst.level)}</td><td>${escapeHtml(sst.file)}</td><td>${fmtBytes(sst.bytes)}</td><td>${sst.exists ? 'yes' : 'no'}</td></tr>`
    );
  });
  unreferenced.forEach((sst) => {
    sstableRows.push(
      `<tr class="warning-row"><td>unreferenced</td><td>${escapeHtml(sst.file)}</td><td>${fmtBytes(sst.bytes)}</td><td>orphan</td></tr>`
    );
  });

  byId('sstable-body').innerHTML = sstableRows.length
    ? sstableRows.join('')
    : '<tr><td colspan="4">No SSTables in manifest yet. Run flush or compaction verification.</td></tr>';
}

function renderStateRaw(metrics, lsm, debug, files) {
  els.raw.textContent = JSON.stringify({metrics, lsm_state: lsm, debug_state: debug, files}, null, 2);
}

async function refreshState() {
  try {
    const [metrics, lsm, debug, files] = await Promise.all([
      getJson('/api/metrics'),
      getJson('/api/lsm-state'),
      getJson('/api/debug/state'),
      getJson('/api/debug/files'),
    ]);
    setConnected(true);
    renderSystem(metrics, lsm, debug);
    renderStorage(files);
    renderStateRaw(metrics, lsm, debug, files);
  } catch (err) {
    setConnected(false, err.message);
  }
}

function statusClass(status) {
  if (status === 'pass') return 'pass';
  if (status === 'fail') return 'fail';
  return 'pending';
}

function setMatrixCell(test, status) {
  const cell = byId(`matrix-${test}`);
  if (!cell) return;
  cell.textContent = status === 'pass' ? 'PASS' : status === 'fail' ? 'FAIL' : 'RUNNING';
  cell.className = statusClass(status);
}

function renderVerification(result) {
  const status = result.status || 'fail';
  const mismatches = Array.isArray(result.mismatches) ? result.mismatches.length : 0;
  const bloomSkips = Number(result.bloom_skips || 0);

  setText('verify-status', status.toUpperCase());
  byId('verify-status').className = statusClass(status);
  setText('verify-recovered', fmtNum(result.post_recovery_checked || 0));
  setText('verify-mismatches', fmtNum(mismatches));
  byId('verify-mismatches').className = mismatches === 0 ? 'pass' : 'fail';
  setText('verify-bloom', fmtNum(bloomSkips));

  setText('ev-test', result.test || '-');
  setText('ev-requested', fmtNum(result.requested_events || 0));
  setText('ev-model', fmtNum(result.model_live_keys || 0));
  setText('ev-flush', `L0 ${result.l0_before_flush || 0} -> ${result.l0_after_flush || 0}`);
  setText(
    'ev-compaction',
    `L0 ${result.l0_before_compaction || 0} -> ${result.l0_after_compaction || 0}; L1 ${result.l1_before_compaction || 0} -> ${result.l1_after_compaction || 0}`
  );
  setText('ev-deletes', `${fmtNum(result.deleted_checked || 0)} checked, ${fmtNum(result.deleted_found || 0)} found`);
  setText('ev-gc', `${fmtBytes(result.vlog_before_gc || 0)} -> ${fmtBytes(result.vlog_after_gc || 0)}`);

  if (result.test) setMatrixCell(result.test, status);
  if (result.test === 'full') {
    ['basic', 'overwrite', 'delete', 'flush', 'bloom', 'compaction', 'recovery'].forEach((test) => setMatrixCell(test, status));
  }

  const steps = Array.isArray(result.steps) ? result.steps : [];
  byId('verify-steps').innerHTML = steps.length
    ? steps.map((step) => `
        <div class="step-row ${statusClass(step.status)}">
          <div class="step-status">${String(step.status || 'fail').toUpperCase()}</div>
          <div>
            <div class="step-name">${escapeHtml(step.name || 'Unnamed step')}</div>
            <div class="step-detail">${escapeHtml(step.detail || '')}</div>
          </div>
        </div>
      `).join('')
    : '<div class="empty-state">No proof steps were returned.</div>';

  els.raw.textContent = JSON.stringify(result, null, 2);
}

function setBusy(isBusy, label) {
  els.verifyRun.disabled = isBusy;
  document.querySelectorAll('.matrix-btn').forEach((btn) => {
    btn.disabled = isBusy;
  });
  els.verifyRun.textContent = isBusy ? `Running ${label}...` : 'Run Full Verification';
}

async function runVerification(test) {
  const ops = parseInt(els.verifyOps.value, 10);
  setBusy(true, test);
  setMatrixCell(test === 'full' ? 'basic' : test, 'running');
  try {
    const result = await postJson(`/api/verify/${test}`, {ops});
    renderVerification(result);
    await refreshState();
    els.raw.textContent = JSON.stringify(result, null, 2);
  } catch (err) {
    renderVerification({
      test,
      status: 'fail',
      requested_events: ops,
      mismatches: [err.message],
      steps: [{name: 'Verification request', status: 'fail', detail: err.message}],
    });
  } finally {
    setBusy(false, test);
  }
}

function escapeHtml(value) {
  return String(value ?? '')
    .replaceAll('&', '&amp;')
    .replaceAll('<', '&lt;')
    .replaceAll('>', '&gt;')
    .replaceAll('"', '&quot;')
    .replaceAll("'", '&#039;');
}

els.refreshState.addEventListener('click', refreshState);
els.verifyRun.addEventListener('click', () => runVerification('full'));
els.matrix.addEventListener('click', (event) => {
  const button = event.target.closest('.matrix-btn');
  if (!button) return;
  runVerification(button.dataset.test);
});

refreshState();
setInterval(refreshState, POLL_INTERVAL_MS);
