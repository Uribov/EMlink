/* EM-SDR 前端
   频谱：白线 + 1px 网格，自动跟随底噪缩放
   瀑布：灰阶，新数据在最上面，整体往下滚
   节点：从后端 /get_nodes 拉，按 node_id 分格 */

'use strict';

const $ = (id) => document.getElementById(id);
const api = () => window.pywebview && window.pywebview.api;

/* 后端所有桥方法统一返回 {ok, data, error}，这里解包并统一处理错误 */
async function call(name, ...args) {
  const a = api();
  if (!a || typeof a[name] !== 'function') return null;
  const r = await a[name](...args);
  if (r && r.ok === false) {
    console.warn(`[api] ${name} 失败:`, r.error);
    return null;
  }
  return (r && Object.prototype.hasOwnProperty.call(r, 'data')) ? r.data : r;
}

/* ---------------- 通用 ---------------- */

function fitCanvas(cv) {
  const dpr = window.devicePixelRatio || 1;
  const rect = cv.getBoundingClientRect();
  const w = Math.max(1, Math.floor(rect.width * dpr));
  const h = Math.max(1, Math.floor(rect.height * dpr));
  if (cv.width !== w || cv.height !== h) {
    cv.width = w;
    cv.height = h;
  }
  const ctx = cv.getContext('2d');
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  return { ctx, w: rect.width, h: rect.height };
}

function fmtAge(ms) {
  if (ms === null || ms === undefined) return '—';
  if (ms < 1000) return `${ms}ms`;
  if (ms < 60000) return `${(ms / 1000).toFixed(1)}s`;
  return `${Math.round(ms / 60000)}m`;
}

/* ---------------- 频谱 ---------------- */

const specCv = $('spec-canvas');
let specRange = null;        // 平滑后的 [lo, hi]，避免每帧跳

function drawSpectrum(snap) {
  const { ctx, w, h } = fitCanvas(specCv);
  const spec = snap.spec || [];
  ctx.clearRect(0, 0, w, h);

  // 网格：竖 8 格、横 4 格，1px 发丝线
  ctx.strokeStyle = '#1a1a1a';
  ctx.lineWidth = 1;
  for (let i = 1; i < 8; i++) {
    const x = Math.round((w * i) / 8) + 0.5;
    ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, h); ctx.stroke();
  }
  for (let i = 1; i < 4; i++) {
    const y = Math.round((h * i) / 4) + 0.5;
    ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(w, y); ctx.stroke();
  }

  if (!spec.length) {
    ctx.fillStyle = '#4a4a4a';
    ctx.font = '11px ui-monospace, Menlo, Consolas, monospace';
    ctx.fillText('等待采集…', 12, h / 2);
    return;
  }

  // 纵向范围：故意拉开。底噪往下 12 dB、往上 45 dB（不够容纳峰值时再扩）。
  // 范围越大，噪声起伏在图上的占比越小 —— 看起来只有一点点，信号峰才跳得出来。
  const lo = snap.floor - 12;
  const hi = Math.max(snap.floor + 45, snap.peak + 3);
  if (!specRange) specRange = [lo, hi];
  specRange[0] += (lo - specRange[0]) * 0.15;
  specRange[1] += (hi - specRange[1]) * 0.15;

  const [rLo, rHi] = specRange;
  const span = Math.max(rHi - rLo, 1);

  // 底噪基线
  const floorY = h - ((snap.floor - rLo) / span) * h;
  if (floorY > 0 && floorY < h) {
    ctx.strokeStyle = '#2c2c2c';
    ctx.setLineDash([2, 4]);
    ctx.beginPath();
    ctx.moveTo(0, Math.round(floorY) + 0.5);
    ctx.lineTo(w, Math.round(floorY) + 0.5);
    ctx.stroke();
    ctx.setLineDash([]);
  }

  // 频谱：密密麻麻的柱状图。每根柱子上沿 = 该频点的 dBFS，
  // 柱宽自适应画布（256 个点铺满，中间留 1px 缝隙，读起来像 SDR 的密集竖线）
  const n = spec.length;
  const barStep = w / n;
  const barW = Math.max(1, barStep - 1);

  for (let i = 0; i < n; i++) {
    const x = i * barStep;
    const y = h - ((spec[i] - rLo) / span) * h;
    const top = Math.max(0, Math.min(h, y));
    if (h - top < 1) continue;

    // 超过底噪一定幅度的柱子用亮白，其余用中灰 —— 一眼能分出"有信号"和"只有噪声"
    const strong = spec[i] > snap.floor + 6;
    ctx.fillStyle = strong ? '#ededed' : '#5a5a5a';
    ctx.fillRect(x, top, barW, h - top);
  }

  // 中心频率刻度
  ctx.fillStyle = '#4a4a4a';
  ctx.font = '9px ui-monospace, Menlo, Consolas, monospace';
  ctx.fillText(`${snap.floor.toFixed(0)} dBFS 底噪`, 8, 12);
  ctx.fillText(`${snap.peak.toFixed(0)} dBFS 峰值`, 8, 24);
}

