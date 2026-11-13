/* MiniHub LCD UI editor - front end.
 *
 * Edits the Brookesia JSON assets under components/minihub_lcd/ui/ through a
 * tree: left = structure, right = properties of the selected node.
 *
 * The authoritative check is the contract suite run by the server on save
 * (POST /api/save is transactional - it rolls back when the suite fails), so
 * this file never re-implements the contract rules.
 */
'use strict';

const S = {
  root: null,          // root.json
  assets: {},          // "screens/overview.json" -> parsed asset
  contract: null,      // ui_contract.json (read-only reference)
  sel: { asset: null, path: [] },
  dirty: false,
  collapsed: new Set(),
  mode: 'edit',        // 'edit' | 'preview'
  previewAsset: null,  // which screen the preview shows
  screenProfile: null, // base geometry / density read from the firmware
  previewWidth: null,  // null = the firmware's current resolution
  showFixed: false,    // overlay pin_shell_geometry()'s unscaled px boxes
};

const COLOR_KEYS = ['bgColor', 'textColor', 'borderColor'];

/* ---------------------------------------------------------------- helpers */

function el(tag, cls, text) {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  if (text !== undefined && text !== null) e.textContent = text;
  return e;
}

function section(title) {
  const s = el('div', 'section');
  s.appendChild(el('h2', null, title));
  return s;
}

function setStatus(text, kind) {
  const s = document.getElementById('status');
  s.textContent = text;
  s.className = kind || '';
}

function markDirty() {
  S.dirty = true;
  setDirtyMark('● 未保存');
}

/** The dirty marker lives in the header; never let its absence break a render. */
function setDirtyMark(text) {
  const marker = document.getElementById('dirty');
  if (marker) marker.textContent = text;
}

/* ------------------------------------------------------------- data model */

function rootNodeOf(asset) {
  return asset && asset.type === 'viewTemplate' ? asset.node : asset;
}

/** path = array of child indices; [] means the asset root node. */
function nodeAt(assetPath, path) {
  let node = rootNodeOf(S.assets[assetPath]);
  for (const idx of path) {
    if (!node) return null;
    node = (node.children || [])[idx];
  }
  return node || null;
}

function parentOf(assetPath, path) {
  if (!path.length) return null;
  return nodeAt(assetPath, path.slice(0, -1));
}

/**
 * Children of a node, or null when it has none.
 *
 * The panel must NEVER create a container just by looking at a node: adding
 * an empty `"children": []` / `"events": []` to a node the user merely
 * clicked would end up in the file on the next save. Containers are created
 * on write, not on read.
 */
function childrenOf(node) {
  if (!node || !Array.isArray(node.children)) return null;
  return node.children;
}

/** Attach `key` to `node` only now that something is really written into it. */
function attach(node, key, initial) {
  if (!node[key]) node[key] = initial;
  return node[key];
}

function pathKey(assetPath, path) {
  return assetPath + '#' + path.join('.');
}

function knownActions() {
  const set = new Set();
  const c = S.contract || {};
  for (const v of Object.values(c.actions || {})) set.add(v);
  for (const spec of Object.values(c.action_patterns || {})) {
    if (spec && spec.format) set.add(spec.format);
  }
  for (const asset of Object.values(S.assets || {})) {
    if (asset.type === 'screenFlow') {
      for (const tr of asset.transitions || []) if (tr.action) set.add(tr.action);
    }
    collectEventActions(asset, set);
  }
  return [...set].sort();
}

function collectEventActions(node, out) {
  if (!node || typeof node !== 'object') return;
  if (Array.isArray(node)) {
    node.forEach((n) => collectEventActions(n, out));
    return;
  }
  for (const ev of node.events || []) if (ev.action) out.add(ev.action);
  for (const child of node.children || []) collectEventActions(child, out);
}

/* ------------------------------------------------------------------- tree */

