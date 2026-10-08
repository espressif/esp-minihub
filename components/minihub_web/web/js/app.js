let pts = [],
    packs = [],
    modules = [],
    storeCatalog = [],
    storeCatalogOk = true,
    storeTab = 'installed',
    filter = 'all',
    query = '',
    storeQ = '',
    roomFilter = '',
    typeFilter = '',
    homePickerModule = '',
    homePickerSelected = '';
let rtspCamStatus = null;
let authStatusByAdapter = {};
let matterStatus = {
    ready: false,
    commissioner: false
};
let radioProv = {
    zigbee_permit_join: false,
    ble_mesh_permit_provision: false,
    ble_mesh_provisioning: false,
    busy: false
};
let view = 'overview',
    geoLoc = null,
    leafletMap = null,
    deviceLayer = null;
let enabledAdapters = {};
const ADAPTER_HIDDEN = {
    thread: true
};
/* Catalog / UI names live in /web/i18n/*.json, not in a JS table: a new device
 * type then needs a string entry, and nothing here, to carry an English name. */
function mhName(kind, key, fallback) {
    const k = kind + '.' + key;
    const s = t(k);
    return s === k ? (fallback || key) : s;
}

function mhTypeLabel(type) {
    return mhName('type', type, type || '');
}

function mhAdapterLabel(ad) {
    return mhName('adapter', ad, ad);
}

function mhTitle(view) {
    return mhName('title', view, view);
}

const TYPE_ICON = {
    switch: '\u26A1',
    light: '\u2600',
    sensor: '\uD83C\uDF21',
    button: '\u25CE',
    binary_sensor: '\uD83D\uDEAA',
    client: '\uD83D\uDCF1',
    smoke: '\uD83D\uDEA8',
    router: '\uD83D\uDCE1',
    car: '\uD83D\uDE97',
    cover: '\u239A',
    camera: '\uD83D\uDCF7',
    television: '\uD83D\uDCFA',
    lock: '\uD83D\uDD12',
    speaker: '\uD83D\uDD0A',
    'air-conditioner': '\u2744',
    fridge: '\uD83E\uDDCA',
    water_purifier: '\uD83D\uDCA7',
    water_heater: '\u2615',
    washer: '\uD83E\uDDFA',
    dishwasher: '\uD83E\uDDFA',
    oven: '\uD83C\uDF73',
    appliance: '\uD83D\uDCE6'
};

function dwTypeDisplay(p) {
    const pr = (p && p.props && typeof p.props === 'object') ? p.props : {};
    const cat = pr.category || pr.product || pr.product_name;
    if (cat != null && String(cat) !== '') {
        return String(cat);
    }
    return mhTypeLabel(p && p.type);
}

function adapterEnabled(ad) {
    if (ADAPTER_HIDDEN[ad]) return false;
    const v = enabledAdapters[ad];
    if (typeof v === 'boolean') return v;
    if (v && typeof v.enabled === 'boolean') return v.enabled;
    return false;
}

function adapterVersion(ad) {
    const v = enabledAdapters[ad];
    if (v && v.module_version) return v.module_version;
    if (v && v.adapter_version) return v.adapter_version;
    const m = modules.find(x => x.adapter === ad);
    if (m && m.adapter_version) return m.adapter_version;
    return m && m.version ? m.version : '';
}

function modUi(m) {
    return (m && m.ui) || {};
}

function modLabels(m) {
    return modUi(m).labels || {};
}

function modActions(m) {
    return modUi(m).actions || [];
}

function hasModAction(m, a) {
    return modActions(m).indexOf(a) >= 0;
}

function hasCredLogin(m) {
    const f = modUi(m).login_form;
    return f === 'credentials_2fa' || f === 'credentials_api_key' || f === 'credentials_host';
}

function isApiKeyLogin(m) {
    return modUi(m).login_form === 'credentials_api_key';
}

function isHostLogin(m) {
    return modUi(m).login_form === 'credentials_host';
}

function lbl(m, key, fb) {
    const L = modLabels(m);
    return (L && L[key]) || fb || key;
}

function adapterLabel(ad) {
    const m = modules.find(x => x.adapter === ad);
    if (m && modUi(m).filter_label) return modUi(m).filter_label;
    if (m && m.name) return m.name;
    return mhAdapterLabel(ad);
}

