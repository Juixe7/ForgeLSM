'use strict';

const POLL_INTERVAL_MS = 2500;

const els = {
  statusPill: document.getElementById('status-pill'),
  statusText: document.getElementById('status-text'),
  refreshState: document.getElementById('refresh-state'),
  resetProduction: document.getElementById('reset-production'),
  prodRaw: document.getElementById('prod-raw'),
  prodTrace: document.getElementById('prod-trace'),
  streamStart: document.getElementById('stream-start'),
  streamStop: document.getElementById('stream-stop'),
  streamMode: document.getElementById('stream-mode'),
  streamOps: document.getElementById('stream-ops'),
  streamDevices: document.getElementById('stream-devices'),
  demoRun: document.getElementById('demo-run'),
  demoEvents: document.getElementById('demo-events'),
  demoRaw: document.getElementById('demo-raw'),
  demoTrace: document.getElementById('demo-trace'),
  demoTimeline: document.getElementById('demo-timeline'),
  verifyRun: document.getElementById('verify-run'),
  verifyOps: document.getElementById('verify-ops'),
  matrix: document.getElementById('verification-matrix'),
  verifyRaw: document.getElementById('verify-raw'),
};

function byId(id) {
  return document.getElementById(id);
}

function fmtNum(value) {
  return Number(value || 0).toLocaleString('en-IN');
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

function escapeHtml(value) {
  return String(value ?? '')
    .replaceAll('&', '&amp;')
    .replaceAll('<', '&lt;')
    .replaceAll('>', '&gt;')
    .replaceAll('"', '&quot;')
    .replaceAll("'", '&#039;');
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
  setText('state-l2', `${fmtNum(lsm.l2_count)} files`);
  setText('state-wal', debug.wal_tainted ? 'TAINTED' : `Clean, ${fmtNum(debug.wal_files)} file(s), ${fmtBytes(debug.wal_bytes)}`);
  setText('state-total-disk', fmtBytes(debug.total_disk_bytes));
  setText('state-live-logical', fmtBytes(debug.live_logical_bytes_estimate));
  setText('state-live-keys', fmtNum(debug.live_keys_estimate));
  setText('state-tombstones', fmtNum(debug.tombstones_estimate));
  setText('state-space-amp', `${Number(debug.space_amplification_estimate || 0).toFixed(2)}x`);
}

function renderStorage(files) {
  const fileRows = [];
  fileRows.push(`<tr><th>Manifest</th><td>${files.manifest_loaded ? 'loaded' : 'not created yet'}</td><td>${fmtBytes(files.manifest_bytes)}</td></tr>`);

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

  const rows = [];
  const sstables = Array.isArray(files.sstables) ? files.sstables : [];
  const unreferenced = Array.isArray(files.unreferenced_sstables) ? files.unreferenced_sstables : [];

  sstables.forEach((sst) => {
    rows.push(`<tr><td>${escapeHtml(sst.level)}</td><td>${escapeHtml(sst.file)}</td><td>${fmtBytes(sst.bytes)}</td><td>${sst.exists ? 'yes' : 'no'}</td></tr>`);
  });
  unreferenced.forEach((sst) => {
    rows.push(`<tr class="warning-row"><td>unreferenced</td><td>${escapeHtml(sst.file)}</td><td>${fmtBytes(sst.bytes)}</td><td>orphan</td></tr>`);
  });

  byId('sstable-body').innerHTML = rows.length
    ? rows.join('')
    : '<tr><td colspan="4">No production SSTables yet. Run a large production workload to cross the 4 MB flush threshold.</td></tr>';
}

async function refreshState(writeRaw = true) {
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
    if (writeRaw) {
      els.prodRaw.textContent = JSON.stringify({metrics, lsm_state: lsm, debug_state: debug, files}, null, 2);
    }
  } catch (err) {
    setConnected(false, err.message);
  }
}

function statusClass(status) {
  if (status === 'pass') return 'pass';
  if (status === 'fail') return 'fail';
  return 'pending';
}

function renderTrace(trace) {
  const rows = Array.isArray(trace) ? trace : [];
  els.prodTrace.innerHTML = rows.length
    ? rows.map((entry) => `
        <div class="step-row pass">
          <div class="step-status">${escapeHtml(entry.phase || 'trace')}</div>
          <div>
            <div class="step-name">${escapeHtml(entry.phase || 'operation')}</div>
            <div class="step-detail">${escapeHtml(entry.detail || '')}</div>
          </div>
        </div>
      `).join('')
    : '<div class="empty-state">No trace entries returned.</div>';
}