function renderTree() {
  const box = document.getElementById('tree');
  box.innerHTML = '';
  const groups = new Map();
  for (const rel of S.root.assets || []) {
    const folder = rel.includes('/') ? rel.split('/')[0] : '(root)';
    if (!groups.has(folder)) groups.set(folder, []);
    groups.get(folder).push(rel);
  }
  for (const [folder, list] of groups) {
    box.appendChild(el('div', 'group-name', folder));
    for (const rel of list) box.appendChild(assetItem(rel));
  }
  const dl = document.getElementById('action-list');
  dl.innerHTML = '';
  for (const a of knownActions()) {
    const o = document.createElement('option');
    o.value = a;
    dl.appendChild(o);
  }
}

function assetItem(rel) {
  const asset = S.assets[rel];
  const wrap = el('div', 'asset');
  if (!asset) {
    const miss = el('div', 'node');
    miss.appendChild(el('span', 'tw', '·'));
    miss.appendChild(el('span', 'type', 'missing'));
    miss.appendChild(el('span', 'id missing', rel));
    wrap.appendChild(miss);
    return wrap;
  }

  const head = el('div', 'node');
  if (S.sel.asset === rel && !S.sel.path.length) head.classList.add('sel');
  head.appendChild(el('span', 'tw', '▾'));
  head.appendChild(el('span', 'type', asset.type || '?'));
  head.appendChild(el('span', 'id', '#' + (asset.id || '')));
  if (asset.type === 'viewScreen' && asset.mount_mode) {
    head.appendChild(el('span', 'badge', asset.mount_mode));
  }
  head.onclick = () => select(rel, []);
  wrap.appendChild(head);

  const kids = el('div', 'kids');
  const root = rootNodeOf(asset);
  if (root && Array.isArray(root.children)) {
    root.children.forEach((_, i) => kids.appendChild(nodeItem(rel, [i])));
  }
  wrap.appendChild(kids);
  return wrap;
}

function nodeItem(assetPath, path) {
  const node = nodeAt(assetPath, path);
  const row = el('div', 'node');
  if (S.sel.asset === assetPath && path.join('.') === S.sel.path.join('.')) {
    row.classList.add('sel');
  }
  const hasKids = Array.isArray(node.children) && node.children.length > 0;
  const tw = el('span', 'tw', hasKids ? '▾' : '');
  row.appendChild(tw);
  row.appendChild(el('span', 'type', (node && node.type) || '?'));
  const idSpan = el('span', 'id' + (node && node.id ? '' : ' missing'),
    (node && node.id) ? '#' + node.id : '(无 id)');
  row.appendChild(idSpan);
  if (node && node.events && node.events.length) {
    row.appendChild(el('span', 'badge', '⚡' + node.events.length));
  }
  row.onclick = (ev) => {
    ev.stopPropagation();
    select(assetPath, path);
  };
  const holder = el('div');
  holder.appendChild(row);
  if (hasKids) {
    const kids = el('div', 'kids');
    node.children.forEach((_, i) => kids.appendChild(nodeItem(assetPath, path.concat(i))));
    holder.appendChild(kids);
  }
  return holder;
}

function select(assetPath, path) {
  const before = JSON.stringify(S.assets[assetPath]);
  S.sel = { asset: assetPath, path: path.slice() };
  if (S.assets[assetPath] && S.assets[assetPath].type === 'viewScreen') {
    S.previewAsset = assetPath;
  }
  renderTree();
  renderMode();
  warnIfMutated(assetPath, before);
}

/* ------------------------------------------------------------- preview pane */

function setMode(mode) {
  S.mode = mode;
  document.getElementById('mode-edit').classList.toggle('on', mode === 'edit');
  document.getElementById('mode-preview').classList.toggle('on', mode === 'preview');
  renderMode();
}

function renderMode() {
  const panel = document.getElementById('panel');
  if (S.mode === 'preview') renderPreviewPane(panel);
  else renderPanel();
}

