/* MiniHub LCD UI editor - screen preview.
 *
 * Renders a Brookesia screen asset (components/minihub_lcd/ui/screens/*.json)
 * in the browser so the layout can be judged without flashing the board.
 *
 * WHAT THIS IS
 *   An APPROXIMATION of the Brookesia layout rules, not the real engine.
 *   It covers the subset the LCD assets actually use:
 *
 *     placement  mode absolute | relative | flow, x, y, width, height, flexGrow
 *     layout     flex, flexFlow column | row | rowWrap,
 *                mainAlign start | center | end | spaceBetween,
 *                crossAlign start | center | end | stretch, gap
 *     style      bgColor, borderWidth, borderColor, padding, radius,
 *                textColor, fontSize
 *     commonProps hidden, scrollable
 *     node types container, label, button, image
 *
 *   Text metrics use the browser font, so glyph widths differ from the device
 *   (Montserrat + bundled CJK). Use the preview to judge LAYOUT, not to judge
 *   pixel-perfect typography.
 *
 * SCREEN SIZE
 *   The base geometry, density and font scale come from the firmware through
 *   /api/ui -> screen_profile (parsed out of minihub_lcd_app.cpp), so the
 *   preview never owns a second copy of 800x480. Switching the preview to a
 *   different (same-aspect) resolution changes density = width / baseWidth,
 *   and dp / sp are scaled exactly as Brookesia's parser does:
 *
 *       dp -> dp * density
 *       sp -> sp * density * font_scale
 *       %  -> resolved against the parent box (never scaled)
 *       px -> raw pixels (never scaled)
 *
 *   Fixed-pixel things on the device (the imageSet bitmaps, the CJK font and
 *   pin_shell_geometry()'s kShell* constants) do NOT scale. The last one is
 *   drawn as a dashed overlay so a density change exposes the mismatch.
 */
'use strict';

const PV = {
  // Filled by pvSetProfile() from /api/ui. These defaults are only a fallback
  // for when the profile endpoint is unavailable.
  base: {
    width: 800, height: 480, sidebar: 112, main: 688,
    fontScale: 1, firmwareDensity: 1, source: null,
  },
  density: 1,
  W: 800,
  H: 480,
  zoom: 0.62,      // stage width at the BASE resolution, in CSS px / device px
  FONT: 'Montserrat, "PingFang SC", "Microsoft YaHei", sans-serif',
};

/** Adopt the firmware's screen profile and pick a target (device) width. */
function pvSetProfile(profile, targetWidth) {
  const p = profile || {};
  const num = (v, d) => (Number(v) > 0 ? Number(v) : d);
  PV.base = {
    width: num(p.base_width, 800),
    height: num(p.base_height, 480),
    sidebar: num(p.sidebar_width, 112),
    main: num(p.main_width, 688),
    fontScale: num(p.font_scale, 1),
    firmwareDensity: num(p.density, 1),
    source: p.source || null,
  };
  const w = num(targetWidth, Math.round(PV.base.width * PV.base.firmwareDensity));
  PV.density = w / PV.base.width;
  PV.W = Math.round(w);
  PV.H = Math.round(PV.base.height * PV.density);
}

/** Keep the preview the same visual size when the device resolution changes. */
function pvFitZoom() {
  return PV.zoom * (PV.base.width / PV.W);
}

function pvFontFactor() {
  return PV.density * PV.base.fontScale;
}

let pvMetrics = null;

function pvCtx() {
  if (!pvMetrics) {
    const c = document.createElement('canvas');
    pvMetrics = c.getContext('2d');
  }
  return pvMetrics;
}

/** "12dp" -> 12*density ; "16sp" -> 16*density*fontScale ; "80%" -> 80. */
function pvNum(v, fallback) {
  if (v === undefined || v === null || v === '') return fallback;
  if (typeof v === 'number') return v * PV.density;
  const m = String(v).trim().match(/^(-?\d+(?:\.\d+)?)(dp|sp|px|%)?$/);
  if (!m) return fallback;
  const n = parseFloat(m[1]);
  const unit = m[2];
  if (unit === '%' || unit === 'px') return n;
  return n * (unit === 'sp' ? pvFontFactor() : PV.density);
}