function authModuleFor(ad) {
    return modules.find(x => x.adapter === ad && x.installed !== false && x.enabled !== false && x.ui && x.ui.auth);
}

function authModules() {
    return modules.filter(x => x.installed !== false && x.enabled !== false && x.ui && x.ui.auth);
}

function authStatus(ad) {
    return authStatusByAdapter[ad] || {
        logged_in: false
    };
}

let _syncPollTimer = null;
function pollAuthSyncWhileBusy() {
    if (_syncPollTimer) return;
    const tick = async function () {
        await loadMeta();
        const busy = authModules().some(function (m) {
            const st = authStatus(m.adapter);
            return !!(st && st.syncing);
        });
        if (view === 'overview') renderOverview();
        if (!busy) {
            _syncPollTimer = null;
            return;
        }
        _syncPollTimer = setTimeout(tick, 1500);
    };
    _syncPollTimer = setTimeout(tick, 200);
}

function fillTpl(s, vars) {
    return String(s || '').replace(/\{(\w+)\}/g, (m, k) => vars[k] != null ? vars[k] : m);
}

function pointGroup(p) {
    const owner = p && p.props && typeof p.props.owner === 'string' ? p.props.owner : '';
    if (owner && adapterEnabled(owner)) return owner;
    return (p && p.adapter) || '';
}

function pointProtocol(p) {
    if (!p) return '';
    if (p.protocol) return String(p.protocol);
    if (p.props && p.props.protocol) return String(p.props.protocol);
    const id = String(p.point_id || '');
    const dot = id.indexOf('.');
    return dot > 0 ? id.slice(0, dot) : '';
}

/* <protocol>.<device> or <protocol>.<device>.<param>. Slash ids (switch/bm_000a) are not. */
function pointIdScoped(pointId) {
    if (typeof pointId !== 'string' || !pointId) return false;
    const seg = pointId.split('.');
    if (seg.length < 2 || !seg[0]) return false;
    for (let i = 1; i < seg.length; i++) {
        if (seg[i]) return true;
    }
    return false;
}

/* Adapter chip: well-formed id whose protocol prefix or adapter/owner matches.
 * Unscoped ids never match a chip — they stay on 全部 only. */
function pointInAdapter(p, ad) {
    if (!ad || !p || !pointIdScoped(p.point_id)) return false;
    if (pointGroup(p) === ad) return true;
    return pointProtocol(p) === ad;
}

function pointDisplayName(p) {
    const n = p && p.props && p.props.name;
    if (n && String(n).trim()) return String(n).trim();
    const id = (p && p.point_id) || '';

    const dot = id.split('.');
    if (dot.length >= 2) return dot[dot.length - (dot.length >= 3 ? 2 : 1)];
    return id.split('/').pop();
}

function pointRoomName(p) {
    const n = p && p.props && p.props.room_name;
    return n ? String(n) : '';
}

function pointMatchesQuery(p, q) {
    if (!q) return true;
    const hay = ((p.point_id || '') + ' ' + pointDisplayName(p) + ' ' + pointRoomName(p)).toLowerCase();
    return hay.includes(q);
}

function onRoomFilterChange(v) {
    roomFilter = v || '';
    renderOverview();
}

function onTypeFilterChange(v) {
    typeFilter = v || '';
    renderOverview();
}

function refreshOverviewFilterOptions(list) {
    const rooms = Array.from(new Set(list.map(pointRoomName).filter(Boolean))).sort();
    const types = Array.from(new Set(list.map(p => p.type).filter(Boolean))).sort();
    const rs = document.getElementById('filter-room');
    const ts = document.getElementById('filter-type');
    if (rs) {
        const cur = roomFilter;
        rs.innerHTML = '<option value="">' + escAttr(t('ov.all_rooms')) + '</option>' + rooms.map(r => '<option value="' + escAttr(r) + '">' + esc(r) + '</option>').join('');
        rs.value = rooms.indexOf(cur) >= 0 ? cur : '';
        roomFilter = rs.value;
    }
    if (ts) {
        const cur = typeFilter;
        ts.innerHTML = '<option value="">' + escAttr(t('ov.all_types')) + '</option>' + types.map(tp => '<option value="' + escAttr(tp) + '">' + esc(mhTypeLabel(tp)) + '</option>').join('');
        ts.value = types.indexOf(cur) >= 0 ? cur : '';
        typeFilter = ts.value;
    }
}