function renderPreviewPane(panel) {
  panel.innerHTML = '';
  const screens = Object.keys(S.assets)
    .filter((k) => S.assets[k].type === 'viewScreen');
  if (!screens.length) {
    panel.appendChild(el('div', 'hint', '没有可预览的屏'));
    return;
  }
  const key = S.previewAsset && screens.includes(S.previewAsset)
    ? S.previewAsset : screens[0];
  S.previewAsset = key;

  // The screen profile comes from minihub_lcd_app.cpp through /api/ui.
  // Choosing another target width == setting env.density on a same-aspect
  // panel, so every dp/sp in preview.js scales with it.
  pvSetProfile(S.screenProfile, S.previewWidth);

  const bar = el('div', 'pv-bar');
  for (const s of screens) {
    const b = el('button', s === key ? 'on' : null, S.assets[s].id || s);
    b.onclick = () => { S.previewAsset = s; renderMode(); };
    bar.appendChild(b);
  }
  panel.appendChild(bar);

  const resBar = el('div', 'pv-bar');
  resBar.appendChild(el('span', 'pv-res', '目标屏宽'));
  const baseW = PV.base.width;
  const baseH = PV.base.height;
  const deviceW = Math.round(baseW * PV.base.firmwareDensity);

  const pick = document.createElement('select');
  const widths = Array.from(new Set([deviceW, 480, 640, 800, 960, 1280, PV.W]))
    .sort((a, b) => a - b);
  for (const w of widths) {
    const opt = document.createElement('option');
    opt.value = String(w);
    opt.textContent = w + '×' + Math.round(w * baseH / baseW)
      + (w === deviceW ? ' (设备当前)' : '');
    if (w === PV.W) opt.selected = true;
    pick.appendChild(opt);
  }
  pick.onchange = () => { S.previewWidth = Number(pick.value); renderMode(); };
  resBar.appendChild(pick);

  const custom = document.createElement('input');
  custom.type = 'number';
  custom.min = '160';
  custom.max = '3840';
  custom.step = '16';
  custom.placeholder = '自定义';
  resBar.appendChild(custom);
  const apply = el('button', null, '应用');
  apply.onclick = () => {
    const w = Number(custom.value);
    if (w >= 160 && w <= 3840) { S.previewWidth = Math.round(w); renderMode(); }
  };
  resBar.appendChild(apply);

  const fixedLabel = document.createElement('label');
  const fixedCb = document.createElement('input');
  fixedCb.type = 'checkbox';
  fixedCb.checked = S.showFixed;
  fixedCb.onchange = () => { S.showFixed = fixedCb.checked; renderMode(); };
  fixedLabel.appendChild(fixedCb);
  fixedLabel.appendChild(document.createTextNode('显示 C++ 固定几何'));
  resBar.appendChild(fixedLabel);
  panel.appendChild(resBar);

  const note = el('div', 'pv-note');
  note.textContent = '模拟 ' + PV.W + '×' + PV.H + ' · density '
    + PV.density.toFixed(2) + ' · 字号 ×' + pvFontFactor().toFixed(2)
    + '：dp/sp 已按 Brookesia 的 density 与 font_scale 缩放，基准几何来自 '
    + (PV.base.source || '默认值') + '。'
    + '橙色虚线是 pin_shell_geometry() 用裸 px 钉死的位置，density≠1 时会与真实布局错开。'
    + '图标位图与 CJK 字体在设备上不随 density 缩放，本预览未建模；'
    + '字体仍用浏览器字体，用来看布局而非逐像素效果。点画面元素可跳到左树对应节点。';
  panel.appendChild(note);

  const wrap = el('div', 'pv-wrap');
  const stage = pvBuildScreen(key, S.assets, { showFixed: S.showFixed });
  wrap.appendChild(stage);
  for (const node of stage.querySelectorAll('.pv-node')) {
    node.onclick = (ev) => {
      ev.stopPropagation();
      const raw = node.dataset.pvPath || '';
      const path = raw ? raw.split('.').map(Number) : [];
      select(key, path);
      setMode('edit');
    };
  }
  panel.appendChild(wrap);
}