/** flexGrow is a unitless ratio - multiplying it by density would be wrong. */
function pvGrow(v) {
  const n = Number(v);
  return Number.isFinite(n) ? n : 0;
}

function pvPercent(v) {
  return typeof v === 'string' && v.trim().endsWith('%');
}

function pvHidden(node) {
  return !!(node && node.commonProps && node.commonProps.hidden);
}

function pvAbsolute(node) {
  const pl = (node && node.placement) || {};
  return pl.mode === 'absolute' || pl.x !== undefined || pl.y !== undefined;
}

function pvRelativeCentered(node) {
  const pl = (node && node.placement) || {};
  return pl.mode === 'relative' && pl.align === 'center';
}

function pvText(node) {
  const lp = node.labelProps || {};
  if (lp.text) return lp.text;
  const bind = (node.bindings || {})['labelProps.text'];
  return bind || '';
}

function pvFontSize(node) {
  return pvNum((node.style || {}).fontSize, 14 * pvFontFactor());
}

function pvMeasureText(text, fontSize) {
  const ctx = pvCtx();
  ctx.font = `${fontSize}px ${PV.FONT}`;
  const m = ctx.measureText(text || '');
  return { w: Math.ceil(m.width), h: Math.ceil(fontSize * 1.32) };
}

/* ------------------------------------------------------------- measuring */

/** Intrinsic size of a node ("wrap" size). */
function pvContentSize(node) {
  const style = node.style || {};
  const pad = pvNum(style.padding, 0);
  const type = node.type || 'container';

  if (type === 'label') {
    const t = pvMeasureText(pvText(node), pvFontSize(node));
    return { w: t.w + 2 * pad, h: t.h + 2 * pad };
  }
  if (type === 'image') {
    const pl = node.placement || {};
    return {
      w: pvNum(pl.width, 32) + 2 * pad,
      h: pvNum(pl.height, 32) + 2 * pad,
    };
  }

  const lay = node.layout || {};
  const row = (lay.flexFlow || 'column').indexOf('row') === 0;
  const gap = pvNum(lay.gap, 0);
  const kids = (node.children || []).filter((k) => !pvHidden(k) && !pvAbsolute(k)
                                           && !pvRelativeCentered(k));
  let main = 0;
  let cross = 0;
  for (const k of kids) {
    const sz = pvContentSize(k);
    if (row) {
      main += sz.w;
      cross = Math.max(cross, sz.h);
    } else {
      main += sz.h;
      cross = Math.max(cross, sz.w);
    }
  }
  main += gap * Math.max(0, kids.length - 1);
  return row
    ? { w: main + 2 * pad, h: cross + 2 * pad }
    : { w: cross + 2 * pad, h: main + 2 * pad };
}

/** Resolve one child's size inside a parent box of (boxW x boxH). */
function pvChildSize(node, boxW, boxH) {
  const pl = node.placement || {};
  const content = pvContentSize(node);
  let w;
  let h;
  if (pvPercent(pl.width)) w = (pvNum(pl.width, 0) / 100) * boxW;
  else if (pl.width === undefined || pl.width === 'wrap') w = content.w;
  else w = pvNum(pl.width, content.w);
  if (pvPercent(pl.height)) h = (pvNum(pl.height, 0) / 100) * boxH;
  else if (pl.height === undefined || pl.height === 'wrap') h = content.h;
  else h = pvNum(pl.height, content.h);
  return { w, h, content };
}

/* --------------------------------------------------------------- painting */

/** Index of `child` inside its parent's children - the tree path segment. */
function pvChildIndex(child, parent) {
  const kids = (parent && parent.children) || [];
  const i = kids.indexOf(child);
  return i < 0 ? 0 : i;
}