/* ---------------- 瀑布 ---------------- */

const wfCv = $('wf-canvas');
let wfImg = null;
let wfRows = [];                 // 前端自己累积瀑布行
const WF_MAX_ROWS = 200;

function drawWaterfall(snap) {
  // 后端每帧只发最新一行，这里往下累积
  if (snap.row && snap.row.length) {
    wfRows.push(snap.row);
    if (wfRows.length > WF_MAX_ROWS) wfRows.shift();
  }

  const { ctx, w, h } = fitCanvas(wfCv);
  const rows = wfRows;

  // 行数或宽度变了就重建离屏图
  const needW = Math.floor(w);
  const needH = rows.length;
  if (!wfImg || wfImg.width !== needW || wfImg.height !== needH) {
    if (needW > 0 && needH > 0) {
      wfImg = ctx.createImageData(needW, needH);
    } else {
      wfImg = null;
    }
  }
  wfRows = rows;

  ctx.clearRect(0, 0, w, h);

  if (!wfImg || !rows.length) {
    ctx.fillStyle = '#4a4a4a';
    ctx.font = '11px ui-monospace, Menlo, Consolas, monospace';
    ctx.fillText('等待采集…', 12, h / 2);
    return;
  }

  // 灰阶映射：0 → 近黑，255 → 白
  const gw = wfImg.width;
  const data = wfImg.data;
  for (let y = 0; y < rows.length; y++) {
    const row = rows[y];
    const rn = row.length;
    const base = y * gw * 4;
    for (let x = 0; x < gw; x++) {
      const si = Math.min(rn - 1, Math.floor((x / gw) * rn));
      const v = row[si];
      const p = base + x * 4;
      // 轻微提亮低端，避免整片死黑
      const g = Math.min(255, Math.round(v * 1.15));
      data[p] = g;
      data[p + 1] = g;
      data[p + 2] = g;
      data[p + 3] = 255;
    }
  }

  // 把离屏图缩放到 canvas 实际高度（行数少时会被拉高，保持像素感）
  const tmp = document.createElement('canvas');
  tmp.width = wfImg.width;
  tmp.height = wfImg.height;
  tmp.getContext('2d').putImageData(wfImg, 0, 0);
  ctx.imageSmoothingEnabled = false;
  ctx.drawImage(tmp, 0, 0, w, h);
}

/* ---------------- 节点 ---------------- */

const grid = $('node-grid');
let lastNodeKey = '';