/**
 * Rendering must never change the model.
 *
 * A stray `"events": []` / `"children": []` added by the panel while merely
 * LOOKING at a node would silently land in the file on the next save. Catch
 * it here, loudly, instead of weeks later in a diff.
 */
function warnIfMutated(assetPath, before) {
  if (before === JSON.stringify(S.assets[assetPath])) return;
  console.error('渲染修改了模型，保存会把噪声写进文件:', assetPath);
  setStatus('内部错误：渲染修改了模型', 'err');
}

/* ---------------------------------------------------------- property panel */

function renderPanel() {
  const p = document.getElementById('panel');
  p.innerHTML = '';
  if (!S.sel.asset || !S.assets[S.sel.asset]) {
    p.appendChild(el('div', 'hint', '从左侧选择一个节点'));
    return;
  }
  const asset = S.assets[S.sel.asset];
  const node = nodeAt(S.sel.asset, S.sel.path);

  const crumb = el('div', 'crumb');
  crumb.innerHTML = '';
  crumb.appendChild(el('b', null, S.sel.asset));
  crumb.appendChild(document.createTextNode(' › '));
  const trail = [];
  let cur = rootNodeOf(asset);
  for (const i of S.sel.path) {
    cur = (cur.children || [])[i];
    trail.push((cur && cur.id) || '[' + i + ']');
  }
  crumb.appendChild(el('b', null, trail.join(' › ') || '(根节点)'));
  p.appendChild(crumb);

  if (!S.sel.path.length) p.appendChild(assetSection(asset));
  if (node) p.appendChild(nodeSection(node));
}

function assetSection(asset) {
  const sec = section('资产');
  const row = el('div', 'field');
  row.appendChild(el('label', null, '类型'));
  const type = document.createElement('input');
  type.value = asset.type || '';
  type.oninput = () => { asset.type = type.value; markDirty(); renderTree(); };
  row.appendChild(type);
  sec.appendChild(row);

  const idRow = el('div', 'field');
  idRow.appendChild(el('label', null, '资产 id'));
  const id = document.createElement('input');
  id.value = asset.id || '';
  id.oninput = () => { asset.id = id.value; markDirty(); renderTree(); };
  idRow.appendChild(id);
  sec.appendChild(idRow);

  if (asset.type === 'viewScreen') {
    const r = el('div', 'field');
    r.appendChild(el('label', null, '挂载模式'));
    const sel = document.createElement('select');
    for (const m of ['dynamic', 'static', 'auto']) {
      const o = document.createElement('option');
      o.value = m; o.textContent = m;
      if (asset.mount_mode === m) o.selected = true;
      sel.appendChild(o);
    }
    sel.onchange = () => { asset.mount_mode = sel.value; markDirty(); renderTree(); };
    r.appendChild(sel);
    sec.appendChild(r);
  }

  if (asset.type === 'screenFlow') {
    sec.appendChild(flowLists(asset));
  }

  const acts = el('div', 'row');
  acts.appendChild(newScreenButton());
  acts.appendChild(deleteAssetButton(asset));
  sec.appendChild(acts);
  return sec;
}