function adapterRoomChipsHtml(adapterPts) {
    const rooms = Array.from(new Set(adapterPts.map(pointRoomName).filter(Boolean))).sort();
    if (!rooms.length) return '';
    let h = '<div class="room-chips"><button type="button" class="chip' + (roomFilter === '' ? ' active' : '') + '" onclick="onRoomFilterChange(\'\')">' + esc(t('ov.all_rooms')) + '</button>';
    rooms.forEach(r => {
        const safe = String(r).replace(/\\/g, '\\\\').replace(/'/g, "\\'");
        h += '<button type="button" class="chip' + (roomFilter === r ? ' active' : '') + '" onclick="onRoomFilterChange(\'' + safe + '\')">' + esc(r) + '</button>';
    });
    return h + '</div>';
}
function toast(m) {
    const t = document.getElementById('toast');
    t.textContent = m;
    t.classList.add('show');
    setTimeout(() => t.classList.remove('show'), 2200)
}

function esc(s) {
    return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;')
}

function switchView(v) {

    if (v === 'devices') v = 'overview';
    view = v;
    document.querySelectorAll('.view').forEach(el => el.hidden = true);
    const viewEl = document.getElementById('view-' + v);
    if (viewEl) viewEl.hidden = false;
    document.querySelectorAll('.nav-item').forEach(n => n.classList.toggle('active', n.dataset.view === v));
    document.getElementById('page-title').textContent = mhTitle(v);
    document.getElementById('sidebar').classList.remove('open');
    if (v === 'map') {
        ensureLeaflet().then(async () => {
            await renderMap();
            setTimeout(() => {
                if (leafletMap) leafletMap.invalidateSize();
            }, 150);
        }).catch(() => {
            const info = document.getElementById('map-info');
            if (info) info.textContent = t('app.map_lib_failed');
        });
    }
    if (v === 'store') {
        loadStoreConfig();
        renderStore();
    }
    if (v === 'rules') {
        rulesAiLoadCfg();
        loadPoints();
        loadRules();
    }
    if (v === 'scenes') {
        loadPoints();
        loadScenes();
    }
    if (v === 'system') {
        loadSystem();
        sysOtaPoll();
    }
    /* The device list paints from what is already cached — a synchronous render —
     * and only then catches up over the network. Awaiting points here would hold
     * the empty grid on screen for a whole round trip. */
    if (v === 'overview' && (!pts.length || Date.now() - lastPtsAt > 2000)) {
        loadPoints();
    }
}
function renderLocalUser() {
    const n = localName();
    document.getElementById('user-name').textContent = n;
    document.getElementById('user-ava').textContent = n.slice(0, 1).toUpperCase();
}
async function loadAdapters() {
    try {
        const r = await fetch('/api/adapters');
        if (!r.ok) return false;
        enabledAdapters = await r.json();
        if (filter !== 'all' && !adapterEnabled(filter)) filter = 'all';
        renderToolbar();
        if (view === 'overview') renderOverview();
        if (view === 'system' && typeof sysFillNet === 'function') sysFillNet();
        return true;
    } catch (e) {
        return false;
    }
}
async function loadMeta() {
    try {
        const next = {};
        for (const mod of authModules()) {
            const ad = mod.adapter;
            if (!adapterEnabled(ad) || mod.has_rpc === false) {
                next[ad] = {
                    logged_in: false
                };
                continue;
            }
            const st = await moduleRpc(mod.id, 'status');
            if (st.json && st.json.logged_in != null) {
                next[ad] = st.json;
            } else {
                next[ad] = {
                    logged_in: false
                };
            }
        }
        authStatusByAdapter = next;
        if (adapterEnabled('matter')) {
            const mt = await fetch('/api/matter/status');
            if (mt.ok) matterStatus = await mt.json();
        } else {
            matterStatus = {
                ready: false,
                commissioner: false
            };
        }
        if (adapterEnabled('zigbee') || adapterEnabled('ble_mesh')) {
            const rp = await fetch('/api/radio/provision').catch(() => null);
            if (rp && rp.ok) radioProv = await rp.json();
        } else {
            radioProv = {
                zigbee_permit_join: false,
                ble_mesh_permit_provision: false,
                ble_mesh_provisioning: false,
                busy: false
            };
        }
        const hub = await (await fetch('/api/')).json();
        const hv = document.getElementById('hub-ver');
        if (hv) hv.textContent = 'v' + (hub && hub.version ? hub.version : '?');
        if (view === 'overview') renderOverview();
    } catch (e) {}
}
let pointsPromise = null;
let lastPtsAt = 0;   /* successful /api/points; tells the view switch whether the cache is fresh */
async function loadPoints(forceRender) {
    /* A caller arriving mid-load must await that run instead of returning early:
     * returning early left `pts` stale (usually empty), which made e.g. the rule
     * editor render with no trigger devices to choose from. */
    if (pointsLoading && pointsPromise) {
        return pointsPromise;
    }
    pointsLoading = true;
    pointsPromise = (async function () {
        try {
            const r = await fetch('/api/points');
            if (!r.ok) throw new Error('points');
            const next = await r.json();
            const sig = pointsUiSig(next);
            const changed = sig !== lastPtsSig;
            lastPtsSig = sig;
            pts = next;
            const foot = document.getElementById('foot');
            lastPtsAt = Date.now();
            if (foot) foot.textContent = t('foot.updated', { time: new Date().toLocaleTimeString(window.mhLocale()) });
            if (changed || forceRender) {
                if (view === 'overview') renderOverview();
                if (view === 'map' && window.L) renderMap();

                if (typeof refreshDeviceDetail === 'function') refreshDeviceDetail();
            }
        } catch (e) {
            const c = document.getElementById('content');
            if (c && !pts.length) c.innerHTML = '<div class="empty">' + t('app.load_failed') + '</div>';
        } finally {
            pointsLoading = false;
        }
    })();
    return pointsPromise;
}

/* A command just changed a device, so the on-screen snapshot must reflect it.
 * A plain loadPoints() can reuse an in-flight poll that STARTED before the
 * command — i.e. a pre-command snapshot — and then the UI would not re-render
 * until the next poll (seconds later). Wait for any in-flight load to settle,
 * then fetch fresh (no overlapping requests, so the small httpd is not hit). */
async function refreshAfterCommand() {
    if (pointsLoading && pointsPromise) {
        try { await pointsPromise; } catch (e) {}
    }
    /* forceRender: always re-draw, so a failed command reverts any optimistic
     * state instead of leaving the card stuck on the flipped value. */
    return loadPoints(true);
}

async function loadRtspCamStatusSafe() {
    if (rtspStatusLoading) return;
    rtspStatusLoading = true;
    try {
        await loadRtspCamStatus();
        if (view === 'overview') renderOverview();
    } finally {
        rtspStatusLoading = false;
    }
}
async function loadModulesForAuth() {
    try {
        const m = await fetch('/api/modules').catch(() => null);
        if (m && m.ok) modules = await m.json();
    } catch (e) {}
}
function renderOverview() {
    const q = query.toLowerCase();

    if (typeof ensureDwCatalog === 'function' && typeof dwCatalog !== 'undefined' && !dwCatalog) {
        ensureDwCatalog().catch(function () {});
    }
    /* Collapse registry rows to devices before filter/stats/cards.
     * Otherwise legacy per-param points (thread.color_bulb.brightness …)
     * inflate 能力点 and render a fake toggle per parameter. */
    /* Keep unscoped ids (switch/bm_000a) even when their adapter is off, so 全部 can list them. */
    let base = overviewCollapsePoints(pts.filter(p => p && (adapterEnabled(p.adapter) || !pointIdScoped(p.point_id))));
    refreshOverviewFilterOptions(base);
    let list = base.filter(p => (filter === 'all' || pointInAdapter(p, filter)) && pointMatchesQuery(p, q) && (!roomFilter || pointRoomName(p) === roomFilter) && (!typeFilter || p.type === typeFilter));
    document.getElementById('s-total').textContent = list.length;
    document.getElementById('s-online').textContent = list.filter(p => p.online).length;
    document.getElementById('s-adapters').textContent = new Set(list.filter(p => pointIdScoped(p.point_id)).map(pointGroup).filter(adapterEnabled)).size;
    const groups = {};
    const loose = [];
    list.forEach(p => {
        /* Slash / other non-<protocol>.<device> ids: show on 全部, not inside an adapter section. */
        if (!pointIdScoped(p.point_id)) {
            if (filter === 'all') loose.push(p);
            return;
        }
        const g = (filter === 'all') ? pointGroup(p) : filter;
        (groups[g] = groups[g] || []).push(p);
    });
    authModules().forEach(m => {
        const ad = m.adapter;
        if (adapterEnabled(ad) && (filter === 'all' || filter === ad) && !groups[ad]) groups[ad] = [];
    });
    if (adapterEnabled('matter') && (filter === 'all' || filter === 'matter') && !groups.matter) {
        groups.matter = [];
    }
    if (adapterEnabled('zigbee') && (filter === 'all' || filter === 'zigbee') && !groups.zigbee) {
        groups.zigbee = [];
    }
    if (adapterEnabled('ble_mesh') && (filter === 'all' || filter === 'ble_mesh') && !groups.ble_mesh) {
        groups.ble_mesh = [];
    }
    const el = document.getElementById('content');
    let html = (filter === 'all' || filter === 'rtsp_cam') ? renderRtspCamCard() : '';
    authModules().forEach(mod => {
        const ad = mod.adapter;
        if (!adapterEnabled(ad) || !(filter === 'all' || filter === ad)) return;
        if (!(hasModAction(mod, 'login') && hasCredLogin(mod)) && !hasModAction(mod, 'session')) return;
        const st = authStatus(ad);
        if (!st.logged_in) {
            const canLogin = hasModAction(mod, 'login') && hasCredLogin(mod);
            const canSession = hasModAction(mod, 'session');
            let ctaBtns = '';
            if (canLogin) {
                ctaBtns += '<button class="btn-primary" type="button" onclick="openModuleLogin(\'' + mod.id + '\')">' + esc(lbl(mod, 'cta_logged_out_button', lbl(mod, 'login', t('app.login')))) + '</button>';
            }
            if (canSession) {
                ctaBtns += '<button class="btn-sm" type="button" style="margin-left:8px" onclick="openModuleSession(\'' + mod.id + '\')">' + esc(lbl(mod, 'session', t('app.import_session'))) + '</button>';
            }
            if (!ctaBtns) return;
            html += '<div class="cta"><div style="font-weight:700">' + esc(lbl(mod, 'cta_logged_out_title', t('app.not_logged_in', { adapter: adapterLabel(ad) }))) + '</div>' + '<p style="color:var(--muted);margin:12px 0">' + esc(lbl(mod, 'cta_logged_out_body', '')) + '</p>' + '<div style="display:flex;flex-wrap:wrap;gap:8px;justify-content:center;align-items:center">' + ctaBtns + '</div></div>';
        } else if (!pts.filter(p => p.adapter === ad).length && hasModAction(mod, 'sync')) {
            const ctaFn = hasModAction(mod, 'homes') ? 'openHomePicker' : 'syncModule';
            html += '<div class="cta"><div style="font-weight:700">' + esc(fillTpl(lbl(mod, 'cta_empty_title', adapterLabel(ad) + ' · {user}'), {
                user: st.user_id || '-'
            })) + '</div>' + '<p style="color:var(--muted);margin:12px 0">' + esc(lbl(mod, 'cta_empty_body', '')) + '</p>' + '<button class="btn-primary" onclick="' + ctaFn + '(\'' + mod.id + '\')">' + esc(lbl(mod, 'cta_empty_button', lbl(mod, 'sync', t('app.sync')))) + '</button></div>';
        }
    });
    const groupKeys = Object.keys(groups).sort().filter(adapterEnabled);
    if (!list.length && !html && !groupKeys.length) {
        lastOverviewHtml = '';
        el.innerHTML = '<div class="empty">' + t('app.no_devices') + '</div>';
        return;
    }
    if (!list.length && html && !groupKeys.length) {
        if (html === lastOverviewHtml) return;
        lastOverviewHtml = html;
        el.innerHTML = html;
        return;
    }
    html += groupKeys.map(ad => {
        const sectionPts = (filter === 'all')
            ? base.filter(p => pointIdScoped(p.point_id) && pointGroup(p) === ad)
            : base.filter(p => pointInAdapter(p, ad));
        const chips = adapterRoomChipsHtml(sectionPts);
        return '<div class="section">' + sectionHeader(ad, groups[ad].length) + chips + '<div class="grid">' + groups[ad].map(cardHtml).join('') + '</div></div>';
    }).join('');
    /* No adapter header and no new chip — these ids do not name an adapter. */
    if (filter === 'all' && loose.length) {
        html += '<div class="section"><div class="grid">' + loose.map(cardHtml).join('') + '</div></div>';
    }
    /* Structure signature: the pre-section CTAs (auth/rtsp) plus the ordered set
     * of device point_ids. When this is unchanged we only patch cards in place
     * (keeping icon <img> elements alive); otherwise a full rebuild is required. */
    const ctaPart = html.split('<div class="section"')[0];
    const structSig = ctaPart + '' + list.map(p => p.point_id).join('');
    if (structSig !== lastOverviewSig) {
        lastOverviewSig = structSig;
        lastOverviewHtml = html;
        el.innerHTML = html;
        rebuildCardEls(el);
        bindOverviewToggles(el);
        return;
    }
    if (html === lastOverviewHtml) return;
    /* Same card set/order: update in place to avoid re-fetching icons.
     * Force a full rebuild if any card newly appeared/disappeared (e.g. visibility
     * toggled) or a cached node is detached (e.g. view switch). */
    let needsFull = (Object.keys(cardEls).length === 0);
    if (!needsFull) {
        list.forEach(function (p) {
            const ex = cardEls[p.point_id];
            const tmp = document.createElement('div');
            tmp.innerHTML = cardHtml(p);
            const willRender = !!tmp.firstElementChild;
            if (!ex || !ex.isConnected || willRender !== !!ex) needsFull = true;
        });
    }
    if (needsFull) {
        lastOverviewSig = structSig;
        lastOverviewHtml = html;
        el.innerHTML = html;
        rebuildCardEls(el);
        bindOverviewToggles(el);
        return;
    }
    lastOverviewHtml = html;
    list.forEach(function (p) {
        const ex = cardEls[p.point_id];
        if (ex) updateCardEl(ex, p);
    });
}

/* Keyed card update: keep the device icon <img> element alive across state
 * updates so it is never recreated/re-fetched (prevents overview icon flicker).
 * cardEls maps point_id -> the live .dcard element; lastOverviewSig captures the
 * card set/order (+ pre-section CTAs) so we only do a full rebuild when the
 * structure changes and otherwise patch cards in place. */
let cardEls = {};
let lastOverviewSig = '';

function rebuildCardEls(root) {
    cardEls = {};
    root.querySelectorAll('.dcard').forEach(function (d) {
        const pid = d.getAttribute('data-pid');
        if (pid) cardEls[pid] = d;
    });
}

function bindOverviewToggles(root) {
    root.querySelectorAll('.toggle input:not(:disabled)').forEach(function (inp) {
        inp.onchange = function () { cmd(inp.dataset.id, inp.checked ? 'turn_on' : 'turn_off'); };
    });
}

function updateCardEl(existing, p) {
    const tmp = document.createElement('div');
    tmp.innerHTML = cardHtml(p);
    const newCard = tmp.firstElementChild;
    if (!newCard) {            /* device hidden (visible:false) */
        existing.remove();
        delete cardEls[p.point_id];
        return;
    }
    const oldImg = existing.querySelector('img.dw-device-icon');
    const newImg = newCard.querySelector('img.dw-device-icon');
    /* Reuse the existing <img> when its src is unchanged so the browser keeps
     * the decoded image instead of re-requesting it (the flicker source). */
    if (oldImg && newImg && oldImg.getAttribute('src') === newImg.getAttribute('src')) {
        /* The class carries the on/off dim state (dw-device-icon is-on / is-off).
         * Copy it onto the kept node: otherwise a device that was just toggled
         * keeps the previous icon brightness until the next full rebuild. */
        oldImg.className = newImg.className;
        newImg.parentNode.replaceChild(oldImg, newImg);
    }
    existing.replaceWith(newCard);
    cardEls[p.point_id] = newCard;
    const inp = newCard.querySelector('.toggle input:not(:disabled)');
    if (inp) {
        inp.onchange = function () { cmd(inp.dataset.id, inp.checked ? 'turn_on' : 'turn_off'); };
    }
}

let authDrafts = {};

function authDraftKey(id, mode) {
    return (id || '') + '::' + (mode || 'login');
}
