'use strict';

const POLL_INTERVAL_MS = 2500;

const els = {
  statusPill: document.getElementById('status-pill'),
  statusText: document.getElementById('status-text'),
  refreshState: document.getElementById('refresh-state'),
  resetProduction: document.getElementById('reset-production'),
  prodCommand: document.getElementById('prod-command'),
  prodRunCommand: document.getElementById('prod-run-command'),
  prodSampleGet: document.getElementById('prod-sample-get'),
  prodClearOutput: document.getElementById('prod-clear-output'),
  prodQueryOutput: document.getElementById('prod-query-output'),
  traceToggle: document.getElementById('trace-toggle'),
  traceRefresh: document.getElementById('trace-refresh'),
  engineTerminal: document.getElementById('engine-terminal'),
  streamStart: document.getElementById('stream-start'),
  streamStop: document.getElementById('stream-stop'),
  streamMode: document.getElementById('stream-mode'),
  streamOps: document.getElementById('stream-ops'),
  streamDevices: document.getElementById('stream-devices'),
  experimentRun: document.getElementById('experiment-run'),
  experimentBasic: document.getElementById('experiment-basic'),
  experimentLsm: document.getElementById('experiment-lsm'),
  experimentScript: document.getElementById('experiment-script'),
  experimentSteps: document.getElementById('experiment-steps'),
  experimentRaw: document.getElementById('experiment-raw'),
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
  const response = await fetch(path, {cache: 'no-store'});
  if (!response.ok) throw new Error(`${path} returned ${response.status}`);
  return response.json();
}

async function postJson(path, body) {
  const response = await fetch(path, {
    method: 'POST',
    cache: 'no-store',
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
  setText('state-max-level', `L${fmtNum(lsm.max_level || 0)}`);
  setText('state-wal', debug.wal_tainted ? 'TAINTED' : `Clean, ${fmtNum(debug.wal_files)} file(s), ${fmtBytes(debug.wal_bytes)}`);
  setText('state-total-disk', fmtBytes(debug.total_disk_bytes));
  setText('state-live-logical', fmtBytes(debug.live_logical_bytes_estimate));
  setText('state-live-keys', fmtNum(debug.live_keys_estimate));
  setText('state-tombstones', fmtNum(debug.tombstones_estimate));
  setText('state-space-amp', `${Number(debug.space_amplification_estimate || 0).toFixed(2)}x`);

  const levelCounts = Array.isArray(lsm.level_counts) && lsm.level_counts.length
    ? lsm.level_counts
    : [
        {level: 0, sstables: lsm.l0_count || 0, limit: lsm.l0_limit || 0},
        {level: 1, sstables: lsm.l1_count || 0, limit: 0},
        {level: 2, sstables: lsm.l2_count || 0, limit: 0},
      ];
  byId('level-map-body').innerHTML = levelCounts.map((level) => {
    const count = Number(level.sstables || 0);
    const limit = Number(level.limit || 0);
    const pressure = limit > 0 ? `${((count / limit) * 100).toFixed(1)}%` : '-';
    const className = limit > 0 && count > limit ? ' class="warning-row"' : '';
    return `<tr${className}><td>L${fmtNum(level.level)}</td><td>${fmtNum(count)}</td><td>${limit > 0 ? fmtNum(limit) : '-'}</td><td>${pressure}</td></tr>`;
  }).join('');
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
    : '<tr><td colspan="4">No production SSTables yet. Run enough production or MQTT data to cross the 1 MB flush threshold.</td></tr>';
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
  } catch (err) {
    setConnected(false, err.message);
  }
}

function statusClass(status) {
  if (status === 'pass') return 'pass';
  if (status === 'fail') return 'fail';
  return 'pending';
}

function appendProductionOutput(line) {
  const now = new Date().toLocaleTimeString();
  const current = els.prodQueryOutput.textContent || '';
  const prefix = current.includes('\n') || !current.startsWith('Run get') ? `${current}\n` : '';
  els.prodQueryOutput.textContent = `${prefix}[${now}] ${line}`.trim();
  els.prodQueryOutput.scrollTop = els.prodQueryOutput.scrollHeight;
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
  setText('last-mode', status.mode === 'mqtt' ? 'MQTT-style' : (status.mode === 'stream2' ? 'Stream 2' : 'Stream 1'));
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
    appendProductionOutput(`stream-error | ${err.message}`);
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
    appendProductionOutput(`stream-error | ${err.message}`);
  } finally {
    els.streamStop.disabled = false;
  }
}