function flowLists(asset) {
  const wrap = el('div');
  const sc = section('屏列表 screens');
  const arr = asset.screens || [];   // read-only until the user adds one
  arr.forEach((name, i) => {
    const row = el('div', 'kv-row');
    const inp = document.createElement('input');
    inp.className = 'kv-val';
    inp.value = name;
    inp.oninput = () => { arr[i] = inp.value; markDirty(); };
    const del = el('button', 'kv-del', '×');
    del.onclick = () => {
      attach(asset, 'screens', []).splice(i, 1);
      markDirty();
      renderPanel();
    };
    row.appendChild(inp); row.appendChild(del);
    sc.appendChild(row);
  });
  const add = el('button', 'btn-small', '+ 添加屏');
  add.onclick = () => {
    attach(asset, 'screens', []).push('new_screen');
    markDirty();
    renderPanel();
  };
  sc.appendChild(add);
  wrap.appendChild(sc);

  const tr = section('跳转 transitions');
  const tarr = asset.transitions || [];   // read-only until the user adds one
  tarr.forEach((t, i) => {
    const row = el('div', 'kv-row');
    const act = document.createElement('input');
    act.className = 'kv-val';
    act.value = t.action || '';
    act.setAttribute('list', 'action-list');
    act.oninput = () => { t.action = act.value; markDirty(); };
    const to = document.createElement('input');
    to.className = 'kv-val';
    to.value = t.to || '';
    to.oninput = () => { t.to = to.value; markDirty(); };
    const del = el('button', 'kv-del', '×');
    del.onclick = () => {
      attach(asset, 'transitions', []).splice(i, 1);
      markDirty();
      renderPanel();
    };
    row.appendChild(act); row.appendChild(to); row.appendChild(del);
    tr.appendChild(row);
  });
  const tadd = el('button', 'btn-small', '+ 添加跳转');
  tadd.onclick = () => {
    attach(asset, 'transitions', []).push({ from: [], action: '', to: '' });
    markDirty();
    renderPanel();
  };
  tr.appendChild(tadd);
  wrap.appendChild(tr);
  return wrap;
}

function nodeSection(node) {
  const sec = section('节点');
  const isRoot = !S.sel.path.length;

  const typeRow = el('div', 'field');
  typeRow.appendChild(el('label', null, 'type'));
  const type = document.createElement('input');
  type.value = node.type || '';
  type.oninput = () => { node.type = type.value; markDirty(); renderTree(); };
  typeRow.appendChild(type);
  sec.appendChild(typeRow);

  const idRow = el('div', 'field');
  idRow.appendChild(el('label', null, 'id'));
  const id = document.createElement('input');
  id.value = node.id || '';
  id.oninput = () => { node.id = id.value; markDirty(); renderTree(); };
  idRow.appendChild(id);
  sec.appendChild(idRow);

  if (node.labelProps || node.type === 'label') {
    const lp = node.labelProps || {};   // read-only until the user types
    const r = el('div', 'field');
    r.appendChild(el('label', null, '文本'));
    const t = document.createElement('input');
    t.value = lp.text === undefined ? '' : lp.text;
    t.oninput = () => {
      attach(node, 'labelProps', {}).text = t.value;
      markDirty();
    };
    r.appendChild(t);
    sec.appendChild(r);
  }

  if (node.imageProps) {
    const ip = node.imageProps;
    const r = el('div', 'field');
    r.appendChild(el('label', null, '图片 src'));
    const s = document.createElement('input');
    s.value = ip.src || '';
    s.oninput = () => { ip.src = s.value; markDirty(); };
    r.appendChild(s);
    sec.appendChild(r);
  }

  const acts = el('div', 'row');
  if (!isRoot) {
    acts.appendChild(moveButton(node, -1, '↑ 上移'));
    acts.appendChild(moveButton(node, 1, '↓ 下移'));
    acts.appendChild(duplicateButton(node));
    acts.appendChild(deleteNodeButton(node));
  }
  sec.appendChild(acts);

  const addRow = el('div', 'row');
  const sel = document.createElement('select');
  for (const t of ['container', 'label', 'button', 'image']) {
    const o = document.createElement('option');
    o.value = t; o.textContent = t;
    sel.appendChild(o);
  }
  const add = el('button', null, '+ 添加子节点');
  add.onclick = () => {
    attach(node, 'children', []).push(newNode(sel.value));
    markDirty();
    renderTree();
    renderPanel();
  };
  addRow.appendChild(sel);
  addRow.appendChild(add);
  sec.appendChild(addRow);

  if (node.style) sec.appendChild(kvSection('样式 style', node.style));
  if (node.layout) sec.appendChild(kvSection('布局 layout', node.layout));
  if (node.placement) sec.appendChild(kvSection('位置 placement', node.placement));
  if (node.commonProps) sec.appendChild(kvSection('通用 commonProps', node.commonProps));
  if (node.bindings) sec.appendChild(kvSection('绑定 bindings', node.bindings));
  sec.appendChild(eventsSection(node));

  const raw = el('div');
  raw.appendChild(el('h2', null, '原始 JSON'));
  const ta = document.createElement('textarea');
  ta.className = 'raw';
  ta.value = JSON.stringify(node, null, 2);
  raw.appendChild(ta);
  const apply = el('button', null, '应用到该节点');
  apply.onclick = () => {
    let parsed;
    try {
      parsed = JSON.parse(ta.value);
    } catch (err) {
      setStatus('JSON 解析失败：' + err.message, 'err');
      return;
    }
    const target = nodeAt(S.sel.asset, S.sel.path);
    for (const k of Object.keys(target)) delete target[k];
    Object.assign(target, parsed);
    markDirty();
    renderTree();
    renderPanel();
    setStatus('已应用原始 JSON（记得保存）', 'warn');
  };
  raw.appendChild(apply);
  sec.appendChild(raw);
  return sec;
}