/** Paint `node` as an absolutely positioned box at (x, y, w, h). */
function pvPaint(parentEl, node, x, y, w, h, ctx) {
  if (pvHidden(node)) return null;
  const style = node.style || {};
  const type = node.type || 'container';

  const el = document.createElement('div');
  el.className = 'pv-node pv-' + type;
  el.style.left = x + 'px';
  el.style.top = y + 'px';
  el.style.width = w + 'px';
  el.style.height = h + 'px';

  if (style.bgColor) el.style.background = style.bgColor;
  if (style.borderWidth) {
    el.style.borderWidth = pvNum(style.borderWidth, 0) + 'px';
    el.style.borderStyle = 'solid';
    el.style.borderColor = style.borderColor || 'transparent';
  }
  if (style.radius) el.style.borderRadius = pvNum(style.radius, 0) + 'px';
  if (style.padding) el.style.padding = pvNum(style.padding, 0) + 'px';

  el.dataset.pvPath = ctx.path.join('.');
  el.dataset.pvType = type;
  const nodeId = node.id || '';
  el.dataset.pvId = nodeId;
  el.title = nodeId ? nodeId : type;

  const pad = pvNum(style.padding, 0);
  const boxW = Math.max(0, w - 2 * pad);
  const boxH = Math.max(0, h - 2 * pad);

  if (type === 'label') {
    const span = document.createElement('span');
    span.className = 'pv-text';
    span.textContent = pvText(node);
    span.style.color = style.textColor || '#eef2ff';
    span.style.fontSize = pvFontSize(node) + 'px';
    el.appendChild(span);
    if (!pvText(node)) el.classList.add('pv-empty');
  } else if (type === 'image') {
    const src = (node.imageProps || {}).src || '';
    const box = document.createElement('div');
    box.className = 'pv-image';
    box.textContent = src.startsWith('${') ? src.replace(/[${}]/g, '') : (src || 'img');
    el.appendChild(box);
  } else if (type === 'button') {
    el.classList.add('pv-button');
  }

  // viewScreen is the screen root - it lays its children out like a container.
  if (type === 'container' || type === 'button' || type === 'viewScreen') {
    pvPaintChildren(el, node, boxW, boxH, ctx);
  }

  if (node.commonProps && node.commonProps.scrollable) {
    el.classList.add('pv-scrollable');
  }

  parentEl.appendChild(el);
  return el;
}

/** Lay the children of a container out inside its content box. */
function pvPaintChildren(parentEl, node, boxW, boxH, ctx) {
  const kids = (node.children || []).filter((k) => !pvHidden(k));
  const lay = node.layout || {};
  const flex = lay.type === 'flex';
  const row = flex && (lay.flexFlow || 'column').indexOf('row') === 0;

  // absolute children sit on their own coordinates
  for (const k of kids.filter(pvAbsolute)) {
    const pl = k.placement || {};
    const size = pvChildSize(k, boxW, boxH);
    const cw = pl.width === undefined || pl.width === 'wrap' ? size.content.w : size.w;
    const ch = pl.height === undefined || pl.height === 'wrap' ? size.content.h : size.h;
    pvPaint(parentEl, k, pvNum(pl.x, 0), pvNum(pl.y, 0), cw, ch,
            { path: ctx.path.concat([pvChildIndex(k, node)]), asset: ctx.asset });
  }

  // centred children (button labels)
  for (const k of kids.filter(pvRelativeCentered)) {
    const size = pvChildSize(k, boxW, boxH);
    pvPaint(parentEl, k, (boxW - size.w) / 2, (boxH - size.h) / 2, size.w, size.h,
            { path: ctx.path.concat([pvChildIndex(k, node)]), asset: ctx.asset });
  }

  const flow = kids.filter((k) => !pvAbsolute(k) && !pvRelativeCentered(k));
  if (!flow.length) return;

  const items = flow.map((k) => {
    const pl = k.placement || {};
    const size = pvChildSize(k, boxW, boxH);
    // An explicit cross size wins over "stretch" (same as LVGL): a child that
    // says width:100dp keeps 100dp, it is not widened to the container.
    const fixedCross = row
      ? (pl.height !== undefined && pl.height !== 'wrap' && !pvPercent(pl.height))
      : (pl.width !== undefined && pl.width !== 'wrap' && !pvPercent(pl.width));
    return {
      node: k,
      w: size.w,
      h: size.h,
      grow: pvGrow(pl.flexGrow),
      fixedCross,
    };
  });

  const gap = pvNum(lay.gap, 0);
  const availMain = row ? boxW : boxH;
  const availCross = row ? boxH : boxW;

  let usedMain = items.reduce((s, it) => s + (row ? it.w : it.h), 0)
    + gap * Math.max(0, items.length - 1);
  const growTotal = items.reduce((s, it) => s + it.grow, 0);
  if (growTotal > 0 && availMain - usedMain > 0) {
    const free = availMain - usedMain;
    for (const it of items) {
      const add = free * (it.grow / growTotal);
      if (row) it.w += add;
      else it.h += add;
    }
    usedMain = availMain;
  }

  const mainAlign = lay.mainAlign || 'start';
  const crossAlign = lay.crossAlign || 'start';
  let spacing = gap;
  let cursor = 0;
  const free = availMain - usedMain;
  if (mainAlign === 'center') cursor = free / 2;
  else if (mainAlign === 'end') cursor = free;
  else if (mainAlign === 'spaceBetween' && items.length > 1) {
    spacing = gap + free / (items.length - 1);
  }

  for (const it of items) {
    const cross = row ? it.h : it.w;
    let crossPos = 0;
    if (crossAlign === 'stretch') crossPos = 0;
    else if (crossAlign === 'center') crossPos = (availCross - cross) / 2;
    else if (crossAlign === 'end') crossPos = availCross - cross;

    const useCross = (crossAlign === 'stretch' && !it.fixedCross)
      ? Math.max(cross, availCross) : cross;

    const x = row ? cursor : crossPos;
    const y = row ? crossPos : cursor;
    const cw = row ? it.w : useCross;
    const ch = row ? useCross : it.h;

    pvPaint(parentEl, it.node, x, y, cw, ch,
            { path: ctx.path.concat([pvChildIndex(it.node, node)]), asset: ctx.asset });
    cursor += (row ? cw : ch) + spacing;
  }
}