async function resetProductionStore() {
  els.resetProduction.disabled = true;
  try {
    const result = await postJson('/api/production/reset', {});
    appendProductionOutput(`production-reset | ${result.ok ? 'flsm_production cleared' : 'reset requested'}`);
    await refreshState();
    await refreshStreamStatus();
  } catch (err) {
    appendProductionOutput(`reset-error | ${err.message}`);
  } finally {
    els.resetProduction.disabled = false;
  }
}

function parseProductionCommand(input) {
  const text = String(input || '').trim();
  if (!text) throw new Error('Enter a command first.');
  const [opRaw, ...rest] = text.split(/\s+/);
  const op = opRaw.toLowerCase();
  if (op === 'get') {
    const key = rest.join(' ').trim();
    if (!key) throw new Error('Usage: get <key>');
    return {op, key};
  }
  if (op === 'del' || op === 'delete') {
    const key = rest.join(' ').trim();
    if (!key) throw new Error('Usage: del <key>');
    return {op: 'delete', key};
  }
  if (op === 'put') {
    const key = rest.shift();
    const value = rest.join(' ');
    if (!key || !rest.length) throw new Error('Usage: put <key> <value>');
    return {op, key, value};
  }
  throw new Error('Supported commands: get <key>, del <key>, put <key> <value>');
}

async function runProductionCommand() {
  els.prodRunCommand.disabled = true;
  try {
    const cmd = parseProductionCommand(els.prodCommand.value);
    if (cmd.op === 'get') {
      const result = await postJson('/api/get', {key: cmd.key});
      if (result.found) {
        const value = String(result.value || '');
        appendProductionOutput(`FOUND ${cmd.key} | ${value.slice(0, 500)}${value.length > 500 ? '...' : ''}`);
      } else {
        appendProductionOutput(`NOT FOUND ${cmd.key}`);
      }
    } else if (cmd.op === 'delete') {
      await postJson('/api/delete', {key: cmd.key});
      appendProductionOutput(`DELETED ${cmd.key} | tombstone written`);
    } else if (cmd.op === 'put') {
      await postJson('/api/put', {key: cmd.key, value: cmd.value});
      appendProductionOutput(`PUT ${cmd.key} | ${cmd.value.length} value bytes`);
    }
    await refreshState();
    await refreshEngineTrace();
  } catch (err) {
    appendProductionOutput(`ERROR | ${err.message}`);
  } finally {
    els.prodRunCommand.disabled = false;
  }
}

function setSampleMqttGet() {
  els.prodCommand.value = 'get mqtt:factory/site_a/device/dev_0001/telemetry:1';
  els.prodCommand.focus();
}

function renderEngineTerminal(trace) {
  const lines = Array.isArray(trace.lines) ? trace.lines : [];
  els.traceToggle.textContent = trace.enabled ? 'Disable Trace' : 'Enable Trace';
  els.traceToggle.classList.toggle('primary-btn', Boolean(trace.enabled));
  els.traceToggle.classList.toggle('secondary-btn', !trace.enabled);
  els.engineTerminal.textContent = lines.length
    ? lines.join('\n')
    : (trace.enabled ? 'Trace enabled. Run operations to produce engine events.' : 'Trace disabled or no entries yet.');
  els.engineTerminal.scrollTop = els.engineTerminal.scrollHeight;
}

async function refreshEngineTrace() {
  try {
    renderEngineTerminal(await getJson('/api/trace/read'));
  } catch (err) {
    els.engineTerminal.textContent = `trace-error | ${err.message}`;
  }
}

async function toggleEngineTrace() {
  const currentlyEnabled = els.traceToggle.textContent.toLowerCase().includes('disable');
  els.traceToggle.disabled = true;
  try {
    await postJson('/api/trace/toggle', {enabled: currentlyEnabled ? 0 : 1});
    await refreshEngineTrace();
  } catch (err) {
    els.engineTerminal.textContent = `trace-toggle-error | ${err.message}`;
  } finally {
    els.traceToggle.disabled = false;
  }
}