function renderStreamStatus(status) {
  setText('stream-status', String(status.status || 'idle').toUpperCase());
  byId('stream-status').className = status.running ? 'pending' : (status.status === 'completed' ? 'pass' : '');
  setText('stream-progress', `${Number(status.progress_pct || 0).toFixed(1)}%`);
  setText(
    'stream-mix',
    `P ${fmtNum(status.puts)} / U ${fmtNum(status.updates)} / D ${fmtNum(status.deletes)} / G ${fmtNum(status.gets)}`
  );
  setText('stream-file', status.workload_file || '-');
  setText('last-mode', status.mode === 'stream2' ? 'Stream 2' : 'Stream 1');
  setText('last-completed', `${fmtNum(status.completed_operations)} / ${fmtNum(status.target_operations)} ops`);
  setText('last-mix', `P ${fmtNum(status.puts)} / U ${fmtNum(status.updates)} / D ${fmtNum(status.deletes)} / G ${fmtNum(status.gets)}`);
  setText('last-unique-telemetry', fmtNum(status.unique_telemetry_keys));
  setText('last-unique-state', fmtNum(status.unique_state_keys));
  setText('last-delete-targets', fmtNum(status.successful_delete_targets));
  setText('last-logical', fmtBytes(status.logical_bytes_delta));
  setText('last-physical', fmtBytes(status.physical_bytes_delta));
  setText('last-write-amp', fmtAmp(status.write_amplification));
  setText('last-mem-transition', `${fmtNum(status.memtable_entries_before)} -> ${fmtNum(status.memtable_entries_after)} entries`);
  setText('last-l0-transition', `${fmtNum(status.l0_before)} -> ${fmtNum(status.l0_after)} files`);
  setText('last-l1-transition', `${fmtNum(status.l1_before)} -> ${fmtNum(status.l1_after)} files`);
  setText('last-l2-transition', `${fmtNum(status.l2_before)} -> ${fmtNum(status.l2_after)} files`);
  setText('last-mismatches', fmtNum(status.mismatches));
  byId('last-mismatches').className = Number(status.mismatches || 0) === 0 ? 'pass' : 'fail';

  const log = Array.isArray(status.log) ? status.log : [];
  if (log.length) {
    els.prodTrace.innerHTML = log.map((line) => `
      <div class="step-row pass">
        <div class="step-status">live</div>
        <div>
          <div class="step-name">${escapeHtml(line.split('|')[0] || 'operation')}</div>
          <div class="step-detail">${escapeHtml(line)}</div>
        </div>
      </div>
    `).join('');
  }
}

async function refreshStreamStatus() {
  try {
    const status = await getJson('/api/stream/status');
    renderStreamStatus(status);
  } catch (_) {
    // Main connection status already reports server availability.
  }
}

async function startStream() {
  els.streamStart.disabled = true;
  try {
    await postJson('/api/stream/start', {
      mode: els.streamMode.value,
      operations: parseInt(els.streamOps.value, 10),
      devices: parseInt(els.streamDevices.value, 10),
    });
    await refreshStreamStatus();
  } catch (err) {
    renderTrace([{phase: 'stream-error', detail: err.message}]);
  } finally {
    els.streamStart.disabled = false;
  }
}

async function stopStream() {
  els.streamStop.disabled = true;
  try {
    await postJson('/api/stream/stop', {});
    await refreshStreamStatus();
  } catch (err) {
    renderTrace([{phase: 'stream-error', detail: err.message}]);
  } finally {
    els.streamStop.disabled = false;
  }
}

async function resetProductionStore() {
  els.resetProduction.disabled = true;
  try {
    const result = await postJson('/api/production/reset', {});
    els.prodRaw.textContent = JSON.stringify(result, null, 2);
    renderTrace([{phase: 'production-reset', detail: 'flsm_production cleared; run a stream to generate fresh evidence'}]);
    await refreshState(false);
    await refreshStreamStatus();
  } catch (err) {
    els.prodRaw.textContent = JSON.stringify({error: err.message}, null, 2);
    renderTrace([{phase: 'reset-error', detail: err.message}]);
  } finally {
    els.resetProduction.disabled = false;
  }
}

function setMatrixCell(test, status) {
  const cell = byId(`matrix-${test}`);
  if (!cell) return;
  cell.textContent = status === 'pass' ? 'PASS' : status === 'fail' ? 'FAIL' : 'RUNNING';
  cell.className = statusClass(status);
}

function renderDemoTimeline(timeline) {
  const rows = Array.isArray(timeline) ? timeline : [];
  els.demoTimeline.innerHTML = rows.length
    ? rows.map((entry) => `
        <div class="step-row pass">
          <div class="step-status">LSM</div>
          <div>
            <div class="step-name">${escapeHtml(entry.phase || 'demo step')}</div>
            <div class="step-detail">mem=${fmtNum(entry.memtable_entries)} | L0=${fmtNum(entry.l0)} | L1=${fmtNum(entry.l1)} | L2=${fmtNum(entry.l2)}</div>
          </div>
        </div>
      `).join('')
    : '<div class="empty-state">No timeline returned.</div>';
}