/* ------------------------------------------------------------------ entry */

/**
 * Where pin_shell_geometry() would put sidebar + main in RAW pixels.
 *
 * Those kShell* constants are lv_coord_t and are never multiplied by density,
 * so at density != 1 the dashed boxes drift away from the JSON layout. That is
 * the first thing to fix when adapting to another screen - show it instead of
 * letting someone discover it on hardware.
 */
function pvFixedGeometryOverlay() {
  const layer = document.createElement('div');
  layer.className = 'pv-fixed-layer';
  const boxes = [
    { title: 'sidebar (C++ 固定 px)', x: 0, y: 0,
      w: PV.base.sidebar, h: PV.base.height },
    { title: 'main (C++ 固定 px)', x: PV.base.sidebar, y: 0,
      w: PV.base.main, h: PV.base.height },
  ];
  for (const b of boxes) {
    const box = document.createElement('div');
    box.className = 'pv-fixed-box';
    box.style.left = b.x + 'px';
    box.style.top = b.y + 'px';
    box.style.width = b.w + 'px';
    box.style.height = b.h + 'px';
    box.title = `${b.title}: ${b.w}×${b.h} px（不随 density 缩放）`;
    layer.appendChild(box);
  }
  return layer;
}

/** Build the preview DOM for one screen asset. `opts`: { zoom, showFixed }. */
function pvBuildScreen(assetKey, assets, opts) {
  const o = opts || {};
  const asset = assets[assetKey];
  const stage = document.createElement('div');
  stage.className = 'pv-stage';
  const z = o.zoom || pvFitZoom();
  stage.style.width = Math.round(PV.W * z) + 'px';
  stage.style.height = Math.round(PV.H * z) + 'px';

  const screen = document.createElement('div');
  screen.className = 'pv-screen';
  screen.style.width = PV.W + 'px';
  screen.style.height = PV.H + 'px';
  screen.style.transform = 'scale(' + z + ')';

  const root = asset.type === 'viewTemplate' ? asset.node : asset;
  const style = (root && root.style) || {};
  if (style.bgColor) screen.style.background = style.bgColor;

  pvPaint(screen, root, 0, 0, PV.W, PV.H, { path: [], asset: assetKey });
  if (o.showFixed) screen.appendChild(pvFixedGeometryOverlay());
  stage.appendChild(screen);
  return stage;
}
