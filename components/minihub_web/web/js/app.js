/* MiniHub web — shared state, helpers, navigation */
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
const TITLES = {
    overview: '概览',
    map: '地图',
    rules: '规则',
    scenes: '场景',
    store: 'MiniHub Store',
    system: '系统'
};
const ADAPTER_LABEL = {
    mqtt: 'MQTT',
    zigbee: 'Zigbee',
    ble_mesh: 'BLE Mesh',
    matter: 'Matter'
};
const ADAPTER_HIDDEN = {
    thread: true
};
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
const TYPE_LABEL = {
    switch: '开关',
    light: '灯',
    sensor: '传感器',
    button: '按钮',
    binary_sensor: '传感',
    client: '终端',
    smoke: '烟感',
    router: '路由',
    car: '汽车',
    cover: '窗帘',
    camera: '摄像头',
    television: '电视',
    lock: '门锁',
    speaker: '音箱',
    'air-conditioner': '空调',
    fridge: '冰箱',
    water_purifier: '净水',
    water_heater: '热水',
    washer: '洗衣机',
    dishwasher: '洗碗',
    oven: '烤箱',
    appliance: '家电'
};

/* Prefer props.category (product name) over catalog type label. Widgets still use p.type. */
function dwTypeDisplay(p) {
    const pr = (p && p.props && typeof p.props === 'object') ? p.props : {};
    const cat = pr.category || pr.product || pr.product_name;
    if (cat != null && String(cat) !== '') {
        return String(cat);
    }
    const t = p && p.type;
    return (TYPE_LABEL[t] || t || '');
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
    return ADAPTER_LABEL[ad] || ad;
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

function fillTpl(s, vars) {
    return String(s || '').replace(/\{(\w+)\}/g, (m, k) => vars[k] != null ? vars[k] : m);
}

/* Protocol prefix on the point (`thread.color_bulb` → thread), not hardcoded. */
/* Vendor plugin claim. Radio commands still use p.adapter. */
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
    /* Device ids are <protocol>.<endpoint>[.<param>]; show the endpoint. */
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
        rs.innerHTML = '<option value="">全部房间</option>' + rooms.map(r => '<option value="' + escAttr(r) + '">' + esc(r) + '</option>').join('');
        rs.value = rooms.indexOf(cur) >= 0 ? cur : '';
        roomFilter = rs.value;
    }
    if (ts) {
        const cur = typeFilter;
        ts.innerHTML = '<option value="">全部类型</option>' + types.map(tp => '<option value="' + escAttr(tp) + '">' + esc(TYPE_LABEL[tp] || tp) + '</option>').join('');
        ts.value = types.indexOf(cur) >= 0 ? cur : '';
        typeFilter = ts.value;
    }
}