function setDemoBusy(isBusy) {
  els.demoRun.disabled = isBusy;
  els.demoRun.textContent = isBusy ? 'Running...' : 'Run Demo';
}

async function runDemo() {
  const events = parseInt(els.demoEvents.value, 10);
  setDemoBusy(true);
  setText('demo-status', 'RUNNING');
  byId('demo-status').className = 'pending';
  try {
    const result = await postJson('/api/demo/run', {events});
    setText('demo-status', String(result.status || 'fail').toUpperCase());
    byId('demo-status').className = statusClass(result.status);
    setText('demo-l0', fmtNum(result.final_l0));
    setText('demo-l1', fmtNum(result.final_l1));
    setText('demo-l2', fmtNum(result.final_l2));
    renderDemoTimeline(result.timeline);
    els.demoTrace.textContent = Array.isArray(result.trace) ? result.trace.join('\n') : '';
    els.demoRaw.textContent = JSON.stringify(result, null, 2);
  } catch (err) {
    setText('demo-status', 'FAIL');
    byId('demo-status').className = 'fail';
    els.demoRaw.textContent = JSON.stringify({error: err.message}, null, 2);
    els.demoTrace.textContent = err.message;
  } finally {
    setDemoBusy(false);
  }
}

function renderVerification(result) {
  const status = result.status || 'fail';
  const mismatches = Array.isArray(result.mismatches) ? result.mismatches.length : 0;

  setText('verify-status', status.toUpperCase());
  byId('verify-status').className = statusClass(status);
  setText('verify-recovered', fmtNum(result.post_recovery_checked || 0));
  setText('verify-mismatches', fmtNum(mismatches));
  byId('verify-mismatches').className = mismatches === 0 ? 'pass' : 'fail';
  setText('verify-bloom', fmtNum(result.bloom_skips || 0));

  setText('ev-test', result.test || '-');
  setText('ev-requested', fmtNum(result.requested_events || 0));
  setText('ev-model', fmtNum(result.model_live_keys || 0));
  setText('ev-flush', `L0 ${result.l0_before_flush || 0} -> ${result.l0_after_flush || 0}`);
  setText('ev-compaction', `L0 ${result.l0_before_compaction || 0} -> ${result.l0_after_compaction || 0}; L1 ${result.l1_before_compaction || 0} -> ${result.l1_after_compaction || 0}`);
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

  els.verifyRaw.textContent = JSON.stringify(result, null, 2);
}

function setVerificationBusy(isBusy, label) {
  els.verifyRun.disabled = isBusy;
  document.querySelectorAll('.matrix-btn').forEach((btn) => {
    btn.disabled = isBusy;
  });
  els.verifyRun.textContent = isBusy ? `Running ${label}...` : 'Run Full Verification';
}

async function runVerification(test) {
  const ops = parseInt(els.verifyOps.value, 10);
  setVerificationBusy(true, test);
  setMatrixCell(test === 'full' ? 'basic' : test, 'running');
  try {
    const result = await postJson(`/api/verify/${test}`, {ops});
    renderVerification(result);
    await refreshState(false);
  } catch (err) {
    renderVerification({
      test,
      status: 'fail',
      requested_events: ops,
      mismatches: [err.message],
      steps: [{name: 'Verification request', status: 'fail', detail: err.message}],
    });
  } finally {
    setVerificationBusy(false, test);
  }
}

document.querySelectorAll('.tab-btn').forEach((button) => {
  button.addEventListener('click', () => {
    document.querySelectorAll('.tab-btn').forEach((btn) => btn.classList.remove('active'));
    document.querySelectorAll('.view').forEach((view) => view.classList.remove('active'));
    button.classList.add('active');
    byId(button.dataset.view).classList.add('active');
  });
});

els.refreshState.addEventListener('click', () => refreshState(true));
els.resetProduction.addEventListener('click', resetProductionStore);
els.streamStart.addEventListener('click', startStream);
els.streamStop.addEventListener('click', stopStream);
els.demoRun.addEventListener('click', runDemo);
els.verifyRun.addEventListener('click', () => runVerification('full'));
els.matrix.addEventListener('click', (event) => {
  const button = event.target.closest('.matrix-btn');
  if (!button) return;
  runVerification(button.dataset.test);
});

refreshState();
refreshStreamStatus();
setInterval(() => {
  refreshState(false);
  refreshStreamStatus();
}, POLL_INTERVAL_MS);
