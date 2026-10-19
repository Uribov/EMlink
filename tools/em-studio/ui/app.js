(function () {
  'use strict';

  // 35ms 是为了跟住板子 100ms 半周期的快闪，避免拍频；poll 只读后端内存增量与最近灯态，不发串口命令。
  const POLL_INTERVAL = 35;
  const MAX_LINES = 5000;
  const FALLBACK_EDITOR = '还没有输出 —— 点上面的快捷命令试试';
  const SF_MAX_BY_BW = { 125: 9, 250: 10, 500: 11 };
  const PARAM_KEYS = ['role', 'node_id', 'freq', 'sf', 'bw', 'cr', 'power', 'auto_wifi_s', 'crypto'];
  const LED_TICK_MS = 50;
  const STATUS_POLL_MS = 2000;
  const FLASH_POLL_MS = 300;
  const MAV_MAX_FRAMES = 200;
  const HEARTBEAT_PAYLOAD_HEX = '000000000000000003';

  const state = {
    ports: [],
    order: [],
    tabs: new Map(),
    activeId: null,
    renderedId: null,
    mavRenderedId: null,
    bound: false,
    booted: false,
    polling: false,
    statusPolling: false,
    statusTimer: null,
    flash: { deviceId: null, running: false, timer: null },
    defaultFirmware: { path: '', exists: false, size: 0 },
  };

  function $(id) {
    return document.getElementById(id);
  }

  function showToast(message, bad) {
    const el = $('toast');
    el.textContent = message || '操作失败';
    el.classList.toggle('bad', !!bad);
    el.hidden = false;
    clearTimeout(showToast._timer);
    showToast._timer = setTimeout(function () {
      el.hidden = true;
    }, 3200);
  }

  async function callRaw(name, ...args) {
    const api = window.pywebview && window.pywebview.api;
    if (!api || typeof api[name] !== 'function') {
      throw new Error('后端桥尚未就绪');
    }
    return api[name](...args);
  }

  async function call(name, ...args) {
    const res = await callRaw(name, ...args);
    if (!res || res.ok !== true) {
      throw new Error((res && res.error) || '操作失败');
    }
    return res.data;
  }

  function statusClass(status) {
    if (status === 'connected') return 'live';
    if (status === 'error') return 'bad';
    return 'off';
  }

  function statusText(status) {
    if (status === 'connected') return '已连接';
    if (status === 'error') return '错误';
    return '未连接';
  }

  function makeLine(text, animate) {
    const el = document.createElement('div');
    el.className = 'line';
    if (text.startsWith('> ')) el.classList.add('echo');
    if (text.startsWith('[')) el.classList.add('sys');
    el.textContent = text === '' ? '\u00a0' : text;
    if (!animate) el.style.animation = 'none';
    return el;
  }

  function appendLine(tab, text) {
    text = typeof text === 'string' ? text : String(text);
    tab.lines.push(text);
    if (tab.lines.length > MAX_LINES) {
      tab.lines.splice(0, tab.lines.length - MAX_LINES);
    }
    if (tab.id !== state.activeId) return;
    const out = $('diag-output');
    const empty = out.querySelector('.out-empty');
    if (empty) empty.remove();
    const nearBottom = out.scrollHeight - out.scrollTop - out.clientHeight < 28;
    out.appendChild(makeLine(text, true));
    if (nearBottom) out.scrollTop = out.scrollHeight;
  }

  function renderOutput(tab) {
    const out = $('diag-output');
    out.innerHTML = '';
    if (!tab.lines.length) {
      const hint = document.createElement('div');
      hint.className = 'line out-empty';
      hint.textContent = FALLBACK_EDITOR;
      out.appendChild(hint);
      return;
    }
    const frag = document.createDocumentFragment();
    for (const line of tab.lines) {
      frag.appendChild(makeLine(line, false));
    }
    out.appendChild(frag);
    out.scrollTop = out.scrollHeight;
  }

  function statusLayerLevel(layer) {
    let level = 'ok';
    for (const item of (layer && layer.items) || []) {
      if (item.level === 'err') return 'err';
      if (item.level === 'warn') level = 'warn';
    }
    return level;
  }

  function statusDotClass(level) {
    if (level === 'err') return 'st bad';
    if (level === 'warn') return 'st warn';
    return 'st';
  }

  function updateStatusItems(body, layerName, items) {
    const existing = new Map();
    for (const row of body.querySelectorAll('.status-item')) {
      existing.set(row.dataset.key, row);
    }

    const seen = new Map();
    const keep = new Set();
    for (const item of items) {
      const base = layerName + '|' + (item.label || '');
      const count = seen.get(base) || 0;
      seen.set(base, count + 1);
      const key = base + '|' + count;
      keep.add(key);

      let row = existing.get(key);
      if (!row) {
        row = document.createElement('div');
        row.className = 'status-item';
        row.dataset.key = key;
        const label = document.createElement('span');
        label.className = 'sk';
        const value = document.createElement('span');
        value.className = 'sv';
        row.appendChild(label);
        row.appendChild(value);
        body.appendChild(row);
      }

      const label = row.querySelector('.sk');
      const value = row.querySelector('.sv');
      const labelText = item.label || '';
      const valueText = item.value === null || item.value === undefined ? '' : String(item.value);
      if (label.textContent !== labelText) {
        label.textContent = labelText;
        label.title = labelText;
      }
      if (value.textContent !== valueText) {
        value.textContent = valueText;
        value.title = valueText;
      }

      const level = item.level === 'err' ? 'err' : item.level === 'warn' ? 'warn' : 'ok';
      const valueClass = 'sv ' + level;
      if (value.className !== valueClass) value.className = valueClass;
    }

    for (const [key, row] of existing) {
      if (!keep.has(key)) row.remove();
    }
  }

  function renderStatusCards(tab) {
    const box = $('status-cards');
    if (!box) return;
    const data = tab && tab.statusData;
    if (!data || !Array.isArray(data.layers) || data.layers.length === 0) {
      box.hidden = true;
      return;
    }
    box.hidden = false;

    const existing = new Map();
    for (const card of box.querySelectorAll('.status-card')) {
      existing.set(card.dataset.layer, card);
    }
    const wanted = new Set();

    for (const layer of data.layers) {
      const layerName = layer && layer.name ? String(layer.name) : '';
      wanted.add(layerName);
      let card = existing.get(layerName);

      if (!card) {
        card = document.createElement('div');
        card.className = 'status-card';
        card.dataset.layer = layerName;

        const head = document.createElement('div');
        head.className = 'status-card-head';
        const dot = document.createElement('span');
        dot.className = 'st';
        dot.appendChild(document.createElement('i'));
        const name = document.createElement('span');
        name.className = 'status-layer';
        head.appendChild(dot);
        head.appendChild(name);

        const stale = document.createElement('div');
        stale.className = 'status-stale';
        stale.textContent = '更新失败';
        stale.hidden = true;

        const body = document.createElement('div');
        body.className = 'status-items';

        card.appendChild(head);
        card.appendChild(stale);
        card.appendChild(body);
        box.appendChild(card);
        existing.set(layerName, card);
      }

      const name = card.querySelector('.status-layer');
      if (name.textContent !== layerName) name.textContent = layerName;

      const dot = card.querySelector('.status-card-head .st');
      const dotClass = statusDotClass(statusLayerLevel(layer));
      if (dot.className !== dotClass) dot.className = dotClass;

      const stale = card.querySelector('.status-stale');
      const staleVisible = !!tab.statusStale;
      if (stale.hidden === staleVisible) stale.hidden = !staleVisible;

      updateStatusItems(
        card.querySelector('.status-items'),
        layerName,
        (layer && layer.items) || []
      );
    }

    for (const [layerName, card] of existing) {
      if (!wanted.has(layerName)) card.remove();
    }
  }

  async function pollStatus() {
    const tab = activeTab();
    if (!tab || tab.status !== 'connected' || tab.mode !== 'cli') {
      stopStatusPolling();
      return;
    }
    if (state.statusPolling) return;

    state.statusPolling = true;
    const id = tab.id;
    try {
      const res = await callRaw('api_status', id);
      if (!res || res.ok !== true) {
        throw new Error((res && res.error) || '状态更新失败');
      }
      const current = state.tabs.get(id);
      if (current) {
        current.statusData = res.data;
        current.statusStale = !!res.error;
        current.statusError = res.error || '';
      }
    } catch (err) {
      const current = state.tabs.get(id);
      if (current) {
        current.statusStale = true;
        current.statusError = err.message;
      }
    } finally {
      state.statusPolling = false;
      const current = state.tabs.get(id);
      if (current && current.id === state.activeId && current.status === 'connected') {
        renderStatusCards(current);
      }
    }
  }

  function stopStatusPolling() {
    if (state.statusTimer) {
      clearInterval(state.statusTimer);
      state.statusTimer = null;
    }
  }

  function startStatusPolling() {
    if (state.statusTimer) return;
    state.statusTimer = setInterval(pollStatus, STATUS_POLL_MS);
    pollStatus();
  }

  function renderTabs() {
    const bar = $('tabs');
    bar.innerHTML = '';
    for (const id of state.order) {
      const tab = state.tabs.get(id);
      if (!tab) continue;

      const el = document.createElement('div');
      el.className = 'tab' + (id === state.activeId ? ' active' : '');
      el.title = tab.port;

      const dot = document.createElement('span');
      dot.className = 'st ' + statusClass(tab.status);
      dot.appendChild(document.createElement('i'));

      const name = document.createElement('span');
      name.className = 'tab-name';
      name.textContent = tab.port;

      const label = document.createElement('span');
      label.className = 'tab-state';
      label.textContent = statusText(tab.status);

      const close = document.createElement('button');
      close.type = 'button';
      close.className = 'tab-close';
      close.textContent = '\u00d7';
      close.title = '关闭标签页（断开并移除）';

      el.appendChild(dot);
      el.appendChild(name);
      el.appendChild(label);
      el.appendChild(close);

      el.addEventListener('click', function () {
        activate(id);
      });
      close.addEventListener('click', function (ev) {
        ev.stopPropagation();
        closeTab(id);
      });

      bar.appendChild(el);
    }
  }

  function connectedPorts() {
    const ports = new Set();
    for (const tab of state.tabs.values()) {
      if (tab.status === 'connected') ports.add(tab.port);
    }
    return ports;
  }

  function makeOption(value, label, disabled) {
    const option = document.createElement('option');
    option.value = value;
    option.textContent = label;
    option.disabled = !!disabled;
    return option;
  }

  function ensureTabState(tab) {
    if (!tab) return;
    if (!(tab.changed instanceof Set)) tab.changed = new Set();
    if (typeof tab.configLoaded !== 'boolean') tab.configLoaded = false;
    if (typeof tab.configLoading !== 'boolean') tab.configLoading = false;
    if (!('config' in tab)) tab.config = null;
    if (!tab.led || typeof tab.led !== 'object') tab.led = {};
    if (!('state' in tab.led)) tab.led.state = null;
    if (!('statusData' in tab)) tab.statusData = null;
    if (typeof tab.statusStale !== 'boolean') tab.statusStale = false;
    if (typeof tab.statusError !== 'string') tab.statusError = '';
    if (tab.mode !== 'cli' && tab.mode !== 'mavlink') tab.mode = 'cli';
    if (!Array.isArray(tab.frames)) tab.frames = [];
    if (!('selectedFrame' in tab)) tab.selectedFrame = null;
    if (typeof tab.heartbeat !== 'boolean') tab.heartbeat = false;
    if (!('heartbeatTimer' in tab)) tab.heartbeatTimer = null;
    if (typeof tab.frameSeq !== 'number') tab.frameSeq = 0;
    if (!tab.flash || typeof tab.flash !== 'object') {
      tab.flash = {
        path: state.defaultFirmware.exists ? state.defaultFirmware.path : '',
        erase: false,
        progress: 0,
        state: 'idle',
        message: '',
      };
    }
  }

  function sfMaxForBw(bw) {
    return SF_MAX_BY_BW[Number(bw)] || 11;
  }

  function setParamsNote(message) {
    const el = $('params-note');
    el.textContent = message || '';
    el.hidden = !message;
  }

  function setParamsEnabled(enabled) {
    const panel = $('panel-params');
    const controls = panel.querySelectorAll('input,select,button');
    for (const control of controls) control.disabled = !enabled;
    panel.classList.toggle('disabled', !enabled);
  }

  const FREQ_DIGITS = ['cfg-freq-d1', 'cfg-freq-d2', 'cfg-freq-d3'];

  function buildFreqOptions() {
    const intSelect = $('cfg-freq-int');
    if (!intSelect.options.length) {
      for (let value = 862; value <= 930; value++) {
        intSelect.appendChild(makeOption(String(value), String(value)));
      }
    }
    for (const id of FREQ_DIGITS) {
      const select = $(id);
      if (select.options.length) continue;
      for (let value = 0; value <= 9; value++) {
        select.appendChild(makeOption(String(value), String(value)));
      }
    }
  }

  function parseFreq(value) {
    if (value === null || value === undefined) return null;
    const text = String(value).trim();
    const match = /^(\d+)(?:\.(\d*))?$/.exec(text);
    if (!match) return null;
    let integer = parseInt(match[1], 10);
    if (!Number.isFinite(integer)) return null;
    integer = Math.min(930, Math.max(862, integer));
    const decimal = (match[2] || '').padEnd(3, '0').slice(0, 3);
    if (integer === 930) {
      return { integer: '930', d1: '0', d2: '0', d3: '0' };
    }
    return {
      integer: String(integer),
      d1: decimal.charAt(0),
      d2: decimal.charAt(1),
      d3: decimal.charAt(2),
    };
  }

  function setFreqControl(value) {
    const parsed = parseFreq(value);
    const intSelect = $('cfg-freq-int');
    if (!parsed) {
      intSelect.value = '915';
      $('cfg-freq-d1').value = '0';
      $('cfg-freq-d2').value = '0';
      $('cfg-freq-d3').value = '0';
      return false;
    }
    intSelect.value = parsed.integer;
    $('cfg-freq-d1').value = parsed.d1;
    $('cfg-freq-d2').value = parsed.d2;
    $('cfg-freq-d3').value = parsed.d3;
    return true;
  }

  function readFreqControl() {
    return ($('cfg-freq-int').value || '915') +
      '.' +
      ($('cfg-freq-d1').value || '0') +
      ($('cfg-freq-d2').value || '0') +
      ($('cfg-freq-d3').value || '0');
  }

  function setNodeControl(value) {
    const number = Math.min(254, Math.max(1, parseInt(value, 10) || 1));
    $('cfg-node-range').value = String(number);
    $('cfg-node-id').value = String(number);
  }

  function setAutoWifiControl(value) {
    const number = Math.min(60, Math.max(15, parseInt(value, 10) || 30));
    $('cfg-auto-wifi-range').value = String(number);
    $('cfg-auto-wifi').value = String(number);
  }

  function setSfOptions(bw, value) {
    const select = $('cfg-sf');
    const max = sfMaxForBw(bw);
    select.innerHTML = '';
    for (let sf = 7; sf <= max; sf++) {
      select.appendChild(makeOption(String(sf), String(sf)));
    }
    let target = parseInt(value, 10);
    if (!Number.isFinite(target) || target < 7) target = 7;
    if (target > max) target = max;
    select.value = String(target);
    return target;
  }

  function updateCryptoState(value) {
    const el = $('cfg-crypto-state');
    if (value === 'on') el.textContent = '加密：开（密钥不可回读）';
    else if (value === 'off') el.textContent = '加密：关';
    else el.textContent = '加密：未知';
  }

  function renderChanged(tab) {
    const panel = $('panel-params');
    const fields = panel.querySelectorAll('.pfield');
    for (const field of fields) field.classList.remove('changed');
    const changed = (tab && tab.changed) || new Set();
    for (const key of changed) {
      const field = panel.querySelector('.pfield[data-param="' + key + '"]');
      if (field) field.classList.add('changed');
    }
    $('params-dirty').hidden = changed.size === 0;
  }

  function clearChanged(tab) {
    if (!tab) return;
    ensureTabState(tab);
    tab.changed.clear();
    if (tab.id === state.activeId) renderChanged(tab);
  }

  function markChanged(tab, key) {
    if (!tab) return;
    ensureTabState(tab);
    tab.changed.add(key);
    if (tab.id === state.activeId) renderChanged(tab);
  }

  function normalizeConfig(data) {
    const out = {};
    for (const key of PARAM_KEYS) {
      const value = data && data[key];
      out[key] = value === null || value === undefined ? null : String(value);
    }
    return out;
  }

  function resetConfigForm() {
    $('cfg-role').value = '';
    setNodeControl(1);
    setFreqControl('915.000');
    $('cfg-bw').value = '125';
    $('cfg-bw').dataset.prev = '125';
    setSfOptions(125, 7);
    $('cfg-cr').value = '5';
    $('cfg-power').value = '20';
    setAutoWifiControl(30);
    $('cfg-crypto').value = '';
    updateCryptoState(null);
    showSfBwWarning('');
  }

  function fillConfigForm(tab, resetSecret) {
    if (!tab || !tab.config) {
      resetConfigForm();
      renderChanged(tab);
      return;
    }
    const cfg = tab.config;
    const role = cfg.role;
    $('cfg-role').value = role === 'unset' || role === null || role === undefined ? '' : role;

    if (cfg.node_id !== null && cfg.node_id !== undefined) setNodeControl(cfg.node_id);
    else setNodeControl($('cfg-node-id').value);

    const bw = cfg.bw !== null && cfg.bw !== undefined
      ? Number(cfg.bw)
      : Number($('cfg-bw').value) || 125;
    $('cfg-bw').value = String(bw);
    $('cfg-bw').dataset.prev = String(bw);

    const sf = setSfOptions(bw, cfg.sf !== null && cfg.sf !== undefined
      ? cfg.sf
      : $('cfg-sf').value);

    let freqInvalid = false;
    if (cfg.freq !== null && cfg.freq !== undefined) {
      freqInvalid = !setFreqControl(cfg.freq);
    }
    if (cfg.cr !== null && cfg.cr !== undefined) $('cfg-cr').value = String(cfg.cr);
    if (cfg.power !== null && cfg.power !== undefined) $('cfg-power').value = String(cfg.power);

    if (cfg.auto_wifi_s !== null && cfg.auto_wifi_s !== undefined) {
      setAutoWifiControl(cfg.auto_wifi_s);
    } else {
      setAutoWifiControl($('cfg-auto-wifi').value);
    }

    if (resetSecret) $('cfg-crypto').value = '';
    updateCryptoState(cfg.crypto);
    showSfBwWarning('');
    renderChanged(tab);

    if (cfg.sf !== null && cfg.sf !== undefined && Number(cfg.sf) !== sf) {
      showSfBwWarning('当前 SF' + cfg.sf + ' 超出 BW' + bw + ' 的上限，已显示为 SF' + sf);
    }
    if (freqInvalid) {
      setParamsNote('频率读取异常，已显示默认值 915.000');
    }
  }

  async function fetchConfig(tab, quiet) {
    if (!tab || tab.status !== 'connected') return;
    ensureTabState(tab);
    if (tab.configLoading) return;
    tab.configLoading = true;
    if (tab.id === state.activeId) setParamsEnabled(false);
    try {
      const res = await callRaw('api_get_config', tab.id);
      if (!res || res.ok !== true) {
        throw new Error((res && res.error) || '读配置失败');
      }
      tab.config = normalizeConfig(res.data);
      tab.configLoaded = true;
      tab.changed.clear();
      if (tab.id === state.activeId) {
        fillConfigForm(tab, true);
        const notes = [];
        if (tab.config.freq !== null && tab.config.freq !== undefined &&
            !parseFreq(tab.config.freq)) {
          notes.push('频率读取异常，已显示默认值 915.000');
        }
        if (res.error) notes.push('读配置失败：' + res.error);
        setParamsNote(notes.join('；'));
      }
      if (res.error && !quiet) showToast('读配置失败：' + res.error, true);
    } catch (err) {
      tab.configLoaded = false;
      tab.config = null;
      tab.changed.clear();
      if (tab.id === state.activeId) {
        fillConfigForm(tab, true);
        setParamsNote('读配置失败：' + err.message);
      }
      if (!quiet) showToast('读配置失败：' + err.message, true);
    } finally {
      tab.configLoading = false;
      if (tab.id === state.activeId && tab.status === 'connected') setParamsEnabled(true);
    }
  }

  async function apiSet(tab, key, value) {
    if (!tab || tab.status !== 'connected') {
      showToast('设备未连接', true);
      return false;
    }
    try {
      const res = await callRaw('api_set_config', tab.id, key, value);
      if (!res || res.ok !== true) {
        throw new Error((res && res.error) || '设置失败');
      }
      if (!res.data || res.data.ok !== true) {
        showToast(res.error || '设置失败', true);
        await fetchConfig(tab, true);
        return false;
      }
      if (res.error) showToast(res.error, true);
      return true;
    } catch (err) {
      showToast(err.message, true);
      await fetchConfig(tab, true);
      return false;
    }
  }

  async function submitParam(tab, key, value) {
    if (!tab) return false;
    markChanged(tab, key);
    const ok = await apiSet(tab, key, value);
    if (ok && tab.config) tab.config[key] = String(value);
    return ok;
  }

  function showSfBwWarning(message) {
    const el = $('sf-bw-warning');
    el.textContent = message || '';
    el.hidden = !message;
  }

  async function onBwChange(tab) {
    if (!tab) return;
    const bwSelect = $('cfg-bw');
    const sfSelect = $('cfg-sf');
    const newBw = Number(bwSelect.value);
    const oldBw = Number(bwSelect.dataset.prev || (tab.config && tab.config.bw) || newBw);
    const oldSf = Number(sfSelect.value);
    const max = sfMaxForBw(newBw);
    const targetSf = Math.min(oldSf, max);
    const pairChanged = targetSf !== oldSf;

    markChanged(tab, 'bw');
    if (pairChanged) {
      markChanged(tab, 'sf');
      showSfBwWarning('BW' + newBw + ' 只支持 SF≤' + max + '，已自动从 SF' + oldSf + ' 降到 SF' + targetSf);
    } else {
      showSfBwWarning('');
    }

    let ok = false;
    if (pairChanged && newBw < oldBw) {
      ok = await apiSet(tab, 'sf', String(targetSf));
      if (ok) ok = await apiSet(tab, 'bw', String(newBw));
    } else {
      ok = await apiSet(tab, 'bw', String(newBw));
      if (ok && pairChanged) ok = await apiSet(tab, 'sf', String(targetSf));
    }

    if (ok) {
      bwSelect.value = String(newBw);
      bwSelect.dataset.prev = String(newBw);
      setSfOptions(newBw, targetSf);
      if (tab.config) {
        tab.config.bw = String(newBw);
        tab.config.sf = String(targetSf);
      }
    } else {
      showSfBwWarning('');
    }
  }

  async function saveConfig(tab, reboot) {
    if (!tab || tab.status !== 'connected') {
      showToast('设备未连接', true);
      return;
    }
    if (reboot && !window.confirm('保存并重启？板子会断开约 3 秒，稍后需要点「刷新」重连。')) {
      return;
    }
    try {
      const res = await callRaw('api_save_config', tab.id, !!reboot);
      if (!res || res.ok !== true) {
        throw new Error((res && res.error) || '保存失败');
      }
      const data = res.data || {};
      if (data.saved !== true) {
        showToast(res.error || '保存失败', true);
        return;
      }
      clearChanged(tab);
      if (data.rebooting) {
        tab.configLoaded = false;
        showToast(res.error || '板子重启中（约 3 秒），稍后点「刷新」重连', !!res.error);
      } else {
        showToast(res.error || '已保存到板子 NVS', !!res.error);
        await fetchConfig(tab, true);
      }
    } catch (err) {
      showToast(err.message, true);
    }
  }

  function formatFileSize(bytes) {
    const size = Number(bytes) || 0;
    if (size >= 1024 * 1024) return (size / (1024 * 1024)).toFixed(1) + ' MB';
    return Math.round(size / 1024) + ' KB';
  }

  function renderFlashBuildNote() {
    const note = $('flash-build-note');
    if (!note) return;
    const info = state.defaultFirmware;
    if (!info.path) {
      note.hidden = true;
      note.textContent = '';
      return;
    }
    note.hidden = false;
    note.textContent = info.exists
      ? '上次构建：' + info.path + '（' + formatFileSize(info.size) + '）'
      : '上次构建未找到：' + info.path;
  }

  async function loadDefaultFirmware() {
    try {
      const data = await call('api_default_firmware');
      state.defaultFirmware = {
        path: (data && data.path) || '',
        exists: !!(data && data.exists),
        size: (data && data.size) || 0,
      };
      if (state.defaultFirmware.exists) {
        for (const tab of state.tabs.values()) {
          ensureTabState(tab);
          if (!tab.flash.path) tab.flash.path = state.defaultFirmware.path;
        }
      }
    } catch (err) {
      state.defaultFirmware = { path: '', exists: false, size: 0 };
    }
    renderFlashBuildNote();
    renderFlashPanel();
  }

  function renderFlashPanel() {
    const tab = state.tabs.get(state.activeId);
    const busy = state.flash.running;
    const flashTab = state.flash.deviceId ? state.tabs.get(state.flash.deviceId) : null;
    const pathEl = $('flash-path');
    const erase = $('flash-erase');
    const percentEl = $('flash-percent');
    const fill = $('flash-bar-fill');
    const status = $('flash-status');
    const pick = $('btn-pick-firmware');
    const button = $('btn-flash');

    if (!tab) {
      pathEl.textContent = '未选择固件';
      pathEl.title = '';
      pathEl.classList.remove('set');
      erase.checked = false;
      erase.disabled = true;
      pick.disabled = true;
      button.disabled = true;
      button.textContent = '开始烧录';
      fill.style.width = '0%';
      percentEl.textContent = '0%';
      status.className = 'flash-status';
      status.textContent = '—';
      return;
    }

    ensureTabState(tab);
    const info = tab.flash;
    const here = busy && state.flash.deviceId === tab.id;

    pathEl.textContent = info.path || '未选择固件';
    pathEl.title = info.path || '';
    pathEl.classList.toggle('set', !!info.path);

    pick.disabled = busy;
    erase.checked = !!info.erase;
    erase.disabled = busy;
    button.disabled = busy || !info.path;
    button.textContent = here ? '烧录中…' : (busy ? '其他设备烧录中…' : '开始烧录');

    const percent = Math.max(0, Math.min(100, Number(info.progress) || 0));
    fill.style.width = percent + '%';
    percentEl.textContent = percent + '%';

    status.className = 'flash-status';
    if (busy && !here) {
      status.classList.add('running');
      status.textContent = '另一设备正在烧录' +
        (flashTab ? '（' + flashTab.port + '）' : '') + '…';
      return;
    }
    if (info.state === 'running') {
      status.classList.add('running');
      status.textContent = info.message || '烧录中…';
    } else if (info.state === 'ok') {
      status.classList.add('ok');
      status.textContent = '✅ ' + (info.message || '烧录完成');
    } else if (info.state === 'fail') {
      status.classList.add('fail');
      status.textContent = '❌ 失败：' + (info.message || '烧录失败');
    } else {
      status.textContent = info.message || '—';
    }
  }

  async function pickFirmware() {
    const tab = state.tabs.get(state.activeId);
    if (!tab || state.flash.running) return;
    try {
      const path = await call('api_pick_firmware');
      if (!path) return;
      ensureTabState(tab);
      tab.flash.path = path;
      tab.flash.progress = 0;
      tab.flash.state = 'idle';
      tab.flash.message = '';
      renderFlashPanel();
    } catch (err) {
      showToast(err.message, true);
    }
  }

  function stopFlashPolling() {
    if (!state.flash.timer) return;
    clearInterval(state.flash.timer);
    state.flash.timer = null;
  }

  async function pollFlashProgress(deviceId) {
    try {
      const data = await call('api_flash_progress', deviceId);
      const tab = state.tabs.get(deviceId);
      if (!tab) return;
      ensureTabState(tab);
      tab.flash.progress = Number(data && data.progress) || 0;
      tab.flash.state = (data && data.state) || 'idle';
      tab.flash.message = (data && data.message) || '';

      if (tab.flash.state !== 'running') {
        stopFlashPolling();
        state.flash.running = false;
        state.flash.deviceId = null;
        renderAll();
        refreshPorts();
        if (tab.flash.state === 'ok') {
          showToast(tab.flash.message || '烧录完成');
        } else if (tab.flash.state === 'fail') {
          showToast('烧录失败：' + (tab.flash.message || ''), true);
        }
        return;
      }
      renderFlashPanel();
    } catch (err) {
      stopFlashPolling();
      state.flash.running = false;
      state.flash.deviceId = null;
      const tab = state.tabs.get(deviceId);
      if (tab) {
        ensureTabState(tab);
        tab.flash.state = 'fail';
        tab.flash.message = err.message;
      }
      renderAll();
      showToast(err.message, true);
    }
  }

  function startFlashPolling() {
    stopFlashPolling();
    const deviceId = state.flash.deviceId;
    if (!deviceId) return;
    const poll = function () {
      pollFlashProgress(deviceId);
    };
    state.flash.timer = setInterval(poll, FLASH_POLL_MS);
    poll();
  }

  async function startFlash() {
    const tab = state.tabs.get(state.activeId);
    if (!tab) return;
    if (state.flash.running) {
      showToast('已有烧录任务正在进行', true);
      return;
    }
    ensureTabState(tab);
    if (!tab.flash.path) {
      showToast('请先选择 .bin 固件', true);
      return;
    }

    tab.flash.progress = 0;
    tab.flash.state = 'running';
    tab.flash.message = '准备烧录…';
    state.flash.running = true;
    state.flash.deviceId = tab.id;
    renderAll();

    try {
      await call('api_flash', tab.id, tab.flash.path, !!tab.flash.erase);
    } catch (err) {
      state.flash.running = false;
      state.flash.deviceId = null;
      tab.flash.state = 'fail';
      tab.flash.message = err.message;
      renderAll();
      showToast(err.message, true);
      return;
    }
    startFlashPolling();
  }

  function rebuildPortSelect() {
    const select = $('port-select');
    const previous = select.value;
    const active = state.tabs.get(state.activeId);
    const connected = connectedPorts();
    const free = state.ports.filter(function (p) {
      return !connected.has(p.port);
    });

    const activeConnected = !!(active && active.status === 'connected');
    select.innerHTML = '';
    if (free.length === 0 && !activeConnected) {
      select.appendChild(makeOption('', '（无可用端口）', true));
    }
    for (const info of free) {
      const label = info.desc ? info.port + ' — ' + info.desc : info.port;
      select.appendChild(makeOption(info.port, label));
    }
    if (activeConnected) {
      select.appendChild(makeOption(active.port, active.port + ' — 已连接（当前）'));
    }

    if (previous && Array.prototype.some.call(select.options, function (o) {
      return o.value === previous;
    })) {
      select.value = previous;
    } else if (active && active.status === 'connected') {
      select.value = active.port;
    } else if (select.options.length > 0 && !select.options[0].disabled) {
      select.selectedIndex = 0;
    }
  }

  function updateConnectButton() {
    const button = $('btn-connect');
    if (state.flash.running) {
      button.textContent = '烧录中…';
      button.classList.remove('primary');
      button.disabled = true;
      return;
    }
    const port = $('port-select').value;
    const active = state.tabs.get(state.activeId);
    let tab = null;
    if (port) {
      for (const candidate of state.tabs.values()) {
        if (candidate.port === port && candidate.status === 'connected') {
          tab = candidate;
          break;
        }
      }
    } else if (active && active.status === 'connected') {
      tab = active;
    }

    if (tab) {
      button.textContent = '断开';
      button.classList.remove('primary');
      button.disabled = false;
    } else {
      button.textContent = '连接';
      button.classList.add('primary');
      button.disabled = !port && !(active && active.port);
    }
  }

  function setDiagEnabled(enabled) {
    $('cmd-input').disabled = !enabled;
    $('btn-send').disabled = !enabled;
    $('cmd-input').placeholder = enabled ? '输入命令，回车发送' : '连接后可发送命令';
    const quick = document.querySelectorAll('.quick [data-cmd]');
    for (const button of quick) button.disabled = !enabled;
  }

  function renderActive() {
    const tab = state.tabs.get(state.activeId);
    $('main').hidden = !tab;
    $('empty').hidden = !!tab;
    if (!tab) {
      state.renderedId = null;
      state.mavRenderedId = null;
      updateModeButtons(null);
      $('panel-diag').hidden = false;
      $('panel-mav').hidden = true;
      $('panel-params').hidden = false;
      setDiagEnabled(false);
      setParamsEnabled(false);
      setParamsNote('');
      stopStatusPolling();
      const cards = $('status-cards');
      if (cards) cards.hidden = true;
      return;
    }

    ensureTabState(tab);
    $('info-port').textContent = tab.port || '—';
    $('info-desc').textContent = tab.desc || '—';
    $('info-serial').textContent = tab.serial || '—';
    $('info-vidpid').textContent = tab.vidpid || '—';
    $('info-baud').textContent = String(tab.baud || 115200);

    $('info-status').className = 'st ' + statusClass(tab.status);
    $('info-status-text').textContent = statusText(tab.status);
    if (state.flash.running && state.flash.deviceId === tab.id) {
      $('info-status').className = 'st live';
      $('info-status-text').textContent = '烧录中';
    }
    $('device-note').textContent = tab.note || '';

    const switched = state.renderedId !== tab.id;
    state.renderedId = tab.id;
    fillConfigForm(tab, switched);

    const isMav = tab.mode === 'mavlink';
    updateModeButtons(tab);
    $('panel-diag').hidden = isMav;
    $('panel-mav').hidden = !isMav;
    $('panel-params').hidden = isMav;
    updateLedTriangle();

    if (isMav) {
      setDiagEnabled(false);
      setParamsEnabled(false);
      stopStatusPolling();
      const cards = $('status-cards');
      if (cards) cards.hidden = true;
      renderMavPanel(tab);
      return;
    }

    renderMavPanel(null);
    renderOutput(tab);
    setDiagEnabled(tab.status === 'connected');
    setParamsEnabled(tab.status === 'connected' && !tab.configLoading);

    if (tab.status === 'connected') {
      renderStatusCards(tab);
      startStatusPolling();
    } else {
      stopStatusPolling();
      const cards = $('status-cards');
      if (cards) cards.hidden = true;
    }
  }

  function renderEmptyState() {
    const hasPorts = state.ports.length > 0;
    const main = $('empty-main');
    const hint = $('empty-hint');
    if (hasPorts) {
      main.textContent = '选择上面的串口，点「连接」开始';
      hint.textContent = '';
      hint.hidden = true;
      return;
    }
    main.textContent = '没有检测到串口设备';
    hint.textContent = '插上板子后点「刷新」';
    hint.hidden = false;
  }

  function renderAll() {
    renderTabs();
    rebuildPortSelect();
    renderEmptyState();
    renderActive();
    updateConnectButton();
    $('btn-refresh').disabled = state.flash.running;
    $('port-select').disabled = state.flash.running;
    renderFlashPanel();
  }

  function activate(id) {
    if (!state.tabs.has(id) || id === state.activeId) return;
    state.activeId = id;
    renderAll();
    const tab = state.tabs.get(id);
    if (tab && tab.mode === 'cli' && tab.status === 'connected' && !tab.configLoaded) {
      fetchConfig(tab);
    }
  }

  async function refreshPorts() {
    if (state.flash.running) {
      showToast('烧录进行中，暂时不能刷新端口', true);
      return;
    }
    try {
      const ports = await call('api_list_ports');
      state.ports = Array.isArray(ports) ? ports : [];
    } catch (err) {
      showToast(err.message, true);
      return;
    }
    rebuildPortSelect();
    renderEmptyState();
    updateConnectButton();
  }

  async function disconnectTab(tab) {
    if (state.flash.running) {
      showToast('烧录进行中，暂时不能断开', true);
      return;
    }
    try {
      await call('api_disconnect', tab.id);
      tab.status = 'closed';
      tab.note = '';
      ensureTabState(tab);
      tab.led.state = null;
      stopHeartbeat(tab);
      appendLine(tab, '[已断开]');
      renderAll();
    } catch (err) {
      showToast(err.message, true);
    }
  }

  async function connectPort(port) {
    const data = await call('api_connect', port, 115200);
    const id = data && data.device_id;
    if (!id) throw new Error('连接失败：后端未返回设备 id');

    let tab = state.tabs.get(id);
    if (!tab) {
      const info = state.ports.find(function (item) {
        return item.port === port;
      }) || {};
      tab = {
        id: id,
        port: port,
        desc: info.desc || '',
        serial: info.serial || '',
        vidpid: info.vidpid || '',
        baud: 115200,
        status: 'connected',
        note: '',
        lines: [],
        config: null,
        configLoaded: false,
        configLoading: false,
        changed: new Set(),
        statusData: null,
        statusStale: false,
        statusError: '',
      };
      state.tabs.set(id, tab);
      state.order.push(id);
    } else {
      tab.status = 'connected';
      tab.note = '';
      tab.configLoaded = false;
      tab.statusData = null;
      tab.statusStale = false;
      tab.statusError = '';
    }
    ensureTabState(tab);
    tab.led.state = null;
    tab.frames = [];
    tab.selectedFrame = null;
    tab.frameSeq = 0;
    state.mavRenderedId = null;
    state.activeId = id;
    renderAll();
    appendLine(tab, '> 已连接 ' + port + ' · 115200');
    if (tab.mode === 'cli') fetchConfig(tab);
  }

  async function onConnectClick() {
    if (state.flash.running) {
      showToast('烧录进行中，暂时不能连接/断开', true);
      return;
    }
    const port = $('port-select').value;
    const active = state.tabs.get(state.activeId);

    let connectedTab = null;
    if (port) {
      for (const tab of state.tabs.values()) {
        if (tab.port === port && tab.status === 'connected') {
          connectedTab = tab;
          break;
        }
      }
    } else if (active && active.status === 'connected') {
      connectedTab = active;
    }

    if (connectedTab) {
      await disconnectTab(connectedTab);
      return;
    }

    const target = port || (active && active.port);
    if (!target) {
      showToast('没有可用串口，请先刷新', true);
      return;
    }
    try {
      await connectPort(target);
    } catch (err) {
      showToast(err.message, true);
    }
  }

  async function closeTab(id) {
    const tab = state.tabs.get(id);
    if (!tab) return;
    if (state.flash.running && state.flash.deviceId === id) {
      showToast('烧录进行中，暂时不能关闭该标签', true);
      return;
    }
    try {
      await call('api_close_tab', id);
    } catch (err) {
      showToast(err.message, true);
      return;
    }
    stopHeartbeat(tab);
    state.tabs.delete(id);
    state.order = state.order.filter(function (item) {
      return item !== id;
    });
    if (state.activeId === id) {
      state.activeId = state.order.length ? state.order[state.order.length - 1] : null;
      state.mavRenderedId = null;
    }
    renderAll();
  }

  async function sendCommand(text) {
    const tab = state.tabs.get(state.activeId);
    if (!tab) return false;
    if (tab.status !== 'connected') {
      showToast('设备未连接', true);
      return false;
    }
    const command = (text || '').trim();
    if (!command) return false;
    try {
      await call('api_send', tab.id, command);
      return true;
    } catch (err) {
      showToast(err.message, true);
      return false;
    }
  }

  function sameLed(a, b) {
    if (!a || !b) return a === b;
    return a.mode === b.mode &&
      Number(a.r) === Number(b.r) &&
      Number(a.g) === Number(b.g) &&
      Number(a.b) === Number(b.b) &&
      Number(a.half) === Number(b.half);
  }

  async function pollAll() {
    const ids = state.order.slice();
    if (ids.length === 0) return;

    const results = await Promise.all(ids.map(async function (id) {
      try {
        const data = await call('api_poll', id);
        return { id: id, data: data };
      } catch (err) {
        return { id: id, error: err.message };
      }
    }));

    let changed = false;
    for (const result of results) {
      const tab = state.tabs.get(result.id);
      if (!tab) continue;

      if (result.error) {
        if (tab.status !== 'error' || tab.note !== result.error) {
          tab.status = 'error';
          tab.note = result.error;
          changed = true;
        }
        if (tab.heartbeat) stopHeartbeat(tab);
        continue;
      }

      const data = result.data || {};
      if (Array.isArray(data.lines)) {
        for (const line of data.lines) appendLine(tab, line);
      }
      if (data.status && data.status !== tab.status) {
        tab.status = data.status;
        changed = true;
      }
      if (typeof data.note === 'string' && data.note !== tab.note) {
        tab.note = data.note;
        if (data.status === 'error' && data.note) {
          appendLine(tab, '[' + data.note + ']');
        }
        changed = true;
      }
      ensureTabState(tab);
      if (data.led === null) {
        tab.led.state = null;
      } else if (data.led && typeof data.led === 'object' &&
                 !sameLed(tab.led.state, data.led)) {
        tab.led.state = data.led;
      }
      if (tab.status !== 'connected' && tab.heartbeat) stopHeartbeat(tab);
    }
    const mavChanged = await pollMavFrames(ids);
    if (changed || mavChanged) renderAll();
  }

  function startPolling() {
    if (state.polling) return;
    state.polling = true;
    const tick = async function () {
      await pollAll();
      setTimeout(tick, POLL_INTERVAL);
    };
    tick();
  }

  function activeTab() {
    return state.tabs.get(state.activeId);
  }

  function setTriStyle(color, period, blinking) {
    const triangle = $('led-tri');
    if (!triangle) return;
    if (triangle.dataset.color !== color) {
      triangle.dataset.color = color;
      triangle.style.setProperty('--tri-color', color);
    }
    const periodValue = period > 0 ? period + 'ms' : '';
    if (triangle.dataset.period !== periodValue) {
      triangle.dataset.period = periodValue;
      if (periodValue) triangle.style.setProperty('--tri-period', periodValue);
      else triangle.style.removeProperty('--tri-period');
    }
    const wantBlink = !!blinking;
    if (triangle.classList.contains('blink') !== wantBlink) {
      triangle.classList.toggle('blink', wantBlink);
    }
  }

  function applyLedState(tab) {
    const led = tab.led.state;
    if (!led) return false;
    const r = Number(led.r);
    const g = Number(led.g);
    const b = Number(led.b);
    const half = Number(led.half);
    if (!Number.isFinite(r) || !Number.isFinite(g) || !Number.isFinite(b)) {
      return false;
    }
    const color = 'rgb(' + r + ',' + g + ',' + b + ')';
    if (r !== 0 || g !== 0 || b !== 0) tab.led.lastColor = color;

    if (Number.isFinite(half) && half > 0) {
      /* 灭帧的黑色不能当动画颜色，沿用上一次非黑的亮帧颜色 */
      const blinkColor = (r === 0 && g === 0 && b === 0)
        ? (tab.led.lastColor || 'var(--dim)')
        : color;
      setTriStyle(blinkColor, half * 2, true);
    } else {
      setTriStyle(color, 0, false);
    }
    return true;
  }

  function updateLedTriangle() {
    const tab = state.tabs.get(state.activeId);
    if (!tab || tab.status !== 'connected') {
      setTriStyle('var(--dim)', 0, false);
      return;
    }
    ensureTabState(tab);
    if (!applyLedState(tab)) {
      setTriStyle('var(--dim)', 0, false);
    }
  }

  function updateModeButtons(tab) {
    const mode = tab ? tab.mode : 'cli';
    const buttons = document.querySelectorAll('.mode-btn');
    for (const button of buttons) {
      button.classList.toggle('active', button.dataset.mode === mode);
    }
  }

  function setMavEnabled(enabled) {
    const panel = $('panel-mav');
    panel.classList.toggle('disabled', !enabled);
    const controls = panel.querySelectorAll('input,textarea,button');
    for (const control of controls) control.disabled = !enabled;
  }

  function renderMavPanel(tab) {
    const panel = $('panel-mav');
    if (!tab || tab.mode !== 'mavlink') {
      panel.hidden = true;
      $('mav-heartbeat').checked = false;
      return;
    }
    panel.hidden = false;
    setMavEnabled(tab.status === 'connected');
    $('mav-heartbeat').checked = !!tab.heartbeat;
    if (state.mavRenderedId !== tab.id) {
      renderMavTable(tab);
      renderMavDetail(tab);
      state.mavRenderedId = tab.id;
    }
  }

  function formatFrameTime(ts) {
    const date = new Date(Number(ts) || Date.now());
    function pad(value, width) {
      return String(value).padStart(width || 2, '0');
    }
    return pad(date.getHours()) + ':' + pad(date.getMinutes()) + ':' +
      pad(date.getSeconds()) + '.' + pad(date.getMilliseconds(), 3);
  }

  function formatHex(hex) {
    if (!hex) return '';
    const parts = [];
    for (let i = 0; i < hex.length; i += 2) parts.push(hex.slice(i, i + 2));
    return parts.join(' ');
  }

  function hex16(value) {
    const number = Number(value);
    if (!Number.isFinite(number)) return '----';
    return (number & 0xFFFF).toString(16).padStart(4, '0').toUpperCase();
  }

  function frameIsText(frame) {
    if (!frame || !frame.payload_hex) return false;
    const text = frame.payload_text || '';
    if (!text) return false;
    return !/[\u0000-\u0008\u000B\u000C\u000E-\u001F\uFFFD]/.test(text);
  }

  function payloadSummary(frame) {
    if (!frame || !frame.payload_hex) return '（空）';
    if (frameIsText(frame)) {
      const text = (frame.payload_text || '').replace(/\s+/g, ' ');
      return text.length > 48 ? text.slice(0, 48) + '…' : text;
    }
    const parts = [];
    for (let i = 0; i < frame.payload_hex.length && i < 32; i += 2) {
      parts.push(frame.payload_hex.slice(i, i + 2));
    }
    return parts.join(' ') + (frame.payload_hex.length > 32 ? ' …' : '');
  }

  function makeFrameRow(tab, frame, animate) {
    const row = document.createElement('div');
    row.className = 'mav-row' +
      (animate ? ' fresh' : '') +
      (tab.selectedFrame === frame ? ' selected' : '');
    row.dataset.key = String(frame._k || '');

    const time = document.createElement('span');
    time.className = 'mav-time';
    time.textContent = formatFrameTime(frame.ts);

    const msgid = document.createElement('span');
    msgid.textContent = String(frame.msgid);

    const sysid = document.createElement('span');
    sysid.textContent = String(frame.sysid);

    const compid = document.createElement('span');
    compid.textContent = String(frame.compid);

    const len = document.createElement('span');
    len.textContent = String(frame.len);

    const payload = document.createElement('span');
    payload.className = 'mav-payload';
    payload.textContent = payloadSummary(frame);
    payload.title = payload.textContent;

    const crc = document.createElement('span');
    crc.className = frame.crc_ok ? 'crc-ok' : 'crc-bad';
    crc.textContent = frame.crc_ok ? '✓' : '✗';
    crc.title = 'recv 0x' + hex16(frame.crc_recv) + ' / calc 0x' + hex16(frame.crc_calc);

    row.appendChild(time);
    row.appendChild(msgid);
    row.appendChild(sysid);
    row.appendChild(compid);
    row.appendChild(len);
    row.appendChild(payload);
    row.appendChild(crc);

    row.addEventListener('click', function () {
      tab.selectedFrame = frame;
      const rows = $('mav-rows');
      for (const selected of rows.querySelectorAll('.mav-row.selected')) {
        selected.classList.remove('selected');
      }
      row.classList.add('selected');
      renderMavDetail(tab);
    });

    if (animate) {
      row.addEventListener('animationend', function () {
        row.classList.remove('fresh');
      }, { once: true });
    }
    return row;
  }

  function updateMavCount(tab) {
    const count = tab && Array.isArray(tab.frames) ? tab.frames.length : 0;
    const bad = tab && Array.isArray(tab.frames)
      ? tab.frames.filter(function (frame) { return !frame.crc_ok; }).length
      : 0;
    $('mav-count').textContent = count + ' 帧' + (bad ? ' · CRC ✗ ' + bad : '');
  }

  function renderMavTable(tab) {
    const rows = $('mav-rows');
    rows.innerHTML = '';
    const frag = document.createDocumentFragment();
    for (const frame of tab.frames) {
      frag.appendChild(makeFrameRow(tab, frame, false));
    }
    rows.appendChild(frag);
    rows.scrollTop = rows.scrollHeight;
    updateMavCount(tab);
  }

  function renderMavDetail(tab) {
    const box = $('mav-detail');
    const frame = tab && tab.selectedFrame;
    if (!frame) {
      box.hidden = true;
      return;
    }
    box.hidden = false;
    $('mav-detail-title').textContent = 'msgid ' + frame.msgid +
      ' · sys ' + frame.sysid + ' · comp ' + frame.compid + ' · len ' + frame.len;
    $('mav-detail-meta').textContent =
      'seq ' + frame.seq + ' · ' + (frame.crc_ok ? 'CRC ✓' : 'CRC ✗') +
      ' · recv 0x' + hex16(frame.crc_recv) +
      ' / calc 0x' + hex16(frame.crc_calc) +
      ' · crc_extra ' + frame.crc_extra;
    $('mav-detail-hex').textContent = formatHex(frame.payload_hex) || '（空）';
    if (!frame.payload_hex) {
      $('mav-detail-text').textContent = '（空）';
    } else if (frameIsText(frame)) {
      $('mav-detail-text').textContent = frame.payload_text || '（空）';
    } else {
      $('mav-detail-text').textContent = '（不可读，见 payload hex）';
    }
  }

  function addFrames(tab, frames) {
    ensureTabState(tab);
    for (const frame of frames) {
      frame._k = ++tab.frameSeq;
      tab.frames.push(frame);
    }

    let dropped = [];
    if (tab.frames.length > MAV_MAX_FRAMES) {
      dropped = tab.frames.splice(0, tab.frames.length - MAV_MAX_FRAMES);
    }
    if (tab.selectedFrame && dropped.indexOf(tab.selectedFrame) !== -1) {
      tab.selectedFrame = null;
      if (tab.id === state.activeId) renderMavDetail(tab);
    }

    if (tab.id !== state.activeId || tab.mode !== 'mavlink') return;
    if (state.mavRenderedId !== tab.id) {
      renderMavTable(tab);
      renderMavDetail(tab);
      state.mavRenderedId = tab.id;
      return;
    }

    const rows = $('mav-rows');
    for (let i = 0; i < dropped.length && rows.firstElementChild; i++) {
      rows.firstElementChild.remove();
    }
    const nearBottom = rows.scrollHeight - rows.scrollTop - rows.clientHeight < 28;
    const keep = new Set(tab.frames);
    const frag = document.createDocumentFragment();
    for (const frame of frames) {
      if (keep.has(frame)) frag.appendChild(makeFrameRow(tab, frame, true));
    }
    rows.appendChild(frag);
    if (nearBottom) rows.scrollTop = rows.scrollHeight;
    updateMavCount(tab);
  }

  async function pollMavFrames(ids) {
    const targets = [];
    for (const id of ids) {
      const tab = state.tabs.get(id);
      if (tab && tab.mode === 'mavlink' && tab.status === 'connected') {
        targets.push(id);
      }
    }
    if (!targets.length) return false;

    const results = await Promise.all(targets.map(async function (id) {
      try {
        const data = await call('api_mav_poll', id);
        return { id: id, data: data };
      } catch (err) {
        return { id: id, error: err.message };
      }
    }));

    let changed = false;
    for (const result of results) {
      const tab = state.tabs.get(result.id);
      if (!tab || result.error) continue;
      const data = result.data || {};
      if (Array.isArray(data.frames) && data.frames.length) {
        addFrames(tab, data.frames);
      }
      if (data.status && data.status !== tab.status) {
        tab.status = data.status;
        changed = true;
      }
      if (typeof data.note === 'string' && data.note !== tab.note) {
        tab.note = data.note;
        changed = true;
      }
      if (tab.status !== 'connected' && tab.heartbeat) stopHeartbeat(tab);
    }
    return changed;
  }

  async function setMode(mode) {
    const tab = activeTab();
    if (!tab) {
      showToast('先连接设备，再切换模式', true);
      return;
    }
    ensureTabState(tab);
    if (tab.mode === mode) return;
    try {
      await call('api_set_mode', tab.id, mode);
    } catch (err) {
      showToast(err.message, true);
      return;
    }

    tab.mode = mode;
    if (mode === 'mavlink') {
      tab.frames = [];
      tab.selectedFrame = null;
      tab.frameSeq = 0;
    } else {
      stopHeartbeat(tab);
    }
    state.mavRenderedId = null;
    renderAll();
    if (mode === 'cli' && tab.status === 'connected' && !tab.configLoaded) {
      fetchConfig(tab);
    }
    showToast(mode === 'mavlink'
      ? '已切到解码模式：CLI 文本丢弃'
      : '已切到配置模式：MAVLink 帧丢弃');
  }

  function startHeartbeat(tab) {
    if (!tab || tab.heartbeatTimer) return;
    tab.heartbeat = true;
    const tick = async function () {
      if (!tab.heartbeat || tab.status !== 'connected') return;
      try {
        await call('api_send_frame', tab.id, HEARTBEAT_PAYLOAD_HEX, 0, 255, 0, 0, true);
      } catch (err) {
        stopHeartbeat(tab);
        showToast('心跳发送失败：' + err.message, true);
      }
    };
    tick();
    tab.heartbeatTimer = setInterval(tick, 1000);
  }

  function stopHeartbeat(tab) {
    if (!tab) return;
    tab.heartbeat = false;
    if (tab.heartbeatTimer) {
      clearInterval(tab.heartbeatTimer);
      tab.heartbeatTimer = null;
    }
    if (tab.id === state.activeId) $('mav-heartbeat').checked = false;
  }

  function readMavInt(id, label, low, high) {
    const raw = ($(id).value || '').trim();
    if (!/^\d+$/.test(raw)) {
      showToast(label + ' 必须是整数', true);
      return null;
    }
    const value = parseInt(raw, 10);
    if (!Number.isInteger(value) || value < low || value > high) {
      showToast(label + ' 必须在 ' + low + '..' + high, true);
      return null;
    }
    return value;
  }

  async function sendMavFrame() {
    const tab = activeTab();
    if (!tab) return;
    if (tab.status !== 'connected') {
      showToast('设备未连接', true);
      return;
    }
    if (tab.mode !== 'mavlink') {
      showToast('请先切到解码模式', true);
      return;
    }
    const msgid = readMavInt('mav-msgid', 'msgid', 0, 0xFFFFFF);
    if (msgid === null) return;
    const sysid = readMavInt('mav-sysid', 'sysid', 0, 0xFF);
    if (sysid === null) return;
    const compid = readMavInt('mav-compid', 'compid', 0, 0xFF);
    if (compid === null) return;
    const target = readMavInt('mav-target', 'target', 0, 0xFF);
    if (target === null) return;

    try {
      const data = await call(
        'api_send_frame',
        tab.id,
        $('mav-payload').value,
        msgid,
        sysid,
        compid,
        target,
        $('mav-hex').checked
      );
      showToast('已发送 ' + data.bytes + ' 字节 · seq ' + data.seq +
        ' · crc_extra ' + data.crc_extra);
    } catch (err) {
      showToast(err.message, true);
    }
  }

  async function sendRequestParams() {
    const tab = activeTab();
    if (!tab) return;
    if (tab.status !== 'connected') {
      showToast('设备未连接', true);
      return;
    }
    if (tab.mode !== 'mavlink') {
      showToast('请先切到解码模式', true);
      return;
    }
    try {
      await call('api_send_frame', tab.id, '00 00', 21, 255, 0, 0, true);
      showToast('已发送 PARAM_REQUEST_LIST（msgid 21，广播）');
    } catch (err) {
      showToast(err.message, true);
    }
  }

  function bindParamEvents() {
    $('cfg-role').addEventListener('change', function () {
      const tab = activeTab();
      if (tab && this.value) submitParam(tab, 'role', this.value);
    });

    $('cfg-node-range').addEventListener('input', function () {
      $('cfg-node-id').value = this.value;
    });
    $('cfg-node-range').addEventListener('change', function () {
      const tab = activeTab();
      if (!tab) return;
      const value = Math.min(254, Math.max(1, parseInt(this.value, 10) || 1));
      setNodeControl(value);
      submitParam(tab, 'node_id', String(value));
    });
    $('cfg-node-id').addEventListener('change', function () {
      const tab = activeTab();
      if (!tab) return;
      const value = parseInt(this.value, 10);
      if (!Number.isInteger(value) || value < 1 || value > 254) {
        showToast('节点号必须是 1–254 的整数', true);
        const known = tab.config && tab.config.node_id;
        setNodeControl(known !== null && known !== undefined ? known : 1);
        return;
      }
      setNodeControl(value);
      submitParam(tab, 'node_id', String(value));
    });

    for (const id of ['cfg-freq-int'].concat(FREQ_DIGITS)) {
      $(id).addEventListener('change', function () {
        const tab = activeTab();
        if (tab) submitParam(tab, 'freq', readFreqControl());
      });
    }

    $('cfg-sf').addEventListener('change', function () {
      const tab = activeTab();
      if (tab) submitParam(tab, 'sf', this.value);
    });
    $('cfg-bw').addEventListener('change', function () {
      const tab = activeTab();
      if (tab) onBwChange(tab);
    });
    $('cfg-cr').addEventListener('change', function () {
      const tab = activeTab();
      if (tab) submitParam(tab, 'cr', this.value);
    });
    $('cfg-power').addEventListener('change', function () {
      const tab = activeTab();
      if (tab) submitParam(tab, 'power', this.value);
    });

    $('cfg-auto-wifi-range').addEventListener('input', function () {
      $('cfg-auto-wifi').value = this.value;
    });
    $('cfg-auto-wifi-range').addEventListener('change', function () {
      const tab = activeTab();
      if (!tab) return;
      const value = Math.min(60, Math.max(15, parseInt(this.value, 10) || 30));
      setAutoWifiControl(value);
      submitParam(tab, 'auto_wifi_s', String(value));
    });
    $('cfg-auto-wifi').addEventListener('change', function () {
      const tab = activeTab();
      if (!tab) return;
      const parsed = parseInt(this.value, 10);
      if (!Number.isFinite(parsed)) {
        showToast('维护延时必须是 15–60 的整数', true);
        const known = tab.config && tab.config.auto_wifi_s;
        setAutoWifiControl(known !== null && known !== undefined ? known : 30);
        return;
      }
      const value = Math.min(60, Math.max(15, parsed));
      setAutoWifiControl(value);
      submitParam(tab, 'auto_wifi_s', String(value));
    });

    $('cfg-crypto').addEventListener('change', async function () {
      const tab = activeTab();
      if (!tab) return;
      const value = this.value.trim().toLowerCase();
      if (!value) return;
      if (value !== 'off' && !/^[0-9a-f]{32}$/.test(value)) {
        showToast('加密密钥必须是 32 位 hex，或输入 off', true);
        return;
      }
      markChanged(tab, 'crypto');
      const ok = await apiSet(tab, 'crypto', value);
      if (ok) {
        if (tab.config) tab.config.crypto = value === 'off' ? 'off' : 'on';
        updateCryptoState(tab.config ? tab.config.crypto : null);
        this.value = '';
      }
    });

    $('btn-save-config').addEventListener('click', function () {
      const tab = activeTab();
      if (tab) saveConfig(tab, false);
    });
    $('btn-save-reboot').addEventListener('click', function () {
      const tab = activeTab();
      if (tab) saveConfig(tab, true);
    });
  }

  function bindEvents() {
    if (state.bound) return;
    state.bound = true;

    $('btn-refresh').addEventListener('click', refreshPorts);
    $('btn-connect').addEventListener('click', onConnectClick);
    $('port-select').addEventListener('change', updateConnectButton);

    $('cmd-form').addEventListener('submit', async function (ev) {
      ev.preventDefault();
      const input = $('cmd-input');
      const text = input.value;
      input.value = '';
      const ok = await sendCommand(text);
      if (!ok) {
        input.value = text;
        input.focus();
      }
    });

    const quick = document.querySelectorAll('.quick [data-cmd]');
    for (const button of quick) {
      button.addEventListener('click', function () {
        sendCommand(button.dataset.cmd);
      });
    }

    const modeButtons = document.querySelectorAll('.mode-btn');
    for (const button of modeButtons) {
      button.addEventListener('click', function () {
        setMode(button.dataset.mode);
      });
    }

    $('btn-mav-send').addEventListener('click', sendMavFrame);
    $('btn-mav-request').addEventListener('click', sendRequestParams);
    $('mav-payload').addEventListener('keydown', function (ev) {
      if ((ev.ctrlKey || ev.metaKey) && ev.key === 'Enter') {
        ev.preventDefault();
        sendMavFrame();
      }
    });
    $('mav-heartbeat').addEventListener('change', function () {
      const tab = activeTab();
      if (!tab) {
        this.checked = false;
        return;
      }
      if (tab.status !== 'connected' || tab.mode !== 'mavlink') {
        this.checked = false;
        showToast('连接并切到解码模式后才能发心跳', true);
        return;
      }
      if (this.checked) startHeartbeat(tab);
      else stopHeartbeat(tab);
    });

    $('btn-pick-firmware').addEventListener('click', pickFirmware);
    $('flash-erase').addEventListener('change', function () {
      const tab = activeTab();
      if (!tab) {
        this.checked = false;
        return;
      }
      ensureTabState(tab);
      tab.flash.erase = this.checked;
    });
    $('btn-flash').addEventListener('click', startFlash);

    bindParamEvents();
  }

  function boot() {
    if (state.booted) return;
    state.booted = true;
    buildFreqOptions();
    setFreqControl('915.000');
    $('cfg-bw').dataset.prev = '125';
    setSfOptions(125, 7);
    setNodeControl(1);
    setAutoWifiControl(30);
    renderChanged(null);
    setParamsEnabled(false);
    refreshPorts();
    loadDefaultFirmware();
    startPolling();
    updateLedTriangle();
    setInterval(updateLedTriangle, LED_TICK_MS);
    renderAll();
  }

  bindEvents();
  renderAll();

  if (window.pywebview && window.pywebview.api) {
    boot();
  } else {
    window.addEventListener('pywebviewready', boot, { once: true });
  }
})();