function kvSection(title, obj) {
  const sec = section(title);
  const keys = Object.keys(obj);
  keys.forEach((k) => {
    const row = el('div', 'kv-row');
    const ki = document.createElement('input');
    ki.className = 'kv-key';
    ki.value = k;
    const vi = document.createElement('input');
    vi.className = 'kv-val';
    vi.value = obj[k] === undefined ? '' : String(obj[k]);
    row.appendChild(ki);
    row.appendChild(vi);
    if (COLOR_KEYS.includes(k)) {
      const sw = document.createElement('input');
      sw.type = 'color';
      sw.className = 'kv-color';
      sw.value = toHex(obj[k]);
      vi.oninput = () => { obj[k] = vi.value; sw.value = toHex(vi.value); markDirty(); };
      sw.oninput = () => { obj[k] = sw.value; vi.value = sw.value; markDirty(); };
      row.appendChild(sw);
    } else {
      vi.oninput = () => { obj[k] = vi.value; markDirty(); };
    }
    const del = el('button', 'kv-del', '×');
    del.onclick = () => { delete obj[k]; markDirty(); renderPanel(); };
    ki.onchange = () => {
      const nk = ki.value.trim();
      if (!nk || nk === k) return;
      const v = obj[k];
      delete obj[k];
      obj[nk] = v;
      markDirty();
      renderPanel();
    };
    row.appendChild(del);
    sec.appendChild(row);
  });
  const add = el('button', null, '+ 添加属性');
  add.onclick = () => {
    const k = prompt('属性名');
    if (!k) return;
    obj[k] = '';
    markDirty();
    renderPanel();
  };
  sec.appendChild(add);
  return sec;
}

function eventsSection(node) {
  const sec = section('事件 events');
  const arr = node.events || [];   // read-only until the user adds one
  arr.forEach((ev, i) => {
    const row = el('div', 'kv-row');
    const ti = document.createElement('input');
    ti.className = 'kv-key';
    ti.value = ev.type || 'clicked';
    ti.oninput = () => { ev.type = ti.value; markDirty(); };
    const ai = document.createElement('input');
    ai.className = 'kv-val';
    ai.value = ev.action || '';
    ai.setAttribute('list', 'action-list');
    ai.oninput = () => { ev.action = ai.value; markDirty(); };
    const del = el('button', 'kv-del', '×');
    del.onclick = () => {
      attach(node, 'events', []).splice(i, 1);
      markDirty();
      renderPanel();
    };
    row.appendChild(ti);
    row.appendChild(ai);
    row.appendChild(del);
    sec.appendChild(row);
  });
  const add = el('button', null, '+ 添加事件');
  add.onclick = () => {
    attach(node, 'events', []).push({ type: 'clicked', action: '' });
    markDirty();
    renderPanel();
  };
  sec.appendChild(add);
  return sec;
}