function renderNodes(snap) {
  const nodes = snap.nodes || [];
  const hint = $('nodes-hint');

  if (!nodes.length) {
    hint.textContent = '等待数据';
    if (lastNodeKey !== '') {
      grid.innerHTML = '<div class="empty">还没有收到任何节点的帧</div>';
      lastNodeKey = '';
    }
    return;
  }

  hint.textContent = `${nodes.length} 个 · 共 ${snap.total} 帧`;

  // 只在内容变化时重绘，避免每 500ms 闪烁
  const key = nodes.map((n) =>
    `${n.node_id}:${n.frames}:${n.age_ms > 5000}:${n.last_len}`).join('|');
  if (key === lastNodeKey) return;
  lastNodeKey = key;

  const maxFrames = Math.max(...nodes.map((n) => n.frames), 1);

  grid.innerHTML = nodes.map((n) => {
    const dead = !n.alive;
    const pct = Math.max(4, Math.round((n.frames / maxFrames) * 100));
    const types = Object.entries(n.types || {})
      .map(([k, v]) => `<span class="tag">${k} ${v}</span>`).join('');
    const rssi = n.rssi === null ? '—' : `${n.rssi.toFixed(0)}`;
    const snr = n.snr === null ? '—' : `${n.snr.toFixed(0)}`;
    return `
      <div class="node ${dead ? 'dead' : ''}" data-node="${n.node_id}">
        <div class="hd">
          <div class="id"><small>节点</small>${n.node_id}</div>
          <span class="dot"></span>
        </div>
        <div class="bar"><i style="width:${pct}%"></i></div>
        <div class="meta">
          <span>帧 <b>${n.frames}</b></span>
          <span>SNR <b>${snr}</b></span>
          <span>dBFS <b>${rssi}</b></span>
        </div>
        <div class="meta">
          <span>长度 <b>${n.last_len}</b></span>
          <span class="age">${fmtAge(n.age_ms)} 前</span>
        </div>
        ${types ? `<div class="tags">${types}</div>` : ''}
      </div>`;
  }).join('');
}

/* ---------------- 节点详情 ---------------- */

let detailNode = null;
let lastLogKey = '';

function openDetail(nodeId) {
  detailNode = nodeId;
  lastLogKey = '';
  $('panel-nodes').hidden = true;
  $('panel-detail').hidden = false;
  $('detail-id').textContent = String(nodeId);
  $('detail-log').innerHTML = '<div class="line out-empty">等待该节点的帧…</div>';
  tickDetail();
}

function closeDetail() {
  detailNode = null;
  $('panel-nodes').hidden = false;
  $('panel-detail').hidden = true;
}

function renderLog(entries) {
  const box = $('detail-log');
  if (!entries || !entries.length) {
    if (lastLogKey !== 'empty') {
      box.innerHTML = '<div class="line out-empty">等待该节点的帧…</div>';
      lastLogKey = 'empty';
    }
    return;
  }

  const key = `${entries.length}:${entries[entries.length - 1].t}`;
  if (key === lastLogKey) return;
  lastLogKey = key;

  const atBottom = box.scrollHeight - box.scrollTop - box.clientHeight < 40;

  box.innerHTML = entries.map((e) => {
    const frag = e.frag ? ` frag=${e.frag}` : '';
    const ack = e.ack ? ' ack' : '';
    const enc = e.enc ? ' enc' : '';
    const hex = e.hex ? `<div class="line"><span class="k">payload</span> <span class="hex">${e.hex}</span></div>` : '';
    return `<div class="line">` +
      `<span class="t">${e.t}.${String(e.ms).padStart(3, '0')}</span> ` +
      `<span class="role">${e.role.padEnd(5, ' ')}</span> ` +
      `<span class="k">src</span>=<span class="v">${e.src}</span> ` +
      `<span class="k">relay</span>=<span class="v">${e.relay}</span> ` +
      `<span class="k">ttl</span>=<span class="v">${e.ttl}</span> ` +
      `<span class="k">seq</span>=<span class="v">${e.seq}</span> ` +
      `<span class="k">${e.proto}</span> ` +
      `<span class="k">len</span>=<span class="v">${e.len}</span>` +
      `<span class="k"> qos</span>=<span class="v">${e.qos}</span>${ack}${enc}${frag} ` +
      `<span class="k">snr</span>=<span class="v">${e.snr}</span>` +
      `</div>${hex}`;
  }).join('');

  if (atBottom) box.scrollTop = box.scrollHeight;
}

async function tickDetail() {
  if (detailNode === null) return;
  const d = await call('get_node_detail', detailNode);
  if (!d || !d.node) return;
  const n = d.node;
  $('det-frames').textContent = n.frames;
  $('det-snr').textContent = n.snr === null ? '—' : n.snr;
  $('det-dbfs').textContent = n.rssi === null ? '—' : n.rssi;
  $('det-age').textContent = fmtAge(n.age_ms);
  $('detail-hint').textContent = n.alive ? '在线' : '已失联';
  renderLog(d.log);
}

grid.addEventListener('click', (e) => {
  const card = e.target.closest('.node');
  if (card && card.dataset.node) openDetail(Number(card.dataset.node));
});

