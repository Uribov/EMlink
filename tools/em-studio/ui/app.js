(function () {
  'use strict';

  // 35ms 是为了跟住板子 100ms 半周期的快闪，避免拍频；poll 只读后端内存增量与最近灯态，不发串口命令。
  const POLL_INTERVAL = 35;
  const MAX_LINES = 5000;
  const FALLBACK_EDITOR = '还没有输出 —— 点上面的快捷命令试试';
  const SF_MAX_BY_BW = { 125: 9, 250: 10, 500: 11 };
  const PARAM_KEYS = ['role', 'node_id', 'freq', 'sf', 'bw', 'cr', 'power', 'auto_wifi_s', 'crypto'];
  const LED_TICK_MS = 50;

  const state = {
    ports: [],
    order: [],
    tabs: new Map(),
    activeId: null,
    renderedId: null,
    bound: false,
    booted: false,
    polling: false,
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
      setDiagEnabled(false);
      setParamsEnabled(false);
      setParamsNote('');
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
    $('device-note').textContent = tab.note || '';

    const switched = state.renderedId !== tab.id;
    state.renderedId = tab.id;
    fillConfigForm(tab, switched);

    renderOutput(tab);
    setDiagEnabled(tab.status === 'connected');
    setParamsEnabled(tab.status === 'connected' && !tab.configLoading);
    updateLedTriangle();
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
  }

  function activate(id) {
    if (!state.tabs.has(id) || id === state.activeId) return;
    state.activeId = id;
    renderAll();
    const tab = state.tabs.get(id);
    if (tab && tab.status === 'connected' && !tab.configLoaded) fetchConfig(tab);
  }

  async function refreshPorts() {
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
    try {
      await call('api_disconnect', tab.id);
      tab.status = 'closed';
      tab.note = '';
      ensureTabState(tab);
      tab.led.state = null;
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
      };
      state.tabs.set(id, tab);
      state.order.push(id);
    } else {
      tab.status = 'connected';
      tab.note = '';
      tab.configLoaded = false;
    }
    ensureTabState(tab);
    tab.led.state = null;
    state.activeId = id;
    renderAll();
    appendLine(tab, '> 已连接 ' + port + ' · 115200');
    fetchConfig(tab);
  }

  async function onConnectClick() {
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
    try {
      await call('api_close_tab', id);
    } catch (err) {
      showToast(err.message, true);
      return;
    }
    state.tabs.delete(id);
    state.order = state.order.filter(function (item) {
      return item !== id;
    });
    if (state.activeId === id) {
      state.activeId = state.order.length ? state.order[state.order.length - 1] : null;
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
    }
    if (changed) renderAll();
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