function renderExperiment(result) {
  const status = result.status || 'fail';
  const mismatches = Number(result.mismatches || 0);

  setText('experiment-status', status.toUpperCase());
  byId('experiment-status').className = statusClass(status);
  setText('experiment-ops', fmtNum(result.generated_ops || 0));
  setText('experiment-checks', fmtNum(result.verify_checks || 0));
  setText('experiment-mismatches', fmtNum(mismatches));
  byId('experiment-mismatches').className = mismatches === 0 ? 'pass' : 'fail';

  setText('experiment-model', fmtNum(result.model_live_keys || 0));
  setText('experiment-deleted', fmtNum(result.model_deleted_keys || 0));
  setText('experiment-mem', `${fmtNum(result.memtable_entries || 0)} entries, ${fmtBytes(result.memtable_bytes || 0)} / ${fmtBytes(result.flush_threshold || 0)}`);
  setText('experiment-levels', `L0 ${fmtNum(result.l0_count || 0)} / L1 ${fmtNum(result.l1_count || 0)} / L2 ${fmtNum(result.l2_count || 0)}`);
  setText('experiment-files', `WAL ${fmtBytes(result.wal_bytes || 0)}, SST ${fmtBytes(result.sst_bytes || 0)}, VLog ${fmtBytes(result.vlog_bytes || 0)}`);
  setText('experiment-disk', fmtBytes(result.total_disk_bytes || 0));
  setText('experiment-space', fmtAmp(result.space_amplification_estimate || 0));
  setText('experiment-writeamp', fmtAmp(result.write_amplification || 0));

  const steps = Array.isArray(result.steps) ? result.steps : [];
  els.experimentSteps.innerHTML = steps.length
    ? steps.map((step) => `
        <div class="step-row ${statusClass(step.status)}">
          <div class="step-status">${String(step.status || 'fail').toUpperCase()}</div>
          <div>
            <div class="step-name">${escapeHtml(step.name || 'Unnamed step')}</div>
            <div class="step-detail">${escapeHtml(step.detail || '')}</div>
          </div>
        </div>
      `).join('')
    : '<div class="empty-state">No experiment steps were returned.</div>';

  els.experimentRaw.textContent = JSON.stringify(result, null, 2);
}

function setExperimentBusy(isBusy) {
  els.experimentRun.disabled = isBusy;
  els.experimentBasic.disabled = isBusy;
  els.experimentLsm.disabled = isBusy;
  els.experimentRun.textContent = isBusy ? 'Running...' : 'Run Experiment';
}

async function runExperiment() {
  setExperimentBusy(true);
  setText('experiment-status', 'RUNNING');
  byId('experiment-status').className = 'pending';
  try {
    const result = await postJson('/api/experiment/run', {script: els.experimentScript.value});
    renderExperiment(result);
  } catch (err) {
    renderExperiment({
      status: 'fail',
      mismatches: 1,
      steps: [{name: 'Experiment request', status: 'fail', detail: err.message}],
    });
  } finally {
    setExperimentBusy(false);
  }
}

function setExperimentPreset(kind) {
  if (kind === 'basic') {
    els.experimentScript.value = [
      'put manual:key hello',
      'get manual:key',
      'update manual:key hello_v2',
      'get manual:key',
      'delete manual:key',
      'get manual:key',
      'verify',
    ].join('\n');
    return;
  }
  els.experimentScript.value = [
    'insert 1..2000 random',
    'update 500..1200 random',
    'delete 700..900 random',
    'flush',
    'insert 2001..4000 random',
    'flush',
    'compact',
    'recover',
    'verify',
    'state',
  ].join('\n');
}

document.querySelectorAll('.tab-btn').forEach((button) => {
  button.addEventListener('click', () => {
    document.querySelectorAll('.tab-btn').forEach((btn) => btn.classList.remove('active'));
    document.querySelectorAll('.view').forEach((view) => view.classList.remove('active'));
    button.classList.add('active');
    byId(button.dataset.view).classList.add('active');
  });
});

els.refreshState.addEventListener('click', () => refreshState());
els.resetProduction.addEventListener('click', resetProductionStore);
els.prodRunCommand.addEventListener('click', runProductionCommand);
els.prodSampleGet.addEventListener('click', setSampleMqttGet);
els.prodClearOutput.addEventListener('click', () => {
  els.prodQueryOutput.textContent = 'Output cleared. Run get, put, or del commands to verify production data.';
});
els.prodCommand.addEventListener('keydown', (event) => {
  if (event.key === 'Enter' && (event.ctrlKey || event.metaKey)) {
    event.preventDefault();
    runProductionCommand();
  }
});
els.streamStart.addEventListener('click', startStream);
els.streamStop.addEventListener('click', stopStream);
els.traceToggle.addEventListener('click', toggleEngineTrace);
els.traceRefresh.addEventListener('click', refreshEngineTrace);
els.experimentRun.addEventListener('click', runExperiment);
els.experimentBasic.addEventListener('click', () => setExperimentPreset('basic'));
els.experimentLsm.addEventListener('click', () => setExperimentPreset('lsm'));

refreshState();
refreshStreamStatus();
refreshEngineTrace();
setInterval(() => {
  refreshState();
  refreshStreamStatus();
  refreshEngineTrace();
}, POLL_INTERVAL_MS);