$('btn-back').addEventListener('click', closeDetail);

/* ---------------- 左栏控件 ---------------- */

const GAIN_STEPS = [0.0, 0.9, 1.4, 2.7, 3.7, 7.7, 8.7, 12.5, 14.4, 15.7,
  16.6, 19.7, 20.7, 22.9, 25.4, 28.0, 29.7, 32.8, 33.8, 36.4,
  37.2, 38.6, 40.2, 42.1, 43.4, 43.9, 44.5, 48.0, 49.6];

const freqSlider = $('freq-slider');
const gainSlider = $('gain-slider');

function paintFreq() {
  const hz = Number(freqSlider.value);
  $('freq-val').textContent = (hz / 1e6).toFixed(3);
  document.querySelectorAll('.mini').forEach((b) => {
    b.classList.toggle('act', Number(b.dataset.freq) === hz);
  });
}

function paintGain() {
  const idx = Number(gainSlider.value);
  $('gain-val').textContent = GAIN_STEPS[idx].toFixed(1);
}

freqSlider.addEventListener('input', paintFreq);
gainSlider.addEventListener('input', paintGain);

// 拖动时不重启 rtl_sdr，松手才生效
freqSlider.addEventListener('change', async () => {
  await call('set_freq', Number(freqSlider.value));
});

gainSlider.addEventListener('change', async () => {
  await call('set_gain', GAIN_STEPS[Number(gainSlider.value)]);
});

document.querySelectorAll('.mini').forEach((b) => {
  b.addEventListener('click', async () => {
    const hz = Number(b.dataset.freq);
    freqSlider.value = hz;
    paintFreq();
    await call('set_freq', hz);
  });
});

/* ---------------- 启停 ---------------- */

let running = false;

async function toggleRun() {
  const btn = $('btn-run');
  btn.disabled = true;
  try {
    if (!running) {
      const st = await call('start');
      running = !!(st && st.running);
    } else {
      await call('stop');
      running = false;
    }
  } finally {
    btn.disabled = false;
  }
}

$('btn-run').addEventListener('click', toggleRun);

function paintRunState(st) {
  const isRun = !!st.running;
  const stRun = $('st-run');
  stRun.classList.toggle('on', isRun);
  stRun.classList.toggle('bad', st.state === 'error');
  $('btn-run').textContent = isRun ? '停止' : '启动';

  $('st-floor').textContent = st.floor !== undefined ? st.floor : '—';
  $('st-samples').textContent = st.seconds !== undefined
    ? `${st.seconds}s` : '—';
  $('st-error').textContent = st.error || '';
  running = isRun;
}

/* ---------------- 轮询 ---------------- */

async function tickFast() {
  const snap = await call('get_spectrum');
  if (!snap) return;
  try {
    drawSpectrum(snap);
    drawWaterfall(snap);
    $('st-floor').textContent = snap.floor;
    $('st-peak').textContent = snap.peak;
  } catch (e) { /* 窗口关闭时会抛，忽略 */ }
}

async function tickSlow() {
  const st = await call('status');
  if (st) paintRunState(st);
  const ns = await call('get_nodes');
  if (ns) {
    renderNodes(ns);
    $('hdr-frames').textContent = `${ns.total} 帧`;
    $('hdr-nodes').textContent = `${(ns.nodes || []).length} 节点`;
    $('st-total').textContent = ns.total;
    $('st-fail').textContent = ns.fail;
  }
  if (st) $('st-samples').textContent = `${st.seconds}s`;

  // 详情页打开时同步刷新它的日志
  if (detailNode !== null) tickDetail();
}

function boot() {
  paintFreq();
  paintGain();
  setInterval(tickFast, 40);       // 25 fps（后端每帧只发一行，够快也够轻）
  setInterval(tickSlow, 500);      // 2 fps
  window.addEventListener('resize', () => {
    specRange = null;            // 尺寸变了，重新算纵向范围
    drawSpectrum({ spec: [], floor: -100, peak: -80 });
    drawWaterfall({});
  });
}

if (window.pywebview && window.pywebview.api) {
  boot();
} else {
  window.addEventListener('pywebviewready', boot);
}