function toHex(v) {
  if (typeof v !== 'string') return '#000000';
  if (/^#[0-9a-fA-F]{6}$/.test(v)) return v;
  if (/^#[0-9a-fA-F]{3}$/.test(v)) {
    return '#' + v[1] + v[1] + v[2] + v[2] + v[3] + v[3];
  }
  return '#000000';
}

/* ------------------------------------------------------------- node ops */

function newNode(type) {
  switch (type) {
    case 'label':
      return { type: 'label', id: 'new_label', labelProps: { text: '新标签' },
               style: { textColor: '#eef2ff' } };
    case 'button':
      return { type: 'button', id: 'new_button',
               style: { bgColor: '#6366f1' },
               placement: { width: '100dp', height: '40dp' },
               children: [{ type: 'label', id: 'new_button_lbl',
                            labelProps: { text: '按钮' },
                            placement: { mode: 'relative', align: 'center' },
                            style: { textColor: '#eef2ff' } }] };
    case 'image':
      return { type: 'image', id: 'new_image', imageProps: { src: '' },
               placement: { width: '32dp', height: '32dp' } };
    default:
      return { type: 'container', id: 'new_container',
               layout: { type: 'flex', flexFlow: 'column', gap: '4dp' },
               placement: { width: 'wrap', height: 'wrap' },
               style: {}, children: [] };
  }
}

function moveButton(node, delta, label) {
  const b = el('button', null, label);
  b.onclick = () => {
    const parent = parentOf(S.sel.asset, S.sel.path);
    if (!parent) return;
    const kids = childrenOf(parent);
    if (!kids) return;
    const i = S.sel.path[S.sel.path.length - 1];
    const j = i + delta;
    if (j < 0 || j >= kids.length) return;
    const tmp = kids[i];
    kids[i] = kids[j];
    kids[j] = tmp;
    S.sel.path[S.sel.path.length - 1] = j;
    markDirty();
    renderTree();
    renderPanel();
  };
  return b;
}

function duplicateButton(node) {
  const b = el('button', null, '⧉ 复制');
  b.onclick = () => {
    const parent = parentOf(S.sel.asset, S.sel.path);
    if (!parent) return;
    const kids = childrenOf(parent);
    if (!kids) return;
    const i = S.sel.path[S.sel.path.length - 1];
    const copy = JSON.parse(JSON.stringify(kids[i]));
    copy.id = (copy.id || 'node') + '_copy';
    kids.splice(i + 1, 0, copy);
    markDirty();
    renderTree();
    renderPanel();
  };
  return b;
}

function deleteNodeButton(node) {
  const b = el('button', 'danger', '🗑 删除');
  b.onclick = () => {
    const parent = parentOf(S.sel.asset, S.sel.path);
    if (!parent) return;
    const kids = childrenOf(parent);
    if (!kids) return;
    if (!confirm('删除节点 #' + (node.id || '') + ' ?')) return;
    kids.splice(S.sel.path[S.sel.path.length - 1], 1);
    S.sel.path = S.sel.path.slice(0, -1);
    markDirty();
    renderTree();
    renderPanel();
  };
  return b;
}

function newScreenButton() {
  const b = el('button', null, '+ 新建屏');
  b.onclick = () => {
    const id = prompt('新屏 id (小写字母/下划线)', 'new_screen');
    if (!id) return;
    const rel = 'screens/' + id + '.json';
    if (S.assets[rel]) { setStatus('已存在: ' + rel, 'err'); return; }
    S.assets[rel] = {
      type: 'viewScreen', id: id, mount_mode: 'dynamic',
      style: { bgColor: '#0b0f1a' },
      children: [{
        type: 'container', id: id + '_main',
        layout: { type: 'flex', flexFlow: 'column', gap: '8dp' },
        placement: { x: '112dp', y: '0dp', width: '688dp', height: '480dp' },
        style: { bgColor: '#0b0f1a', padding: '10dp' },
        children: [],
      }],
    };
    S.root.assets = S.root.assets || [];
    S.root.assets.push(rel);
    markDirty();
    renderTree();
    select(rel, []);
  };
  return b;
}

function deleteAssetButton(asset) {
  const b = el('button', 'danger', '🗑 删除该资产');
  b.onclick = () => {
    const rel = S.sel.asset;
    if (!confirm('删除资产 ' + rel + ' ?\n(若 ui_contract.json 仍引用它，保存会被 contract 测试拒绝)')) return;
    delete S.assets[rel];
    S.root.assets = (S.root.assets || []).filter((r) => r !== rel);
    S.sel = { asset: null, path: [] };
    markDirty();
    renderTree();
    renderPanel();
  };
  return b;
}

/* ------------------------------------------------------------------ api */

async function loadAll() {
  const res = await fetch('/api/ui');
  const data = await res.json();
  if (data.error) { setStatus(data.error, 'err'); return; }
  S.root = data.root;
  S.assets = data.assets;
  S.contract = data.contract;
  S.screenProfile = data.screen_profile || null;
  S.dirty = false;
  setDirtyMark('');
  if (!S.sel.asset || !S.assets[S.sel.asset]) S.sel = { asset: null, path: [] };
  renderTree();
  renderMode();
  setStatus('已加载 ' + Object.keys(S.assets).length + ' 个资产', 'ok');
}

async function validate() {
  setStatus('校验中…');
  const res = await fetch('/api/validate', { method: 'POST' });
  const data = await res.json();
  showOutput(data.output);
  setStatus(data.ok ? 'contract 测试通过' : 'contract 测试失败', data.ok ? 'ok' : 'err');
}

async function save() {
  if (!confirm('保存到 components/minihub_lcd/ui/ ?')) return;
  setStatus('保存中…');
  const res = await fetch('/api/save', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ root: S.root, assets: S.assets }),
  });
  const data = await res.json();
  showOutput(data.output);
  if (data.ok) {
    S.dirty = false;
    setDirtyMark('');
    setStatus(data.message, 'ok');
  } else {
    setStatus(data.message, 'err');
  }
}