function adapterRoomChipsHtml(adapterPts) {
    const rooms = Array.from(new Set(adapterPts.map(pointRoomName).filter(Boolean))).sort();
    if (!rooms.length) return '';
    let h = '<div class="room-chips"><button type="button" class="chip' + (roomFilter === '' ? ' active' : '') + '" onclick="onRoomFilterChange(\'\')">全部房间</button>';
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
    /* Devices list lives on Overview only; no separate Devices nav page. */
    if (v === 'devices') v = 'overview';
    view = v;
    document.querySelectorAll('.view').forEach(el => el.hidden = true);
    const viewEl = document.getElementById('view-' + v);
    if (viewEl) viewEl.hidden = false;
    document.querySelectorAll('.nav-item').forEach(n => n.classList.toggle('active', n.dataset.view === v));
    document.getElementById('page-title').textContent = TITLES[v] || v;
    document.getElementById('sidebar').classList.remove('open');
    if (v === 'map') {
        ensureLeaflet().then(async () => {
            await renderMap();
            setTimeout(() => {
                if (leafletMap) leafletMap.invalidateSize();
            }, 150);
        }).catch(() => {
            const info = document.getElementById('map-info');
            if (info) info.textContent = '地图库加载失败（网络无法访问 CDN），概览不受影响';
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
async function loadPoints() {
    if (pointsLoading) return;
    pointsLoading = true;
    try {
        const r = await fetch('/api/points');
        if (!r.ok) throw new Error('points');
        const next = await r.json();
        const sig = JSON.stringify(next);
        const changed = sig !== lastPtsSig;
        lastPtsSig = sig;
        pts = next;
        const foot = document.getElementById('foot');
        if (foot) foot.textContent = '更新 ' + new Date().toLocaleTimeString();
        if (changed) {
            if (view === 'overview') renderOverview();
            if (view === 'map' && window.L) renderMap();
            /* Open detail modal: re-render from point via catalog widgets (any type). */
            if (typeof refreshDeviceDetail === 'function') refreshDeviceDetail();
        }
    } catch (e) {
        const c = document.getElementById('content');
        if (c && !pts.length) c.innerHTML = '<div class="empty">加载失败</div>';
    } finally {
        pointsLoading = false;
    }
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
    /* Warm type catalog so Overview cards can show /img/devices/<type>.svg. */
    if (typeof ensureDwCatalog === 'function' && typeof dwCatalog !== 'undefined' && !dwCatalog) {
        ensureDwCatalog().then(function () {
            if (view === 'overview') renderOverview();
        }).catch(function () {});
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
                ctaBtns += '<button class="btn-primary" type="button" onclick="openModuleLogin(\'' + mod.id + '\')">' + esc(lbl(mod, 'cta_logged_out_button', lbl(mod, 'login', '登录'))) + '</button>';
            }
            if (canSession) {
                ctaBtns += '<button class="btn-sm" type="button" style="margin-left:8px" onclick="openModuleSession(\'' + mod.id + '\')">' + esc(lbl(mod, 'session', '导入会话')) + '</button>';
            }
            if (!ctaBtns) return;
            html += '<div class="cta"><div style="font-weight:700">' + esc(lbl(mod, 'cta_logged_out_title', adapterLabel(ad) + ' 未登录')) + '</div>' + '<p style="color:var(--muted);margin:12px 0">' + esc(lbl(mod, 'cta_logged_out_body', '')) + '</p>' + '<div style="display:flex;flex-wrap:wrap;gap:8px;justify-content:center;align-items:center">' + ctaBtns + '</div></div>';
        } else if (!pts.filter(p => p.adapter === ad).length && hasModAction(mod, 'sync')) {
            const ctaFn = hasModAction(mod, 'homes') ? 'openHomePicker' : 'syncModule';
            html += '<div class="cta"><div style="font-weight:700">' + esc(fillTpl(lbl(mod, 'cta_empty_title', adapterLabel(ad) + ' · {user}'), {
                user: st.user_id || '-'
            })) + '</div>' + '<p style="color:var(--muted);margin:12px 0">' + esc(lbl(mod, 'cta_empty_body', '')) + '</p>' + '<button class="btn-primary" onclick="' + ctaFn + '(\'' + mod.id + '\')">' + esc(lbl(mod, 'cta_empty_button', lbl(mod, 'sync', '同步'))) + '</button></div>';
        }
    });
    const groupKeys = Object.keys(groups).sort().filter(adapterEnabled);
    if (!list.length && !html && !groupKeys.length) {
        el.innerHTML = '<div class="empty">暂无设备</div>';
        return;
    }
    if (!list.length && html && !groupKeys.length) {
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
    el.innerHTML = html;
    el.querySelectorAll('.toggle input:not(:disabled)').forEach(inp => {
        inp.onchange = () => cmd(inp.dataset.id, inp.checked ? 'turn_on' : 'turn_off');
    });
}

let authDrafts = {};

function authDraftKey(id, mode) {
    return (id || '') + '::' + (mode || 'login');
}
