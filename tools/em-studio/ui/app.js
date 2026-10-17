(function () {
  'use strict';

  const POLL_INTERVAL = 150;
  const MAX_LINES = 5000;

  const state = {
    ports: [],
    order: [],
    tabs: new Map(),
    activeId: null,
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

  async function call(name, ...args) {
    const api = window.pywebview && window.pywebview.api;
    if (!api || typeof api[name] !== 'function') {
      throw new Error('后端桥尚未就绪');
    }
    const res = await api[name](...args);
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
    const nearBottom = out.scrollHeight - out.scrollTop - out.clientHeight < 28;
    out.appendChild(makeLine(text, true));
    if (nearBottom) out.scrollTop = out.scrollHeight;
  }

  function renderOutput(tab) {
    const out = $('diag-output');
    out.innerHTML = '';
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
      setDiagEnabled(false);
      return;
    }

    $('info-port').textContent = tab.port || '—';
    $('info-desc').textContent = tab.desc || '—';
    $('info-serial').textContent = tab.serial || '—';
    $('info-vidpid').textContent = tab.vidpid || '—';
    $('info-baud').textContent = String(tab.baud || 115200);

    $('info-status').className = 'st ' + statusClass(tab.status);
    $('info-status-text').textContent = statusText(tab.status);
    $('device-note').textContent = tab.note || '';

    renderOutput(tab);
    setDiagEnabled(tab.status === 'connected');
  }

  function renderAll() {
    renderTabs();
    rebuildPortSelect();
    renderActive();
    updateConnectButton();
  }

  function activate(id) {
    if (!state.tabs.has(id) || id === state.activeId) return;
    state.activeId = id;
    renderAll();
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
    updateConnectButton();
  }

  async function disconnectTab(tab) {
    try {
      await call('api_disconnect', tab.id);
      tab.status = 'closed';
      tab.note = '';
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
      };
      state.tabs.set(id, tab);
      state.order.push(id);
    } else {
      tab.status = 'connected';
      tab.note = '';
    }
    state.activeId = id;
    renderAll();
    appendLine(tab, '> 已连接 ' + port + ' · 115200');
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
  }

  function boot() {
    if (state.booted) return;
    state.booted = true;
    refreshPorts();
    startPolling();
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