function showOutput(text) {
  // The <details> lives in index.html; JS only fills it. Keeping the DOM
  // structure in the HTML is what lets the id cross-check test stay strict.
  const box = document.getElementById('out');
  if (!box) return;
  box.querySelector('pre').textContent = text || '(无输出)';
  box.style.display = 'block';
  box.open = true;
}

/* ------------------------------------------------------------------ boot */

document.getElementById('btn-reload').onclick = async () => {
  if (S.dirty && !confirm('有未保存的修改，重新加载会丢弃。继续 ?')) return;
  await loadAll();
};
document.getElementById('btn-validate').onclick = validate;
document.getElementById('btn-save').onclick = save;
document.getElementById('mode-edit').onclick = () => setMode('edit');
document.getElementById('mode-preview').onclick = () => setMode('preview');
window.addEventListener('beforeunload', (e) => {
  if (S.dirty) { e.preventDefault(); e.returnValue = ''; }
});
// A frontend bug must never look like "the tree is empty": surface it in the
// status bar instead of dying silently between two render calls.
window.addEventListener('error', (e) => {
  setStatus('JS 错误: ' + (e.message || e.error), 'err');
});
window.addEventListener('unhandledrejection', (e) => {
  const why = e.reason && e.reason.message ? e.reason.message : e.reason;
  setStatus('异步错误: ' + why, 'err');
});
loadAll().catch((err) => {
  setStatus('加载失败: ' + err.message, 'err');
  console.error(err);
});
