async function moduleRpc(id, method, params) {
    const r = await fetch('/api/modules/' + id + '/rpc', {
        method: 'POST',
        headers: {
            'Content-Type': 'application/json'
        },
        body: JSON.stringify({
            method,
            params: params || {}
        })
    });
    let j = null;
    try {
        j = await r.json();
    } catch (e) {}
    return {
        ok: r.ok,
        json: j
    };
}

function escAttr(s) {
    return esc(s).replace(/"/g, '&quot;');
}

function rtspCamMod() {
    return modules.find(x => x.id === 'rtsp_cam' && x.installed !== false && x.enabled !== false) || null;
}

function fmtRtspBytes(n) {
    n = Number(n) || 0;
    if (n < 1024) return n + ' B';
    if (n < 1048576) return (n / 1024).toFixed(1) + ' KB';
    return (n / 1048576).toFixed(1) + ' MB';
}
async function loadRtspCamStatus() {
    if (!rtspCamMod()) {
        rtspCamStatus = null;
        return;
    }
    try {
        const st = await moduleRpc('rtsp_cam', 'status');
        rtspCamStatus = (st && st.json) ? st.json : {
            result: 'error',
            last_error: 'status failed'
        };
    } catch (e) {
        rtspCamStatus = {
            result: 'error',
            last_error: 'network'
        };
    }
}

function renderRtspCamCard() {
    if (!rtspCamMod()) return '';
    const st = rtspCamStatus || {};
    const usb = !!st.usb_mounted;
    const rec = !!st.recording;
    const file = st.current_file ? String(st.current_file).split('/').pop() : '';
    const bytes = fmtRtspBytes(st.bytes_written);
    const err = st.last_error ? ('<div class="modal-hint" style="margin-top:8px;color:var(--bad)">' + esc(String(st.last_error)) + '</div>') : '';
    const status = esc((usb ? t('ov.usb_mounted') : t('ov.usb_not_mounted')) + ' · ' + (rec ? t('ov.recording') : t('ov.stopped')) + (file ? ' · ' + file : '') + ((rec || st.bytes_written) ? ' · ' + bytes : ''));
    const preview = '<div id="rtsp-cam-preview" style="aspect-ratio:16/9;max-width:28rem;margin:0 auto 12px;border-radius:10px;border:1px solid var(--border);background:var(--card);display:flex;align-items:center;justify-content:center;color:var(--muted);font-size:.85rem">' + t('ov.no_preview') + '</div>';
    let acts = '';
    if (!rec) {
        acts += '<button class="btn-primary" type="button" ' + (usb ? '' : 'disabled ') + 'onclick="rtspCamStart()">' + t('ov.start') + '</button>';
    } else {
        acts += '<button class="btn-sm bad" type="button" onclick="rtspCamStop()">' + t('ov.stop') + '</button>';
    }
    acts += '<a class="btn-sm" style="text-decoration:none;display:inline-block" href="/modules/rtsp_cam/web/">' + t('ov.details') + '</a>';
    return '<div class="cta" id="rtsp-cam-card"><div style="font-weight:700">' + t('ov.camera_recording') + '</div><p style="color:var(--muted);margin:12px 0">' + status + '</p>' + preview + '<div style="display:flex;flex-wrap:wrap;gap:8px;justify-content:center;align-items:center">' + acts + '</div>' + err + '</div>';
}
async function rtspCamStart() {
    toast(t('ov.starting_recording'));
    try {
        await moduleRpc('rtsp_cam', 'start');
        for (let i = 0; i < 25; i++) {
            await loadRtspCamStatus();
            if (rtspCamStatus && rtspCamStatus.recording) {
                toast(t('ov.started'));
                if (view === 'overview') renderOverview();
                return;
            }
            await new Promise(res => setTimeout(res, 200));
            if (view === 'overview') renderOverview();
        }
        toast((rtspCamStatus && rtspCamStatus.last_error) || t('ov.start_failed'));
    } catch (e) {
        toast(t('ov.network_error'));
        await loadRtspCamStatus();
    }
    if (view === 'overview') renderOverview();
}
async function rtspCamStop() {
    toast(t('ov.stopping_recording'));
    try {
        const r = await moduleRpc('rtsp_cam', 'stop');
        if (!(r.ok && r.json && r.json.result === 'ok')) {
            toast((r.json && (r.json.error || r.json.last_error)) || t('ov.stop_failed'));
        } else {
            for (let i = 0; i < 20; i++) {
                await loadRtspCamStatus();
                if (!(rtspCamStatus && rtspCamStatus.recording)) {
                    toast(t('ov.stopped'));
                    break;
                }
                await new Promise(res => setTimeout(res, 200));
            }
            if (rtspCamStatus && rtspCamStatus.recording) toast(t('ov.still_recording'));
        }
    } catch (e) {
        toast(t('ov.network_error'));
    }
    await loadRtspCamStatus();
    if (view === 'overview') renderOverview();
}

function hashOffset(id) {
    let h = 0;
    for (let i = 0; i < id.length; i++) h = (h * 31 + id.charCodeAt(i)) >>> 0;
    const a = (h % 360) * Math.PI / 180,
        d = 0.0008 + ((h >> 8) % 12) * 0.00025;
    return {
        dlat: Math.cos(a) * d,
        dlon: Math.sin(a) * d
    };
}

function inChina(g) {
    if (g.country && (g.country === 'China' || g.country === '中国')) return true;
    return g.lat >= 0.8293 && g.lat <= 55.8271 && g.lon >= 72.004 && g.lon <= 137.8347;
}

function wgs84togcj02(wgLat, wgLon) {
    if (wgLat < 0.8293 || wgLat > 55.8271 || wgLon < 72.004 || wgLon > 137.8347) return [wgLat, wgLon];
    const a = 6378245,
        e2 = 0.006693421622965943;

    function tlat(x, y) {
        let r = -100 + 2 * x + 3 * y + 0.2 * y * y + 0.1 * x * y + 0.2 * Math.sqrt(Math.abs(x));
        r += (20 * Math.sin(6 * x * Math.PI) + 20 * Math.sin(2 * x * Math.PI)) * 2 / 3;
        r += (20 * Math.sin(y * Math.PI) + 40 * Math.sin(y / 3 * Math.PI)) * 2 / 3;
        r += (160 * Math.sin(y / 12 * Math.PI) + 320 * Math.sin(y * Math.PI / 30)) * 2 / 3;
        return r;
    }

    function tlon(x, y) {
        let r = 300 + x + 2 * y + 0.1 * x * x + 0.1 * x * y + 0.1 * Math.sqrt(Math.abs(x));
        r += (20 * Math.sin(6 * x * Math.PI) + 20 * Math.sin(2 * x * Math.PI)) * 2 / 3;
        r += (20 * Math.sin(x * Math.PI) + 40 * Math.sin(x / 3 * Math.PI)) * 2 / 3;
        r += (150 * Math.sin(x / 12 * Math.PI) + 300 * Math.sin(x / 30 * Math.PI)) * 2 / 3;
        return r;
    }
    let dlat = tlat(wgLon - 105, wgLat - 35),
        dlon = tlon(wgLon - 105, wgLat - 35);
    const rad = wgLat / 180 * Math.PI;
    let magic = Math.sin(rad);
    magic = 1 - e2 * magic * magic;
    const sqrtm = Math.sqrt(magic);
    dlat = (dlat * 180) / ((a * (1 - e2)) / (magic * sqrtm) * Math.PI);
    dlon = (dlon * 180) / (a / sqrtm * Math.cos(rad) * Math.PI);
    return [wgLat + dlat, wgLon + dlon];
}

function addMapBasemap(map, g) {
    const cn = g.map_provider === 'amap' || inChina(g);
    const osm = L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png', {
        attribution: '© OpenStreetMap',
        maxZoom: 19
    });
    if (!cn) {
        osm.addTo(map);
        return osm;
    }
    const amap = L.tileLayer('https://webrd0{s}.is.autonavi.com/appmaptile?lang=zh_cn&size=1&scale=1&style=8&x={x}&y={y}&z={z}', {
        subdomains: '1234',
        maxZoom: 18,
        attribution: t('ov.map_attribution')
    });
    amap.addTo(map);
    let fb = false;
    amap.on('tileerror', () => {
        if (fb) return;
        fb = true;
        map.removeLayer(amap);
        osm.addTo(map);
    });
    return amap;
}
async function loadGeo(force) {
    if (!force && geoLoc) return geoLoc;
    try {
        const r = await fetch('/api/location' + (force ? '?refresh=1' : ''));
        if (r.ok) {
            const j = await r.json();
            if (j.ok && j.lat) {
                geoLoc = j;
                return j;
            }
        }
    } catch (e) {}
    try {
        const r = await fetch('http://ip-api.com/json/?lang=zh-CN&fields=status,city,regionName,country,lat,lon,query');
        if (r.ok) {
            const d = await r.json();
            if (d.status === 'success') {
                geoLoc = {
                    ok: true,
                    source: 'browser',
                    lat: d.lat,
                    lon: d.lon,
                    city: d.city,
                    region: d.regionName,
                    country: d.country,
                    public_ip: d.query,
                    address: [d.city, d.regionName, d.country].filter(Boolean).join(', ')
                };
                return geoLoc;
            }
        }
    } catch (e) {}
    return null;
}
function bindChips() {
    document.querySelectorAll('#chips .chip').forEach(c => {
        c.onclick = () => {
            document.querySelectorAll('#chips .chip').forEach(x => x.classList.remove('active'));
            c.classList.add('active');
            filter = c.dataset.f;
            renderOverview();
        };
    });
}

function renderToolbar() {
    let h = '<button class="chip' + (filter === 'all' ? ' active' : '') + '" data-f="all">' + t('ov.all') + '</button>';
    Object.keys(enabledAdapters).filter(adapterEnabled).sort().forEach(ad => {
        h += '<button class="chip' + (filter === ad ? ' active' : '') + '" data-f="' + ad + '">' + esc(adapterLabel(ad)) + '</button>';
    });
    document.getElementById('chips').innerHTML = h;
    bindChips();
    if (filter !== 'all' && !adapterEnabled(filter)) filter = 'all';
}

function localName() {
    return localStorage.getItem('minihub_display_name') || t('ov.user');
}

/* Compact Store upgrade control after group title+version+count — only when update exists. */
function sectionModuleUpgradeHtml(ad) {
    const list = (typeof modules !== 'undefined' && Array.isArray(modules)) ? modules : [];
    const mod = list.find(function (x) {
        return x && x.installed !== false && (x.adapter === ad || x.id === ad);
    });
    if (!mod || mod.managed === false) return '';
    const cat = (typeof storeCatalog !== 'undefined' && Array.isArray(storeCatalog))
        ? storeCatalog.find(function (x) { return x && x.id === mod.id; })
        : null;
    const up = !!(cat && cat.upgrade_available) || !!mod.binary_stale;
    if (!up) return '';
    const kind = (cat && cat.type === 'pack') ? 'pack' : 'module';
    let label = t('ov.upgrade');
    if (cat && cat.version) label += ' ' + cat.version;
    return '<button class="btn-sm on section-mod-upg" type="button" title="' + t('ov.module_upgrade_title') + '" onclick="storeUpgrade(\'' +
        kind + '\',\'' + esc(mod.id) + '\')">' + esc(label) + '</button>';
}

function sectionHeader(ad, count) {
    const ver = adapterVersion(ad);
    const verTag = ver ? '<span class="mod-acct">v' + esc(ver) + '</span>' : '';
    const showActs = (filter === 'all' || filter === ad);
    const upg = sectionModuleUpgradeHtml(ad);
    const mod = authModuleFor(ad);
    if (mod) {
        const mid = mod.id;
        const st = authStatus(ad);
        let acct;
        if (st.logged_in) {
            acct = '<span class="mod-acct">' + esc(st.user_id || '-') + (st.region ? ' · ' + esc(st.region) : '') + '</span>';
        } else if (!st.loaded) {
            /* Status not yet known (first paint / reboot) — show a neutral
             * "checking" badge instead of a false "not logged in". */
            acct = showActs ? '<span class="mod-acct off">' + esc(t('ov.checking')) + '</span>' : '';
        } else {
            acct = showActs ? '<span class="mod-acct off">' + esc(lbl(mod, 'logged_out', t('ov.logged_out'))) + '</span>' : '';
        }
        /* "stalled" only qualifies a sync the plugin still claims: once
         * st.syncing clears, the sync really finished and no badge belongs. */
        const stalled = authSyncStalled(ad) && !!(st && st.syncing);
        /* A "syncing" badge means the poll MUST be running; otherwise a loadMeta()
         * that aborted before its poll-start (line ~514) would leave syncing frozen
         * until a manual refresh. Drive the poll from the badge itself so it cannot
         * get stuck (issue: sync badge hangs on "同步中"). */
        const syncingNow = !stalled && (st.syncing || authModuleSyncing(mid));
        /* When the plugin reports a sync progress (0-100) in its status, show it
         * on the badge as "同步中 42%". The plugin must populate st.progress while
         * syncing; without it we fall back to the plain "同步中". */
        let syncingLabel = t('ov.syncing');
        if (typeof st.progress === 'number' && st.progress >= 0 && st.progress <= 100) {
            syncingLabel = t('ov.syncing_pct', { pct: Math.round(st.progress) });
        }
        const syncBadge = stalled
            ? '<span class="mod-acct off">' + t('ov.sync_stalled') + '</span>'
            : (syncingNow ? '<span class="mod-acct busy">' + syncingLabel + '</span>' : '');
        if (syncingNow) {
            if (window.MINIHUB_SYNC_DEBUG) console.log('[syncpoll] render-start poll for ' + ad);
            pollAuthSyncWhileBusy();
        }
        let acts = '';
        if (showActs) {
            if (st.logged_in) {
                if (hasModAction(mod, 'sync')) acts += '<button class="btn-sm on" type="button" onclick="syncModule(\'' + mid + '\')">' + esc(lbl(mod, 'sync', t('ov.sync'))) + '</button>';
                if (hasModAction(mod, 'homes')) acts += '<button class="btn-sm on" type="button" onclick="openHomePicker(\'' + mid + '\')">' + esc(lbl(mod, 'change_home', (st.home_name ? t('ov.switch_home') : t('ov.choose_home')))) + '</button>';
                if (hasModAction(mod, 'logout')) acts += '<button class="btn-sm bad" type="button" onclick="logoutModule(\'' + mid + '\')">' + esc(lbl(mod, 'logout', t('ov.sign_out'))) + '</button>';
            } else {
                if (hasModAction(mod, 'login') && hasCredLogin(mod)) {
                    acts += '<button class="btn-sm on" type="button" onclick="openModuleLogin(\'' + mid + '\')">' + esc(lbl(mod, 'login', t('ov.login'))) + '</button>';
                }
                if (hasModAction(mod, 'session')) {
                    acts += '<button class="btn-sm" type="button" onclick="openModuleSession(\'' + mid + '\')">' + esc(lbl(mod, 'session', t('ov.import_session'))) + '</button>';
                }
            }
        }
        return '<div class="section-h auth-section-h" data-ad="' + esc(ad) + '"><span>' + esc(adapterLabel(ad)) + '</span>' + verTag + acct + syncBadge + '<span>' + count + '</span>' + upg + (acts ? '<span class="section-actions">' + acts + '</span>' : '') + '</div>';
    }
    if (ad === 'matter') {
        const st = matterStatus.ready ? '<span class="mod-acct">' + t('ov.matter_controller_ready') + '</span>' : (showActs ? '<span class="mod-acct off">' + t('ov.not_ready') + '</span>' : '');
        const running = matterStatus.commission === 'running';
        const acts = showActs ? (running
            ? '<button class="btn-sm bad" type="button" onclick="cancelMatterCommission()">' + t('ov.cancel_commission') + '</button>'
            : '<button class="btn-sm on" type="button" onclick="openMatterAdd()">' + t('ov.add') + '</button>') : '';
        return '<div class="section-h matter-section-h"><span>' + mhAdapterLabel(ad) + '</span>' + verTag + st + '<span>' + count + '</span>' + upg + (acts ? '<span class="section-actions">' + acts + '</span>' : '') + '</div>';
    }
    if (ad === 'zigbee') {
        const zbOn = !!radioProv.zigbee_permit_join;
        const bmBusy = !!(radioProv.ble_mesh_provisioning || radioProv.ble_mesh_permit_provision);
        const st = zbOn ? '<span class="mod-acct">' + t('ov.joining') + '</span>' : (bmBusy ? '<span class="mod-acct off">' + t('ov.ble_mesh_provisioning') + '</span>' : '');
        const acts = showActs ? (zbOn ? '<button class="btn-sm" type="button" onclick="stopZigbeeJoin()">' + t('ov.stop_join') + '</button>' : (bmBusy ? '<button class="btn-sm on" type="button" disabled title="' + t('ov.ble_mesh_provisioning') + '">' + t('ov.allow_join') + '</button>' : '<button class="btn-sm on" type="button" onclick="permitZigbeeJoin()">' + t('ov.allow_join') + '</button>')) : '';
        return '<div class="section-h radio-section-h"><span>' + esc(adapterLabel(ad)) + '</span>' + verTag + st + '<span>' + count + '</span>' + upg + (acts ? '<span class="section-actions">' + acts + '</span>' : '') + '</div>';
    }
    if (ad === 'ble_mesh') {
        const bmPermit = !!radioProv.ble_mesh_permit_provision;
        const bmProv = !!radioProv.ble_mesh_provisioning;
        const bmOn = bmPermit || bmProv;
        const zbBusy = !!radioProv.zigbee_permit_join;
        const st = bmOn ? '<span class="mod-acct">' + t('ov.provisioning') + '</span>' : (zbBusy ? '<span class="mod-acct off">' + t('ov.zigbee_joining') + '</span>' : '');
        const acts = showActs ? (bmPermit ? '<button class="btn-sm" type="button" onclick="stopBleMeshProvision()">' + t('ov.stop_provisioning') + '</button>' : (bmProv ? '<button class="btn-sm on" type="button" disabled>' + t('ov.provisioning') + '</button>' : (zbBusy ? '<button class="btn-sm on" type="button" disabled title="' + t('ov.zigbee_joining') + '">' + t('ov.allow_provisioning') + '</button>' : '<button class="btn-sm on" type="button" onclick="permitBleMeshProvision()">' + t('ov.allow_provisioning') + '</button>'))) : '';
        return '<div class="section-h radio-section-h"><span>' + esc(adapterLabel(ad)) + '</span>' + verTag + st + '<span>' + count + '</span>' + upg + (acts ? '<span class="section-actions">' + acts + '</span>' : '') + '</div>';
    }
    return '<div class="section-h">' + esc(adapterLabel(ad)) + verTag + '<span>' + count + '</span>' + upg + '</div>';
}
let pointsLoading = false,
    lastPtsSig = '',
    lastOverviewHtml = '',
    rtspStatusLoading = false;

function pointsUiSig(list) {
    if (!Array.isArray(list)) return '';
    try {
        return JSON.stringify(list.map(function (p) {
            if (!p || typeof p !== 'object') return p;
            const o = {};
            for (const k in p) {
                if (!Object.prototype.hasOwnProperty.call(p, k)) continue;
                if (k === 'updated_ms') continue;
                o[k] = p[k];
            }
            return o;
        }));
    } catch (e) {
        return JSON.stringify(list);
    }
}
function rulesToggleHtml(id, enabled) {
    const on = enabled !== false;
    return '<button class="switch ' + (on ? 'on' : '') + '" type="button" title="' + (on ? t('rules.disable') : t('rules.enable')) + '" onclick="event.stopPropagation();rulesToggleEnabled(\'' + escAttr(id) + '\')"><span class="switch-knob"></span></button>';
}

function renderRules() {
    const list = document.getElementById('rules-list');
    const detail = document.getElementById('rules-detail');
    const edit = document.getElementById('rules-edit');
    if (!list || !detail) return;
    if (edit) {
        if (rulesEditMode) {
            list.hidden = true;
            detail.hidden = true;
            edit.hidden = false;
            return;
        }
        edit.hidden = true;
        edit.innerHTML = '';
    }
    if (rulesSel) {
        const rule = (rulesCfg.rules || []).find(function(x) {
            return x.id === rulesSel;
        });
        list.hidden = true;
        detail.hidden = false;
        if (!rule) {
            rulesSel = null;
            renderRules();
            return;
        }
        const iff = Array.isArray(rule.if) ? rule.if : [];
        const dis = rule.enabled === false;
        const w = whenPlain(rule.when);
        const i = ifPlain(iff);
        const th = thenPlain(rule.then);
        detail.innerHTML = '<button class="btn-sm" type="button" onclick="rulesBack()">' + t('ov.back_to_list') + '</button>' +
            '<button class="btn-sm on" type="button" style="margin-left:8px" onclick="rulesEditOpen(\'' + escAttr(rule.id) + '\')">' + t('ov.edit') + '</button>' +
            rulesToggleHtml(rule.id, rule.enabled) +
            '<div class="dcard" style="margin-top:12px">' +
            '<div class="card-top"><div class="card-name">' + esc(rule.name || rule.id || t('ov.rule')) + '</div>' + (dis ? '' : '<span class="badge sensor">' + t('ov.enabled_badge') + '</span>') + '</div>' +
            (dis ? '<div class="card-meta"><span>' + t('ov.disabled') + '</span></div>' : '') +
            '<div class="rule-flow">' + ruleStepHtml(t('ov.rule_when'), w) + '<div class="rule-arrow">↓</div>' + ruleStepHtml(t('ov.rule_if'), i) + '<div class="rule-arrow">↓</div>' + ruleStepHtml(t('ov.rule_then'), th) + '</div>' +
            '<details class="rule-tech"><summary>' + t('ov.rule_tech_details') + '</summary>' +
            '<h4 class="rule-sec">when</h4>' +
            '<pre class="rule-pre">' + esc(JSON.stringify(rule.when || {}, null, 2)) + '</pre>' +
            '<h4 class="rule-sec">if</h4><pre class="rule-pre">' + esc(JSON.stringify(iff, null, 2)) + '</pre>' +
            '<h4 class="rule-sec">then</h4><pre class="rule-pre">' + esc(JSON.stringify(rule.then || {}, null, 2)) + '</pre>' +
            '</details></div>';
        return;
    }
    detail.hidden = true;
    list.hidden = false;
    const arr = rulesCfg.rules || [];
    if (!arr.length) {
        list.className = '';
        list.innerHTML = '<div class="empty">' + t('ov.no_rules') + '</div>';
        return;
    }
    list.className = 'grid';
    list.innerHTML = arr.map(function(r) {
        const dis = r.enabled === false;
        return '<div class="dcard" style="cursor:pointer" data-rule-id="' + escAttr(r.id || '') + '">' +
            '<div class="card-top"><div class="card-name">' + esc(r.name || r.id || '(unnamed)') + '</div>' +
            (dis ? '' : '<span class="badge sensor">' + t('ov.enabled_badge') + '</span>') +
            rulesToggleHtml(r.id, r.enabled) +
            '<button class="btn-sm bad" type="button" title="' + escAttr(t('ov.delete')) + '" onclick="event.stopPropagation();rulesDelete(\'' + escAttr(r.id || '') + '\')">' + t('ov.delete') + '</button>' +
            '</div>' +
            '<div class="card-meta"><span>' + esc(whenSummary(r.when)) + ' → ' + esc(thenSummary(r.then)) + '</span>' + (dis ? '<span>' + t('ov.disabled_short') + '</span>' : '') + '</div></div>';
    }).join('');
    list.onclick = function(ev) {
        const card = ev.target.closest('[data-rule-id]');
        if (!card) return;
        rulesSel = card.getAttribute('data-rule-id');
        renderRules();
    };
}

function propsHtml(p) {
    if (!p.props || typeof p.props !== 'object') return '';
    const meterOrder = ['power_w', 'voltage_v', 'current_a', 'energy_wh'];
    const otherOrder = ['humidity'];
    const skip = new Set(['extras', 'params', 'owner', 'indicator', 'volume', 'dnd', 'motion_detection', 'motion_tracking', 'time_watermark', 'wdr', 'play_text', 'execute_text', 'open', 'zone_status', 'tamper', 'battery_low', 'name', 'room', 'room_name', 'room_id', 'did', 'home_name', 'home_id', 'appliance_id', 'category', 'product', 'product_name', 'no_control', 'transport', 'commissioning', 'commission_phase', 'detail_rows', 'ieee', 'uuid', 'node_id', 'manufacturer', 'vendor', 'manufacturer_name', 'model', 'model_id', 'model_identifier', 'sw_version', 'fw_version', 'firmware_version', 'app_version', 'device_version', 'hw_version', 'hardware_version', 'ota', 'cold_temp', 'cold_set', 'freeze_temp', 'freeze_set', 'left_zone_temp', 'right_zone_temp', 'tea_temp', 'milk_temp', 'target_temp', 'mode_label', 'card_summary', 'in_tds', 'out_tds', 'filter_ro_life', 'filter_ro_max', 'water_total_l', 'status_text', 'battery', 'stream_video_codec', 'stream_audio_codec', 'supports_brightness', 'supports_color_temp', 'supports_color', 'color_mode', 'color', 'mode_label', 'fan_label', 'mode', 'target_temp', 'temperature', 'fan_level', 'brightness', 'color_temp', 'position', 'target_position'].concat(meterOrder).concat(otherOrder));
    const labels = {
        power_w: t('ov.prop_power'),
        voltage_v: t('ov.prop_voltage'),
        current_a: t('ov.prop_current'),
        energy_wh: t('ov.prop_energy'),
        humidity: t('ov.prop_humidity'),
        app_version: t('ov.prop_app_version'),
        hw_version: t('ov.prop_hw_version')
    };
    const units = {
        power_w: ' W',
        voltage_v: ' V',
        current_a: ' A',
        energy_wh: ' Wh',
        humidity: ' %'
    };
    const meterKeys = meterOrder.filter(k => p.props[k] != null && p.props[k] !== '');
    const otherKeys = otherOrder.filter(k => p.props[k] != null && p.props[k] !== '');
    const customKeys = Object.keys(p.props).filter(k => !skip.has(k) && !/^(siid|piid|motor_)/.test(k) && p.props[k] != null && p.props[k] !== '');
    const keys = meterKeys.concat(otherKeys).concat(customKeys);
    if (!keys.length) return '';
    return '<div class="card-props">' + keys.map(k => {
        const v = p.props[k];
        let n = v;
        if (typeof v === 'number' || (typeof v === 'string' && v !== '' && !isNaN(Number(v)))) {
            let x = Number(v);
            if (k === 'current_a') {
                if (x > 20) x = x / 1000;
                n = x.toFixed(3);
            } else if (k === 'power_w' || k === 'voltage_v' || k === 'energy_wh' || k === 'humidity') n = x.toFixed(1);
            else if (Number.isInteger(x)) n = String(x);
            else n = x.toFixed(2);
        } else if (typeof v === 'boolean') {
            n = v ? t('ov.yes') : t('ov.no');
        }
        return '<span>' + esc(labels[k] || k) + ' ' + esc(String(n)) + (units[k] || '') + '</span>';
    }).join('') + '</div>';
}

function climateSummary(p) {
    const pr = p.props || {};
    /* When the value line already is "有人 20261014 13:11", do not repeat it. */
    if (pr.card_summary && !(p.value === 'detected' &&
            String(pr.card_summary).indexOf(t('ov.detected') + ' ') === 0)) {
        return '<div class="card-summary">' + esc(String(pr.card_summary)) + '</div>';
    }
    const bits = [];
    if (p.type === 'air-conditioner') {
        if (pr.mode_label) bits.push(pr.mode_label);
        if (pr.target_temp != null) bits.push(t('ov.set_temp', { v: pr.target_temp }));
        if (pr.fan_label) bits.push(pr.fan_label);
        if (pr.temperature != null) bits.push(t('ov.indoor_temp', { v: pr.temperature }));
    } else if (p.type === 'fridge') {
        if (pr.cold_temp != null) bits.push(t('ov.fridge_cold_temp', { v: pr.cold_temp }));
        if (pr.freeze_temp != null) bits.push(t('ov.fridge_freeze_temp', { v: pr.freeze_temp }));
    } else if (p.type === 'water_purifier') {
        if (pr.out_tds != null) bits.push(t('ov.out_tds', { v: pr.out_tds }));
        if (pr.filter_ro_life != null) bits.push(t('ov.ro_filter_months', { v: pr.filter_ro_life }));
    } else if (p.type === 'water_heater') {
        if (pr.temperature != null) bits.push(t('ov.water_temp', { v: pr.temperature }));
    }
    return bits.length ? '<div class="card-summary">' + esc(bits.join(' · ')) + '</div>' : '';
}

function isCoverType(type) {
    return type === 'cover' || type === 'curtain' || type === 'airer';
}

function coverSummary(p) {
    if (!isCoverType(p.type)) return '';
    const pct = parseInt(p.value, 10);
    const s = Number.isFinite(pct) ? t('ov.cover_open_pct', { v: pct }) : '';
    return s ? '<div class="card-summary">' + esc(s) + '</div>' : '';
}

function batterySummaryBit(p) {
    const b = (p.props || {}).battery;
    return (b != null && b !== '') ? t('ov.battery_pct', { v: b }) : '';
}

function magnetSummary(p) {
    if (p.type !== 'binary_sensor' && p.type !== 'sensor_contact' && p.type !== 'button') return '';
    const bits = [];
    const bat = batterySummaryBit(p);
    if (bat) bits.push(bat);
    return bits.length ? '<div class="card-summary">' + esc(bits.join(' · ')) + '</div>' : '';
}

function cameraStreamText(p) {
    const pr = (p && p.props) || {};
    const bits = [];
    if (pr.stream_video_codec != null) bits.push(t('ov.video_codec', { v: pr.stream_video_codec }));
    if (pr.stream_audio_codec != null) bits.push(t('ov.audio_codec', { v: pr.stream_audio_codec }));
    return bits.length ? (t('ov.stream_prefix') + bits.join(' · ')) : '';
}

function meterSummary(p) {
    const pr = p.props || {};
    if (pr.voltage == null && pr.current == null && pr.power == null && pr.energy == null && pr.temperature == null) return '';
    const bits = [];
    if (pr.voltage != null) bits.push(Number(pr.voltage) + 'V');
    if (pr.current != null) bits.push(Number(pr.current) + 'A');
    if (pr.power != null) bits.push(Number(pr.power) + 'W');
    if (pr.energy != null) bits.push(Number(pr.energy) + 'kWh');
    if (pr.temperature != null) bits.push(Number(pr.temperature) + '°');
    if (pr.fault != null && Number(pr.fault) !== 0) bits.push(t('ov.fault_code', { v: pr.fault }));
    return bits.length ? '<div class="card-summary">' + esc(bits.join(' · ')) + '</div>' : '';
}

function showDeviceLogBtn(p) {
    if (!p) return false;
    if (p.type === 'button') return true;
    if (p.type !== 'binary_sensor') return false;
    const v = p.value;
    return v === 'detected' || v === 'clear' || v === 'open' || v === 'closed' || v === 'left_open';
}

function deviceLogLabel(p, newv, oldv, cloudLabel) {
    if (cloudLabel) return cloudLabel;
    const v = newv != null ? newv : (p && p.value);
    if (v === 'detected') return t('ov.motion_detected');
    if (v === 'clear') return t('ov.no_motion');
    if (v === 'open' || v === 'left_open') return t('ov.open');
    if (v === 'closed') return t('ov.closed');
    if (p && p.type === 'button') return (newv && newv !== 'idle') ? String(newv) : t('ov.button_press');
    if (oldv != null && newv != null) return esc(String(oldv)) + ' → ' + esc(String(newv));
    return esc(String(v || t('ov.event')));
}

function fmtDeviceLogTime(ts) {
    const n = Number(ts);
    if (!Number.isFinite(n) || n <= 0) return '—';

    if (n >= 1577836800000) {
        const off = Number(window.hubTzOffsetMin) || 0;
        const d = new Date(n + off * 60000);
        const p = x => String(x).padStart(2, '0');
        return d.getUTCFullYear() + '-' + p(d.getUTCMonth() + 1) + '-' + p(d.getUTCDate()) + ' ' + p(d.getUTCHours()) + ':' + p(d.getUTCMinutes()) + ':' + p(d.getUTCSeconds());
    }
    const sec = Math.floor(n / 1000);
    const h = Math.floor(sec / 3600),
        m = Math.floor((sec % 3600) / 60),
        s = sec % 60;
    const p = x => String(x).padStart(2, '0');
    return t('ov.uptime', { h: h, m: p(m), s: p(s) });
}

function closeDeviceLog() {
    const m = document.getElementById('device-log-modal');
    if (m) m.hidden = true;
    window._deviceLogId = null;
}

function renderDeviceLogRows(rows) {
    const list = document.getElementById('device-log-list');
    if (!list) return;
    if (!rows || !rows.length) {
        list.innerHTML = '<div class="modal-hint">' + t('ov.no_log_records') + '</div>';
        return;
    }
    list.innerHTML = rows.map(r => {
        const lab = esc(r.label || '');
        const src = r.source === 'cloud' ? t('ov.cloud') : (r.source === 'local' ? t('ov.local') : '');
        return '<div class="device-log-row"><div class="device-log-label">' + lab + (src ? '<span class="device-log-src">' + src + '</span>' : '') + '</div><div class="device-log-time">' + esc(fmtDeviceLogTime(r.ts_ms)) + '</div></div>';
    }).join('');
}

function mergeDeviceLog(localItems, cloudItems, p) {
    const rows = [];
    (localItems || []).forEach(it => {
        rows.push({
            ts_ms: it.ts_ms,
            label: deviceLogLabel(p, it['new'], it['old'], null),
            source: 'local',
            _k: String(it.ts_ms) + '|' + (it['new'] || '')
        });
    });
    (cloudItems || []).forEach(it => {
        const lab = it.label || t('ov.event');
        const k = String(it.ts_ms) + '|' + lab;
        const near = rows.some(r => Math.abs(Number(r.ts_ms) - Number(it.ts_ms)) < 2000 && r.label === lab);
        if (near) return;
        rows.push({
            ts_ms: it.ts_ms,
            label: lab,
            source: 'cloud',
            _k: k
        });
    });
    rows.sort((a, b) => Number(b.ts_ms) - Number(a.ts_ms));
    return rows;
}
async function openDeviceLog(id, ev) {
    if (ev) {
        ev.preventDefault();
        ev.stopPropagation();
    }
    const p = pts.find(x => x.point_id === id);
    if (!p) return;
    window._deviceLogId = id;
    const title = document.getElementById('device-log-title');
    const meta = document.getElementById('device-log-meta');
    const hint = document.getElementById('device-log-cloud-hint');
    const modal = document.getElementById('device-log-modal');
    if (title) title.textContent = (pointDisplayName(p) || id) + ' · ' + t('ov.event_log');
    if (meta) meta.textContent = id;
    if (hint) hint.textContent = '';
    const list = document.getElementById('device-log-list');
    if (list) list.innerHTML = '<div class="skel"></div>';
    if (modal) modal.hidden = false;
    let localItems = [];
    try {
        const r = await fetch('/api/points/history?point_id=' + encodeURIComponent(id) + '&limit=48');
        if (r.ok) {
            const j = await r.json();
            localItems = j.items || [];
        }
    } catch (e) {}
    renderDeviceLogRows(mergeDeviceLog(localItems, [], p));
    try {
        const ad = (p && p.adapter) || '';
        if (!ad) {
            if (hint) hint.textContent = t('ov.no_adapter_local_only');
            return;
        }
        const r2 = await fetch('/api/modules/' + encodeURIComponent(ad) + '/rpc', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify({
                method: 'device_log',
                params: {
                    point_id: id,
                    limit: 20
                }
            })
        });
        if (r2.ok) {
            const j2 = await r2.json();
            const cloud = j2.items || [];
            renderDeviceLogRows(mergeDeviceLog(localItems, cloud, p));
            if (hint) {
                if (j2.cloud_error) hint.textContent = t('ov.cloud_prefix') + j2.cloud_error + (j2.message ? (' · ' + j2.message) : '');
                else if (!cloud.length) hint.textContent = t('ov.no_cloud_records');
                else hint.textContent = '';
            }
        } else if (hint) {
            hint.textContent = t('ov.cloud_log_failed');
        }
    } catch (e) {
        if (hint) hint.textContent = t('ov.cloud_log_unavailable');
    }
}

/*
 * Overview cards = devices, not raw /api/points rows.
 * Preferred: one point with props.params (sim.color_bulb).
 * Legacy: group protocol.endpoint.parameter into one card so brightness/hue
 * never become their own toggle cards and 能力点 counts devices.
 * Unscoped ids (not <protocol>.<device>, e.g. switch/bm_000a) stay as one
 * card. Overview files them on 全部 only — not under an adapter section.
 */
function overviewParamsBlobNonEmpty(paramsBlob) {
    if (!paramsBlob || typeof paramsBlob !== 'object') return false;
    if (Array.isArray(paramsBlob)) return paramsBlob.length > 0;
    return Object.keys(paramsBlob).length > 0;
}

function overviewCollapsePoints(points) {
    const byKey = Object.create(null);
    const order = [];
    if (!Array.isArray(points)) return [];

    function ensure(key, seed) {
        if (!byKey[key]) {
            byKey[key] = seed;
            order.push(key);
        }
        return byKey[key];
    }

    points.forEach(function (pt) {
        if (!pt || typeof pt.point_id !== 'string') return;
        if (pt.props && pt.props.visible === false) return;

        const pr = (pt.props && typeof pt.props === 'object') ? pt.props : null;
        const paramsBlob = pr && pr.params;
        const seg = pt.point_id.split('.');
        const hasParams = overviewParamsBlobNonEmpty(paramsBlob);
        const scoped = (typeof pointIdScoped === 'function')
            ? pointIdScoped(pt.point_id)
            : (seg.length >= 2 && !!seg[0] && seg.slice(1).some(function (s) { return !!s; }));

        /* Not <protocol>.<device>: keep the row. Do not parse a protocol from '/'. */
        if (!scoped) {
            if (!pt.point_id) return;
            const card = Object.assign({}, pt);
            card.props = Object.assign({}, pr || {});
            if (hasParams && !card.props.params) card.props.params = paramsBlob;
            ensure(pt.point_id, card);
            return;
        }

        if (hasParams || seg.length === 2) {
            const card = Object.assign({}, pt);
            card.props = Object.assign({}, pr || {});
            if (hasParams && !card.props.params) card.props.params = paramsBlob;
            ensure(pt.point_id, card);
            return;
        }

        if (seg.length >= 3) {
            const parameter = seg[seg.length - 1];
            const endpoint = seg[seg.length - 2];
            const protocol = seg.slice(0, -2).join('.');
            const key = protocol + '.' + endpoint;
            let card = byKey[key];
            if (!card) {
                card = {
                    point_id: pt.point_id,
                    type: pt.type || '',
                    adapter: pt.adapter || '',
                    online: !!pt.online,
                    value: pt.value,
                    props: {
                        name: endpoint,
                        params: Object.create(null)
                    },
                    _legacy_device: true
                };
                byKey[key] = card;
                order.push(key);
            }
            if (pt.online) card.online = true;
            if (pt.type && !card.type) card.type = pt.type;
            if (pt.adapter && !card.adapter) card.adapter = pt.adapter;
            const ent = {
                value: pt.value != null ? String(pt.value) : ''
            };
            if (pr && pr.access != null) ent.access = String(pr.access);
            if (pr && pr.visible === false) ent.visible = false;
            card.props.params[parameter] = ent;

            if (parameter === 'power') {
                card.point_id = pt.point_id;
                card.value = pt.value;
                card._has_power = true;
            }
            return;
        }

        ensure(pt.point_id, pt);
    });

    return order.map(function (k) {
        return byKey[k];
    });
}

function cardHtml(p) {
    if (p && p.props && p.props.visible === false) return '';
    const isCover = isCoverType(p.type);
    const pct = isCover ? parseInt(p.value, 10) : NaN;
    const cPhase = (p.props && p.props.commission_phase) || '';
    const commissioning = !!(p.props && (p.props.commissioning || cPhase === 'waiting' || cPhase === 'failed'));
    /* Power summary from props.params.power (or flat value). Never treat brightness "80" as on. */
    const powerVal = pointParamVal(p, 'power');
    const powerOn = powerVal === 'on' || powerVal === 'true' || powerVal === true ||
        p.value === 'on' || p.value === 'true' || p.value === 'open' || p.value === 'left_open' || p.value === 'online';
    const on = isCover ? (Number.isFinite(pct) && pct > 0) : (powerOn || (!(powerVal != null || p.value === 'on' || p.value === 'off' || p.value === 'true' || p.value === 'false') && (parseInt(p.value) || 0) > 0));
    const dis = (!p.online || commissioning) ? ' disabled' : '';
    const noCtrl = !!(p.props && p.props.no_control) || commissioning;
    const readOnly = !!(p.props && p.props.access === 'R');   /* read-only param: show value, no control */
    /* Overview card toggle = power only. Extra params open generic catalog detail / Devices. */
    const hasParams = !!(p.props && p.props.params && typeof p.props.params === 'object');
    const ctrlTypes = ['switch', 'light', 'light_strip', 'fan_light', 'fan', 'router', 'car', 'camera', 'television', 'air-conditioner', 'fridge', 'water_purifier', 'water_heater', 'appliance', 'washer', 'dishwasher', 'oven'];
    const allowToggle = !commissioning && ctrlTypes.includes(p.type) && !noCtrl && !readOnly &&
        (!hasParams || powerVal != null || p.value === 'on' || p.value === 'off' || p.value === 'true' || p.value === 'false' || !p.value);
    const ctrl = allowToggle ? '<label class="toggle"><input type="checkbox" data-id="' + p.point_id + '"' + (on ? ' checked' : '') + dis + '>    <span class="slider"></span></label>' : '';
    const pidJs = String(p.point_id || '').replace(/\\/g, '\\\\').replace(/'/g, "\\'");
    const cancelBtn = (p.adapter === 'matter' && commissioning && cPhase !== 'failed')
        ? '<button class="btn-sm" type="button" style="pointer-events:auto;position:relative;z-index:2" onclick="event.preventDefault();event.stopPropagation();cancelMatterCommission(\'' + String((p.props && p.props.node_id) || '') + '\')">' + t('ov.cancel_commission') + '</button>'
        : '';
    const delBtn = (p.adapter === 'matter') ? '<button class="btn-sm bad" type="button" style="pointer-events:auto;position:relative;z-index:2" onclick="event.preventDefault();event.stopPropagation();deleteMatterDevice(\'' + pidJs + '\')">' + t('ov.delete') + '</button>' : (p.adapter === 'zigbee') ? '<button class="btn-sm bad" type="button" style="pointer-events:auto;position:relative;z-index:2" onclick="event.preventDefault();event.stopPropagation();deleteZigbeeDevice(\'' + pidJs + '\')">' + t('ov.delete') + '</button>' : (p.adapter === 'ble_mesh') ? '<button class="btn-sm bad" type="button" style="pointer-events:auto;position:relative;z-index:2" onclick="event.preventDefault();event.stopPropagation();deleteBleMeshDevice(\'' + pidJs + '\')">' + t('ov.delete') + '</button>' : '';
    const short = pointDisplayName(p);
    const room = pointRoomName(p);
    const apDetail = ['fridge', 'water_purifier', 'water_heater', 'appliance'].includes(p.type);
    const plugDetail = p.type === 'switch' && switchHasDetail(p);
    /* Catalog detail is the default click, with or without props.params.
     * The modal lists only params the point actually reported. Specialized
     * panels override when they match and the point has no params blob. */
    let clickCls = '';
    let clickFn = '';
    if (hasParams) {
        clickCls = ' device-click';
        clickFn = 'openDeviceDetail';
    } else if (p.type === 'air-conditioner') {
        clickCls = ' ac-click';
        clickFn = 'openAcDetail';
    } else if (isCoverType(p.type)) {
        clickCls = ' cover-click';
        clickFn = 'openCoverDetail';
    } else if (apDetail) {
        clickCls = ' appliance-click';
        clickFn = 'openApplianceDetail';
    } else if (p.type === 'camera') {
        clickCls = ' camera-click';
        clickFn = 'openCameraDetail';
    } else if (p.type === 'speaker') {
        clickCls = ' speaker-click';
        clickFn = 'openSpeakerDetail';
    } else if (plugDetail) {
        clickCls = ' plug-click';
        clickFn = 'openPlugDetail';
    }
    if (!clickFn) {
        clickCls = ' device-click';
        clickFn = 'openDeviceDetail';
    }
    const clickAttr = (clickFn && !commissioning) ? (' onclick="' + clickFn + '(\'' + p.point_id + '\',event)"') : '';
    const valShow = (function() {
        if (cPhase === 'waiting' || (commissioning && cPhase !== 'failed')) return matterCommissionLabel(p, cPhase) || t('ov.waiting_commission');
        if (cPhase === 'failed') return t('ov.commission_failed');
        if (p.type === 'button') return (p.online ? t('ov.ready') : t('ov.offline'));
        if (isCover && Number.isFinite(pct)) return pct + '%';
        if (p.value === 'detected') {
            const summary = p.props && p.props.card_summary;
            if (typeof summary === 'string' && summary.indexOf(t('ov.detected') + ' ') === 0) return summary;
            return t('ov.detected');
        }
        if (p.value === 'clear') return t('ov.no_motion');
        if (p.value === 'closed') return t('ov.closed');
        if (p.value === 'open' && p.type === 'binary_sensor') return t('ov.open');
        if (p.value === 'online' || p.value === 'offline') return p.value === 'online' ? t('ov.online') : t('ov.offline');
        if (noCtrl && (p.value === 'on' || p.value === 'off' || powerVal === 'on' || powerVal === 'off')) return on ? t('ov.breaker_on') : t('ov.breaker_off');
        if (p.value === 'on' || p.value === 'off' || powerVal === 'on' || powerVal === 'off') return on ? t('ov.on') : t('ov.off');
        if (hasParams && (powerVal != null || p.value === 'on' || p.value === 'off')) return on ? t('ov.on') : t('ov.off');
        if (p.value === 'unknown' || p.value === 'idle') return p.online ? '—' : t('ov.offline');
        return p.value;
    })();
    /* Catalog type avatar, or a plugin props.icon override; dim with power when known. */
    const dimByPower = !commissioning && (powerVal != null || p.value === 'on' || p.value === 'off' ||
        p.value === 'true' || p.value === 'false' || allowToggle);
    const imageUrl = ((typeof dwIconUrl === 'function') &&
        p.props && p.props.icon && dwIconUrl(p.props.icon)) ||
        ((typeof dwTypeImageUrl === 'function') ? dwTypeImageUrl(p.type) : '');
    let iconHtml = '';
    if (imageUrl && typeof dwDeviceIconHtml === 'function') {
        iconHtml = dwDeviceIconHtml(imageUrl, dimByPower ? !!on : null);
    } else if (TYPE_ICON[p.type]) {
        iconHtml = '<span class="dw-device-icon-fallback">' + TYPE_ICON[p.type] + '</span>';
    }
    const proto = (typeof pointProtocol === 'function') ? pointProtocol(p) : '';
    let onlineLab;
    if (cPhase === 'waiting') onlineLab = matterCommissionLabel(p, cPhase) + (proto ? (' · ' + esc(proto)) : '');
    else if (cPhase === 'failed') onlineLab = t('ov.commission_failed') + (proto ? (' · ' + esc(proto)) : '');
    else onlineLab = (p.online ? t('ov.online') : t('ov.offline')) + (proto ? (' · ' + esc(proto)) : '');
    const valCls = cPhase === 'waiting' ? 'pending' : (cPhase === 'failed' ? 'off' : (on ? 'on' : 'off'));
    const cardClickCls = commissioning ? '' : clickCls;
    const renameBtn = '<button class="btn-sm" type="button" onclick="renameDevice(\'' + pidJs + '\',event)">' + t('ov.rename') + '</button>';
    return '<div class="dcard' + cardClickCls + ((p.online || commissioning) ? '' : ' offline') + (commissioning ? ' commissioning' : '') + '" data-pid="' + escAttr(p.point_id || '') + '"' + clickAttr + '><div class="card-top">' + iconHtml + '<div class="card-name">' + esc(short) + '</div><span class="badge ' + p.type + '">' + esc((typeof dwTypeDisplay === 'function') ? dwTypeDisplay(p) : mhTypeLabel(p.type)) + '</span></div>' + '<div class="card-meta"><span>' + onlineLab + '</span>' + (room ? '<span>' + esc(room) + '</span>' : '') + '</div>' + '<div class="card-val ' + valCls + '">' + esc(String(valShow)) + '</div>' + climateSummary(p) + coverSummary(p) + magnetSummary(p) + meterSummary(p) + propsHtml(p) + '<div class="card-actions">' + renameBtn + (showDeviceLogBtn(p) ? '<button class="btn-sm" type="button" onclick="openDeviceLog(\'' + p.point_id + '\',event)">' + esc(t('ov.log')) + '</button>' : '') + cancelBtn + delBtn + ctrl + '</div></div>';
}

async function renameDevice(pointId, ev) {
    if (ev && ev.stopPropagation) ev.stopPropagation();
    const p = (typeof pts !== 'undefined' && pts) ? pts.find(function (x) { return x && x.point_id === pointId; }) : null;
    const cur = (p && typeof pointDisplayName === 'function') ? pointDisplayName(p) : pointId;
    const entered = window.prompt(t('ov.device_name'), cur || '');
    if (entered == null) return;
    const name = String(entered).trim();
    if (!name) {
        toast(t('ov.name_empty'));
        return;
    }
    const nameBytes = (typeof TextEncoder === 'function') ? new TextEncoder().encode(name).length : name.length;
    if (nameBytes > 48) {
        toast(t('ov.name_too_long'));
        return;
    }
    try {
        const r = await fetch('/api/points/rename', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify({
                point_id: pointId,
                name: name
            })
        });
        const j = await r.json().catch(() => ({}));
        if (r.ok && j.result === 'ok') {
            if (p) {
                p.props = p.props || {};
                p.props.name = name;
            }
            if (typeof renderOverview === 'function') renderOverview();
            toast(t('ov.renamed'));
            if (typeof loadPoints === 'function') loadPoints();
        } else {
            toast(t('ov.rename_failed') + (j.error ? ': ' + j.error : ''));
        }
    } catch (e) {
        toast(t('ov.network_error'));
    }
}

/* A store action (install/enable/disable/upgrade/uninstall/reinstall) asks the
 * device to reload the hub, but that reload runs on its own task and the HTTP
 * call returns first. The immediate loadPoints() below therefore still sees the
 * OLD point set, and removals publish no point.updated of their own, so without
 * this the console kept showing adapters the reload had just unregistered.
 * Normally the device's hub.reloaded event (over /ws/events) does the job; this
 * is the fallback for when the WebSocket is down. */
function refreshAfterHubReload() {
    const pass = function () {
        loadStoreData();
        loadAdapters();
        loadPoints();
    };
    /* Two passes: the reload unloads and re-registers every adapter, so its
     * duration varies, and a module that is already gone from disk but still
     * registered in RAM keeps appearing in /api/modules until a fetch lands
     * after the reload finished. */
    setTimeout(pass, 1200);
    setTimeout(pass, 3200);
    setTimeout(pass, 6000);
}

async function storeAct(kind, id, enabled) {
    const url = kind === 'module' ? '/api/modules' : '/api/packs';
    toast(enabled ? t('ov.enabling') : t('ov.disabling'));
    try {
        const r = await fetch(url, {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify({
                id,
                enabled,
                reload: true
            })
        });
        toast(r.ok ? (enabled ? t('ov.enabled_toast') : t('ov.disabled')) : t('ov.failed'));
        await loadStoreData();
        await loadAdapters();
        await loadPoints();
        refreshAfterHubReload();
    } catch (e) {
        toast(t('ov.network_error'));
    }
}
async function storeInstall(kind, id) {
    toast(t('ov.downloading_install', { id: id }));
    try {
        const r = await fetch('/api/store/install', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify({
                type: kind,
                id,
                reload: true
            })
        });
        toast(r.ok ? t('ov.install_ok') : (r.status === 409 ? t('ov.already_installed') : t('ov.install_failed')));
        if (r.ok && window.uninstalledIds) window.uninstalledIds.delete(id);
        await loadStoreData();
        await loadAdapters();
        await loadPoints();
        refreshAfterHubReload();
    } catch (e) {
        toast(t('ov.network_error'));
    }
}
async function storeUpgrade(kind, id) {
    toast(t('ov.upgrading', { id: id }));
    try {
        const r = await fetch('/api/store/upgrade', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify({
                type: kind,
                id,
                reload: true
            })
        });
        toast(r.ok ? t('ov.upgrade_ok') : (r.status === 409 ? t('ov.up_to_date') : t('ov.upgrade_failed')));
        await loadStoreData();
        await loadAdapters();
        await loadPoints();
        refreshAfterHubReload();
    } catch (e) {
        toast(t('ov.network_error'));
    }
}
async function storeUninstall(kind, id) {
    if (!confirm(t('ov.uninstall_confirm', { id: id }))) return;
    toast(t('ov.uninstalling'));
    try {
        const r = await fetch('/api/store/uninstall', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify({
                type: kind,
                id,
                reload: true
            })
        });
        toast(r.ok ? t('ov.uninstalled') : t('ov.failed'));
        if (r.ok) {
            /* Optimistically hide it from the Installed list right away: the
             * loader keeps the slot in RAM until the hub reload lands, so without
             * this the row lingered and the page looked like it never refreshed.
             * store.js keeps it dropped through the reload passes; a manual
             * catalog refresh (or reinstall) clears the mask and re-shows it. */
            if (!window.uninstalledIds) window.uninstalledIds = new Set();
            window.uninstalledIds.add(id);
        }
        await loadStoreData();
        await loadAdapters();
        await loadPoints();
        refreshAfterHubReload();
    } catch (e) {
        toast(t('ov.network_error'));
    }
}
async function storeReinstall(kind, id) {
    if (!confirm(t('ov.reinstall_confirm', { id: id }))) return;
    toast(t('ov.reinstalling', { id: id }));
    try {
        const r = await fetch('/api/store/reinstall', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify({
                type: kind,
                id,
                reload: true
            })
        });
        if (r.status === 404) {
            toast(t('ov.module_not_installed'));
        } else if (r.ok) {
            toast(t('ov.reinstalled'));
            if (window.uninstalledIds) window.uninstalledIds.delete(id);
        } else {
            toast(t('ov.reinstall_failed'));
        }
        await loadStoreData();
        await loadAdapters();
        await loadPoints();
        refreshAfterHubReload();
    } catch (e) {
        toast(t('ov.network_error'));
    }
}
async function storeDel(kind, id) {
    if (kind === 'module') {
        return storeUninstall('module', id);
    }
    if (!confirm(t('ov.delete_confirm', { id: id }))) return;
    toast(t('ov.deleting'));
    try {
        const r = await fetch('/api/packs', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify({
                id,
                delete: true,
                reload: true
            })
        });
        toast(r.ok ? t('ov.deleted') : t('ov.failed'));
        await loadStoreData();
        await loadAdapters();
        await loadPoints();
        refreshAfterHubReload();
    } catch (e) {
        toast(t('ov.network_error'));
    }
}
let activeAuthModule = '';

function authModById(id) {
    return modules.find(x => x.id === id) || authModuleFor(id);
}
function saveAuthDraft(id, mode) {
    if (!id) return;
    const k = authDraftKey(id, mode);
    authDrafts[k] = {
        user: (document.getElementById('auth-user').value || ''),
        pass: (document.getElementById('auth-pass').value || ''),
        host: (document.getElementById('auth-host') && document.getElementById('auth-host').value) || '',
        keyId: (document.getElementById('auth-keyid') && document.getElementById('auth-keyid').value) || '',
        apiKey: (document.getElementById('auth-apikey') && document.getElementById('auth-apikey').value) || ''
    };
}

function defaultAuthDraft(mod, mode) {
    const d = {
        user: '',
        pass: '',
        host: '',
        keyId: '',
        apiKey: ''
    };
    if (mode === 'session' || !mod) return d;
    if (isHostLogin(mod)) {
        d.host = lbl(mod, 'host_placeholder', '192.168.3.1');
        d.user = lbl(mod, 'username_default', 'admin');
    }
    return d;
}

function loadAuthDraft(mod, mode) {
    const k = authDraftKey(mod && mod.id, mode);
    const d = Object.assign(defaultAuthDraft(mod, mode), authDrafts[k] || {});
    document.getElementById('auth-user').value = d.user || '';
    document.getElementById('auth-pass').value = d.pass || '';
    const hi = document.getElementById('auth-host');
    if (hi) hi.value = d.host || '';
    const kid = document.getElementById('auth-keyid');
    if (kid) kid.value = d.keyId || '';
    const ak = document.getElementById('auth-apikey');
    if (ak) ak.value = d.apiKey || '';
}

function applyAuthLoginLabels(mod, mode) {
    const sessionMode = mode === 'session';
    document.getElementById('auth-login-title').textContent = sessionMode ? lbl(mod, 'session', t('ov.import_session')) : lbl(mod, 'login_title', lbl(mod, 'login', t('ov.login')));
    document.getElementById('auth-user-label').textContent = lbl(mod, 'username', t('ov.username'));
    document.getElementById('auth-pass-label').textContent = lbl(mod, 'password', t('ov.password'));
    const hostBlk = document.getElementById('auth-host-block');
    if (hostBlk) {
        const showHost = !sessionMode && isHostLogin(mod);
        hostBlk.hidden = !showHost;
        document.getElementById('auth-host-label').textContent = lbl(mod, 'host', t('ov.host'));
        const hi = document.getElementById('auth-host');
        if (hi) {
            hi.placeholder = lbl(mod, 'host_placeholder', '192.168.3.1');
        }
    }
    document.getElementById('auth-keyid-label').textContent = lbl(mod, 'key_id', 'API Key ID');
    document.getElementById('auth-apikey-label').textContent = lbl(mod, 'api_key', 'API Key');
    document.getElementById('auth-ticket-label').textContent = lbl(mod, 'ticket', t('ov.ticket_code'));
    document.getElementById('auth-ticket').placeholder = lbl(mod, 'ticket_placeholder', t('ov.ticket_code'));
    document.getElementById('auth-captcha-label').textContent = lbl(mod, 'captcha', t('ov.captcha'));
    document.getElementById('auth-cancel').textContent = lbl(mod, 'cancel', t('ov.cancel'));
    setAuthPrimaryAction(sessionMode ? 'session' : 'login');
    document.getElementById('auth-user').hidden = sessionMode;
    document.getElementById('auth-user-label').hidden = sessionMode;
    document.getElementById('auth-pass').hidden = sessionMode;
    document.getElementById('auth-pass-field').hidden = sessionMode;
    document.getElementById('auth-pass-label').hidden = sessionMode;
    document.getElementById('auth-apikey-block').hidden = sessionMode || !isApiKeyLogin(mod);
    document.getElementById('auth-session-block').hidden = !sessionMode;
    document.getElementById('auth-2fa-block').hidden = true;
    const ticketBlk = document.getElementById('auth-ticket-block');
    if (ticketBlk) ticketBlk.hidden = true;
    document.getElementById('auth-ticket').value = '';
    document.getElementById('auth-login-hint').textContent = sessionMode ? lbl(mod, 'session_hint', t('ov.session_hint')) : '';
    loadAuthDraft(mod, mode);
    const uEl = document.getElementById('auth-user');
    if (uEl) uEl.placeholder = '';
}

/** Primary footer button: login | send_code (captcha stage) | verify (SMS stage) | session. */
function setAuthPrimaryAction(mode) {
    const btn = document.getElementById('auth-submit');
    const ticketBlk = document.getElementById('auth-ticket-block');
    const mod = authModById(activeAuthModule);
    if (!btn) return;
    if (mode === 'send_code') {
        btn.textContent = lbl(mod, 'send_code', t('ov.send_code'));
        btn.onclick = function () { sendAuthVerify(); };
        if (ticketBlk) ticketBlk.hidden = true;
    } else if (mode === 'verify') {
        btn.textContent = lbl(mod, 'login', t('ov.login'));
        btn.onclick = function () { submitAuthLogin(); };
        if (ticketBlk) ticketBlk.hidden = false;
    } else if (mode === 'session') {
        btn.textContent = lbl(mod, 'session', t('ov.import_session'));
        btn.onclick = function () { submitAuthLogin(); };
        if (ticketBlk) ticketBlk.hidden = true;
    } else {
        btn.textContent = lbl(mod, 'login', t('ov.login'));
        btn.onclick = function () { submitAuthLogin(); };
        if (ticketBlk) ticketBlk.hidden = true;
    }
}

function openModuleLogin(id) {
    const mod = authModById(id);
    if (!mod || !hasCredLogin(mod)) {
        toast(t('ov.no_login_form'));
        return;
    }
    if (activeAuthModule && activeAuthModule !== mod.id) {
        saveAuthDraft(activeAuthModule, 'login');
    }
    activeAuthModule = mod.id;
    applyAuthLoginLabels(mod, 'login');
    openAuthLogin();
}

function openModuleSession(id) {
    const mod = authModById(id);
    if (!mod || !hasModAction(mod, 'session')) {
        toast(t('ov.no_session_import'));
        return;
    }
    if (activeAuthModule) {
        saveAuthDraft(activeAuthModule, 'login');
    }
    activeAuthModule = mod.id;
    applyAuthLoginLabels(mod, 'session');
    openAuthLogin();
}

function openAuthLogin() {
    if (document.getElementById('auth-session-block').hidden) {
        document.getElementById('auth-login-hint').textContent = '';
    }
    document.getElementById('auth-2fa-block').hidden = true;
    document.getElementById('auth-captcha-block').hidden = true;
    const ticketBlk = document.getElementById('auth-ticket-block');
    if (ticketBlk) ticketBlk.hidden = true;
    document.getElementById('auth-ticket').value = '';
    document.getElementById('auth-icode').value = '';
    document.getElementById('auth-login-modal').hidden = false;
    const focusEl = document.getElementById('auth-session-block').hidden ? document.getElementById('auth-user') : document.getElementById('auth-access');
    focusEl.focus();
}

function closeAuthLogin() {
    if (activeAuthModule) {
        saveAuthDraft(activeAuthModule, document.getElementById('auth-session-block').hidden ? 'login' : 'session');
    }
    document.getElementById('auth-login-modal').hidden = true;
}

function showAuthCaptcha(b64, mime) {
    const blk = document.getElementById('auth-captcha-block');
    const img = document.getElementById('auth-captcha-img');
    blk.hidden = false;
    if (b64) {
        img.src = 'data:' + (mime || 'image/jpeg') + ';base64,' + b64;
        img.hidden = false;
    } else {
        img.removeAttribute('src');
        img.hidden = true;
    }
}

async function refreshAuthCaptcha() {
    if (!activeAuthModule) {
        return;
    }
    try {
        /* Re-request the 2FA challenge; the plugin returns a fresh captcha when
         * one is required, so the user can change it instead of being stuck with
         * the first image (issue #11). */
        const r = await moduleRpc(activeAuthModule, 'verify_send', {});
        const j = r.json;
        if (j && j.captcha_b64) {
            showAuthCaptcha(j.captcha_b64, j.captcha_mime);
            const hint = document.getElementById('auth-login-hint');
            if (hint && j.hint) hint.textContent = j.hint;
        } else if (r.ok && j && j.result === 'ok') {
            const hint = document.getElementById('auth-login-hint');
            if (hint && j.message) hint.textContent = j.message;
        } else {
            toast(t('ov.network_error'));
        }
    } catch (e) {
        toast(t('ov.network_error'));
    }
}

function revealAuthChallenge(opts) {
    opts = opts || {};
    const two = document.getElementById('auth-2fa-block');
    two.hidden = false;
    const needCaptcha = !!(opts.captcha_b64 || opts.showCaptcha);
    if (opts.captcha_b64) {
        showAuthCaptcha(opts.captcha_b64, opts.captcha_mime);
    } else if (opts.showCaptcha) {
        document.getElementById('auth-captcha-block').hidden = false;
    }
    if (opts.hideCaptcha) {
        document.getElementById('auth-captcha-block').hidden = true;
    }
    /* Captcha stage: only 图形码 + primary「发送验证码」. SMS field appears after send. */
    if (needCaptcha && !opts.hideCaptcha) {
        setAuthPrimaryAction('send_code');
    } else if (opts.hideCaptcha || opts.focus === 'auth-ticket') {
        setAuthPrimaryAction('verify');
    }
    const hint = document.getElementById('auth-login-hint');
    if (opts.hint != null) hint.textContent = opts.hint;
    try {
        two.scrollIntoView({
            behavior: 'smooth',
            block: 'nearest'
        });
    } catch (e) {
        two.scrollIntoView();
    }
    const focusId = opts.focus || (needCaptcha && !opts.hideCaptcha ? 'auth-icode' : 'auth-ticket');
    const el = document.getElementById(focusId);
    if (el) try {
        el.focus();
    } catch (e) {}
}
async function sendAuthVerify() {
    toast(t('ov.sending_code'));
    const hint = document.getElementById('auth-login-hint');
    const icodeRaw = document.getElementById('auth-icode').value.trim().replace(/[^0-9A-Za-z]/g, '');
    document.getElementById('auth-icode').value = icodeRaw;
    if (icodeRaw) {
        hint.textContent = t('ov.submitting_icode', { v: icodeRaw });
    }
    try {
        const r = await moduleRpc(activeAuthModule, 'verify_send', icodeRaw ? {
            icode: icodeRaw
        } : {});
        const j = r.json;
        if (j && j.icode_used) {
            hint.textContent = t('ov.hub_received_icode', { v: j.icode_used }) + (j.result === 'ok' ? t('ov.sms_sent_suffix') : '');
        }
        if (j && j.result === 'captcha' && j.captcha_b64) {
            revealAuthChallenge({
                captcha_b64: j.captcha_b64,
                captcha_mime: j.captcha_mime,
                hint: (j.icode_used ? t('ov.icode_wrong', { v: j.icode_used }) : '') + (j.message || j.hint || t('ov.icode_retry')),
                focus: 'auth-icode'
            });
            document.getElementById('auth-icode').value = '';
            toast(t('ov.captcha_required'));
            return;
        }
        if (j && j.captcha_b64) {
            revealAuthChallenge({
                captcha_b64: j.captcha_b64,
                captcha_mime: j.captcha_mime,
                hint: j.hint || j.message || t('ov.captcha_enter_retry'),
                focus: 'auth-icode'
            });
            document.getElementById('auth-icode').value = '';
            toast(t('ov.captcha_required'));
            return;
        }
        toast(r.ok && j && j.result === 'ok' ? t('ov.code_sent') : t('ov.send_failed'));
        if (r.ok && j && j.result === 'ok') {
            document.getElementById('auth-captcha-block').hidden = true;
            revealAuthChallenge({
                hideCaptcha: true,
                hint: j.hint || t('ov.sms_sent_fill'),
                focus: 'auth-ticket'
            });
            setAuthPrimaryAction('verify');
        }
        if (!r.ok || !j || j.result !== 'ok') {
            if (!j || !j.icode_used) hint.textContent = (j && j.message) || (j && j.error) || t('ov.send_failed');
        }
    } catch (e) {
        toast(t('ov.network_error'));
    }
}
async function submitAuthLogin() {
    const user = document.getElementById('auth-user').value.trim();
    const pass = document.getElementById('auth-pass').value;
    const ticket = document.getElementById('auth-ticket').value.trim();
    const hint = document.getElementById('auth-login-hint');
    const ticketBlk = document.getElementById('auth-ticket-block');
    const ticketVisible = ticketBlk ? !ticketBlk.hidden : !document.getElementById('auth-2fa-block').hidden;
    if (ticket && ticketVisible) {
        toast(t('ov.verifying'));
        try {
            const r = await moduleRpc(activeAuthModule, 'verify', {
                ticket
            });
            const j = r.json;
            if (r.ok && j && j.result === 'ok') {
                closeAuthLogin();
                loadMeta();
                let m = t('ov.login_ok');
                if (j.sync === 'pending') {
                    pollAuthSyncWhileBusy(true);
                    toast(m + ' · ' + t('ov.syncing_devices'));
                    const deadline = Date.now() + 120000;
                    while (Date.now() < deadline) {
                        await new Promise(res => setTimeout(res, 1500));
                        const st = await moduleRpc(activeAuthModule, 'status');
                        const sj = st.json || {};
                        if (!sj.syncing) {
                            await loadMeta();
                            await loadPoints();
                            if (view === 'overview') renderOverview();
                            if (sj.sync === 'need_home') {
                                toast(m + ' · ' + t('ov.choose_home_first'));
                                openHomePicker(activeAuthModule);
                            } else if (sj.sync === 'ok') {
                                let sm = m + ' · ' + t('ov.synced_devices', { n: (sj.synced || 0) });
                                if (sj.lan_tokens > 0) sm += ' · ' + t('ov.lan_devices', { n: sj.lan_tokens });
                                toast(sm);
                            }
                            return;
                        }
                    }
                    await loadMeta();
                    await loadPoints();
                    if (view === 'overview') renderOverview();
                    toast(m + ' · ' + t('ov.sync_still_running'));
                    return;
                }
                await loadPoints();
                if (j.sync === 'need_home') {
                    toast(m + ' · ' + t('ov.choose_home_first'));
                    openHomePicker(activeAuthModule);
                    return;
                }
                if (j.sync === 'ok' && j.synced != null) {
                    m += ' · ' + t('ov.synced_devices', { n: j.synced });
                    if (j.lan_tokens > 0) m += ' · ' + t('ov.lan_devices', { n: j.lan_tokens });
                }
                toast(m);
                return;
            }
            hint.textContent = j.message || j.error || t('ov.verify_failed');
            toast(t('ov.verify_failed'));
            return;
        } catch (e) {
            toast(t('ov.network_error'));
            return;
        }
    }
    const sessionMode = !document.getElementById('auth-session-block').hidden;
    if (sessionMode) {
        const access = document.getElementById('auth-access').value.trim();
        const refresh = document.getElementById('auth-refresh').value.trim();
        const phoneId = document.getElementById('auth-phoneid').value.trim();
        if (!access || !refresh) {
            hint.textContent = t('ov.tokens_required');
            return;
        }
        toast(t('ov.importing_session'));
        try {
            const body = {
                access_token: access,
                refresh_token: refresh
            };
            if (phoneId) body.phone_id = phoneId;
            const r = await moduleRpc(activeAuthModule, 'session', body);
            const j = r.json;
            if (r.ok && j && j.result === 'ok') {
                closeAuthLogin();
                await loadMeta();
                pollAuthSyncWhileBusy(true);
                await loadPoints();
                let m = t('ov.session_imported');
                if (j.sync === 'need_home') {
                    toast(m + ' · ' + t('ov.choose_home_first'));
                    openHomePicker(activeAuthModule);
                    return;
                }
                if (j.sync === 'ok' && j.synced != null) m += ' · ' + t('ov.synced_devices', { n: j.synced });
                toast(m);
                return;
            }
            hint.textContent = (j && j.message) || (j && j.error) || t('ov.import_failed');
            toast(t('ov.import_failed'));
        } catch (e) {
            toast(t('ov.network_error'));
        }
        return;
    }
    if (!user || !pass) {
        hint.textContent = t('ov.credentials_required');
        return;
    }
    const keyId = document.getElementById('auth-keyid').value.trim();
    const apiKey = document.getElementById('auth-apikey').value.trim();
    const mod = authModById(activeAuthModule);
    if (isApiKeyLogin(mod) && (!keyId || !apiKey)) {
        hint.textContent = t('ov.apikey_required');
        return;
    }
    toast(t('ov.signing_in'));
    try {
        const body = {
            username: user,
            password: pass,
            email: user,
            account: user,
            auto_send: false
        };
        if (keyId) body.key_id = keyId;
        if (apiKey) body.api_key = apiKey;
        const hostEl = document.getElementById('auth-host');
        const host = (hostEl && !document.getElementById('auth-host-block').hidden) ? hostEl.value.trim() : '';
        if (host) body.host = host;
        if (isHostLogin(authModById(activeAuthModule))) {
            body.port = 80;
        }
        const r = await moduleRpc(activeAuthModule, 'login', body);
        const j = r.json;
        if (r.ok && j && j.result === 'ok') {
            closeAuthLogin();
            await loadMeta();
            pollAuthSyncWhileBusy(true);
            await loadPoints();
            let m = t('ov.login_ok');
            if (j.sync === 'need_home') {
                toast(m + ' · ' + t('ov.choose_home_first'));
                openHomePicker(activeAuthModule);
                return;
            }
            if (j.sync === 'ok' && j.synced != null) {
                m += ' · ' + t('ov.synced_devices', { n: j.synced });
                if (j.lan_tokens > 0) m += ' · ' + t('ov.lan_devices', { n: j.lan_tokens });
            }
            toast(m);
            return;
        }
        if (j && j.result === 'captcha') {
            revealAuthChallenge({
                captcha_b64: j.captcha_b64,
                captcha_mime: j.captcha_mime,
                showCaptcha: !j.captcha_b64,
                hint: j.hint || j.message || t('ov.captcha_then_send'),
                focus: 'auth-icode'
            });
            toast(t('ov.captcha_required'));
            return;
        }
        if (j && (j.result === 'verify' || j.error === 'ESP_ERR_INVALID_STATE')) {
            if (j.code_sent) {
                revealAuthChallenge({
                    hideCaptcha: true,
                    hint: j.hint || t('ov.sms_sent_fill'),
                    focus: 'auth-ticket'
                });
                toast(t('ov.code_sent'));
                return;
            }
            revealAuthChallenge({
                hideCaptcha: true,
                hint: t('ov.sending_code_busy'),
                focus: 'auth-ticket'
            });
            toast(t('ov.two_factor_required'));
            await sendAuthVerify();
            return;
        }
        hint.textContent = (j && j.message) || (j && j.error) || t('ov.login_failed');
        toast(t('ov.login_failed'));
    } catch (e) {
        toast(t('ov.network_error'));
    }
}
async function logoutModule(id) {
    if (!confirm(t('ov.signout_confirm'))) return;
    toast(t('ov.signing_out'));
    try {
        const r = await moduleRpc(id || activeAuthModule, 'logout');
        const j = r.json;
        if (r.ok && j && j.result === 'ok') {
            await loadMeta();
            await loadPoints();
            toast(t('ov.signed_out'));
        } else {
            toast(t('ov.signout_failed'));
        }
    } catch (e) {
        toast(t('ov.network_error'));
    }
}
async function syncModule(id, homeId) {
    const mid = id || activeAuthModule;
    if (authModuleSyncing(mid)) {
        toast(t('ov.sync_busy'));
        return;
    }
    markAuthSyncing(mid, true);
    if (view === 'overview') renderOverview();
    pollAuthSyncWhileBusy(true);
    toast(homeId ? t('ov.switching_home') : t('ov.syncing_now'));
    try {
        const params = {
            merge: true,
            reload: true
        };
        if (homeId) params.home_id = String(homeId);
        const r = await moduleRpc(mid, 'sync', params);
        const j = r.json;
        if (r.ok && j && j.result === 'need_home') {
            toast(t('ov.choose_home_first'));
            openHomePicker(mid);
            return;
        }
        if (r.ok && j && (j.result === 'pending' || j.syncing)) {
            toast(j.message || (homeId ? t('ov.switching_home_now') : t('ov.syncing_devices')));
            const deadline = Date.now() + 120000;
            let last = j;
            while (Date.now() < deadline) {
                await new Promise(res => setTimeout(res, 1500));
                const st = await moduleRpc(mid, 'status');
                const sj = st.json || {};
                last = sj;
                if (!sj.syncing) break;
            }
            await loadMeta();
            await loadPoints();
            await loadAdapters();
            if (view === 'overview') renderOverview();
            if (last && last.sync === 'need_home') {
                toast(t('ov.choose_home_first'));
                openHomePicker(mid);
                return;
            }
            if (last && last.sync === 'ok') {
                let m = last.sync_home_name
                    ? t('ov.switched_home_devices', { name: last.sync_home_name, n: (last.synced || 0) })
                    : t('ov.synced_devices', { n: (last.synced || 0) });
                if (last.lan_tokens > 0) m += ' · ' + t('ov.lan_devices', { n: last.lan_tokens });
                toast(m);
                return;
            }
            if (last && last.syncing) {
                toast(t('ov.sync_still_running_hint'));
                return;
            }
            toast((last && last.sync_error) || (last && last.message) || t('ov.sync_failed'));
            return;
        }
        if (r.ok && j && j.result === 'ok') {
            let m = j.home_name
                ? t('ov.switched_home_devices', { name: j.home_name, n: (j.synced || 0) })
                : t('ov.synced_devices', { n: (j.synced || 0) });
            if (j.lan_tokens > 0) m += ' · ' + t('ov.lan_devices', { n: j.lan_tokens });
            toast(m);
        } else toast(j && j.message || j && j.error || t('ov.sync_failed'));
        await loadMeta();
        await loadPoints();
        await loadAdapters();
        if (view === 'overview') renderOverview();
    } catch (e) {
        toast(t('ov.failed'));
    } finally {
        markAuthSyncing(mid, false);
        if (view === 'overview') renderOverview();
    }
}

function closeHomePicker() {
    const m = document.getElementById('home-picker-modal');
    if (m) m.hidden = true;
}
async function openHomePicker(id) {
    if (!id) {
        toast(t('ov.module_unspecified'));
        return;
    }
    homePickerModule = id;
    homePickerSelected = '';
    const mod = authModById(homePickerModule) || modules.find(x => x.id === homePickerModule);
    const title = document.getElementById('home-picker-title');
    const hint = document.getElementById('home-picker-hint');
    if (title) title.textContent = lbl(mod, 'home_picker_title', t('ov.choose_home'));
    if (hint) hint.textContent = lbl(mod, 'home_picker_hint', t('ov.home_picker_hint'));
    const conf = document.getElementById('home-picker-confirm');
    if (conf) {
        conf.textContent = lbl(mod, 'home_picker_confirm', t('ov.sync_this_home'));
        conf.disabled = true;
    }
    const listEl = document.getElementById('home-picker-list');
    if (listEl) listEl.innerHTML = '<div class="modal-hint">' + t('ov.loading') + '</div>';
    document.getElementById('home-picker-modal').hidden = false;
    toast(t('ov.loading_homes'));
    try {
        const r = await moduleRpc(homePickerModule, 'homes');
        const j = r.json;
        const homes = (j && j.homes) || [];
        if (!r.ok || !j || j.result !== 'ok') {
            const msg = esc((j && j.message) || (j && j.error) || t('ov.load_failed'));
            /* need_login is the contract; the text match is only a fallback for a
             * module that predates it, and must stay language-agnostic because the
             * message arrives in whatever language that module speaks. */
            const needLogin = !!(j && j.need_login) ||
                /过期|未登录|expired|unauthori[sz]ed|not logged|login required|session/i
                    .test((j && j.message) || '');
            if (listEl) listEl.innerHTML = '<div class="modal-hint">' + msg +
                (needLogin ? '<div style="margin-top:12px"><button class="btn-sm on" type="button" id="home-picker-relogin">' + t('ov.relogin') + '</button></div>' : '') +
                '</div>';
            if (needLogin) {
                const btn = document.getElementById('home-picker-relogin');
                if (btn) btn.onclick = () => {
                    closeHomePicker();
                    openModuleLogin(homePickerModule);
                };
            }
            toast(needLogin ? t('ov.session_expired') : t('ov.load_homes_failed'));
            return;
        }
        if (!homes.length) {
            if (listEl) listEl.innerHTML = '<div class="modal-hint">' + t('ov.no_homes') + '</div>';
            return;
        }
        const curHome = String((authStatus(homePickerModule).home_id) || '');
        homePickerSelected = curHome;
        if (conf) conf.disabled = !homePickerSelected;
        if (listEl) {
            listEl.innerHTML = homes.map(h => {
                const id = String(h.id);
                const on = id === curHome;
                return '<div class="home-item' + (on ? ' on' : '') + '" data-home-id="' + escAttr(id) + '" role="button" tabindex="0">' +
                    '<div style="flex:1"><div style="font-weight:600">' + esc(h.name || id) + (on ? ' · ' + t('ov.current') : '') + '</div>' +
                    '<div class="meta">' + t('ov.rooms_devices', { rooms: (h.room_count || 0), devices: (h.device_count || 0) }) + '</div></div>' +
                    '<button type="button" class="btn-sm' + (on ? '' : ' on') + '" data-home-switch="' + escAttr(id) + '">' +
                    (on ? t('ov.resync') : t('ov.switch_to_home')) + '</button></div>';
            }).join('');
            listEl.onclick = function(ev) {
                const sw = ev.target.closest('[data-home-switch]');
                if (sw) {
                    ev.preventDefault();
                    ev.stopPropagation();
                    const hid = sw.getAttribute('data-home-switch');
                    homePickerSelected = hid;
                    closeHomePicker();
                    syncModule(homePickerModule, hid);
                    return;
                }
                const row = ev.target.closest('.home-item[data-home-id]');
                if (!row) return;
                const hid = row.getAttribute('data-home-id');
                homePickerSelected = hid;
                listEl.querySelectorAll('.home-item').forEach(x => x.classList.toggle('on', x === row));
                if (conf) conf.disabled = !hid;
            };
        }
    } catch (e) {
        if (listEl) listEl.innerHTML = '<div class="modal-hint">' + t('ov.network_error') + '</div>';
        toast(t('ov.network_error'));
    }
}
async function confirmHomePicker() {
    const checked = document.querySelector('#home-picker-list .home-item.on');
    const hid = (checked && checked.getAttribute('data-home-id')) || homePickerSelected;
    if (!hid) {
        toast(t('ov.pick_one_home'));
        return;
    }
    const mid = homePickerModule;
    if (!mid) {
        toast(t('ov.module_unspecified'));
        return;
    }
    closeHomePicker();
    await syncModule(mid, hid);
}
let matterWifiEnabled = false;

function matterDiscoveryValue() {
    /* Always BLE + DNS-SD (CHIP DiscoveryType::kAll). Wi-Fi toggle selects credentials. */
    return matterWifiEnabled ? 'ble_wifi' : 'ble_thread';
}

function syncMatterDiscoveryField() {
    const el = document.getElementById('matter-discovery');
    if (el) el.value = matterDiscoveryValue();
}

function refreshMatterAddUi() {
    const showWifi = matterWifiEnabled;
    const toggle = document.getElementById('matter-wifi-enable');
    if (toggle) toggle.checked = matterWifiEnabled;
    const wifiBlock = document.getElementById('matter-wifi-block');
    if (wifiBlock) wifiBlock.hidden = !showWifi;
    const ssidEl = document.getElementById('matter-wifi-ssid');
    const passEl = document.getElementById('matter-wifi-pass');
    if (ssidEl) ssidEl.disabled = !showWifi;
    if (passEl) passEl.disabled = !showWifi;
    syncMatterDiscoveryField();
    const hintEl = document.getElementById('matter-add-hint');
    if (hintEl) {
        hintEl.textContent = showWifi
            ? t('ov.matter_hint_wifi')
            : t('ov.matter_hint_thread');
        hintEl.hidden = false;
    }
}

function onMatterWifiToggle() {
    const toggle = document.getElementById('matter-wifi-enable');
    matterWifiEnabled = !!(toggle && toggle.checked);
    refreshMatterAddUi();
}

function openMatterAdd() {
    matterWifiEnabled = false;
    document.getElementById('matter-code').value = '';
    document.getElementById('matter-node').value = '';
    document.getElementById('matter-eps').value = '1';
    const pref = (matterStatus && matterStatus.wifi_prefill) || {};
    document.getElementById('matter-wifi-ssid').value = pref.ssid || '';
    document.getElementById('matter-wifi-pass').value = '';
    document.getElementById('matter-wifi-pass').placeholder = pref.has_password ? t('ov.saved_password_hint') : t('ov.wifi_password');
    const submitBtn = document.getElementById('matter-add-submit');
    if (submitBtn) submitBtn.disabled = false;
    refreshMatterAddUi();
    document.getElementById('matter-add-modal').hidden = false;
    document.getElementById('matter-code').focus();
}

function closeMatterAdd() {
    document.getElementById('matter-add-modal').hidden = true;
}

/* Stage names are catalog strings, resolved at render time: the dictionary is
 * not loaded yet while this file is being evaluated, so no lookup at parse time. */
function matterStageLabel(stage) {
    if (stage === 'discovering') return t('ov.stage_discovering');
    if (stage === 'binding') return t('ov.stage_binding');
    if (stage === 'activating') return t('ov.stage_activating');
    return '';
}
const MATTER_STAGE_STEP = { discovering: 1, binding: 2, activating: 3 };
const MATTER_STAGE_TOTAL = 3;

/* A waiting row is only "配网中" while the Hub is actually running that node;
 * the live stage comes from /api/matter/status, not from adapters.json. */
function matterCommissionLabel(p, cPhase) {
    if (cPhase !== 'waiting') return '';
    const st = (typeof matterStatus === 'undefined') ? null : matterStatus;
    const running = st && st.commission === 'running' && st.commission_node_id != null &&
        String(st.commission_node_id) === String((p.props || {}).node_id);
    if (!running) return t('ov.waiting_commission');
    const step = MATTER_STAGE_STEP[st.commission_stage];
    return t('ov.commissioning_stage', { stage: matterStageLabel(st.commission_stage) || t('ov.in_progress') }) +
        (step ? t('ov.stage_step', { step: step, total: MATTER_STAGE_TOTAL }) : '');
}

async function pollMatterCommissionCard(nodeId) {
    const deadline = Date.now() + 180000;
    let lastStage = '';
    while (Date.now() < deadline) {
        await new Promise((res) => setTimeout(res, 1500));
        try {
            await loadPoints();
            const sr = await fetch('/api/matter/status');
            const st = await sr.json().catch(() => ({}));
            const stage = st.commission_stage || '';
            if (stage && stage !== lastStage) {
                lastStage = stage;
                toast((matterStageLabel(stage) || stage) + '… node ' + (st.commission_node_id || nodeId));
            }
            if (st.commission === 'ok') {
                await loadMeta();
                await loadPoints();
                toast(t('ov.commission_ok', { node: (st.commission_node_id || nodeId) }));
                return;
            }
            if (st.commission === 'failed') {
                await loadPoints();
                toast(t('ov.commission_failed') + (lastStage ? t('ov.paren_value', { v: (matterStageLabel(lastStage) || lastStage) }) : '') +
                    ': ' + String(st.last_error || st.commission_result || '').slice(0, 80));
                return;
            }
        } catch (e) {
            /* keep polling */
        }
    }
    await loadPoints();
    toast(t('ov.commission_timeout'));
}

async function cancelMatterCommission(nodeId) {
    const nid = (nodeId != null && nodeId !== '')
        ? String(nodeId)
        : (matterStatus && matterStatus.commission_node_id != null
            ? String(matterStatus.commission_node_id) : null);
    if (!nid) {
        toast(t('ov.no_active_commission'));
        return;
    }
    try {
        const r = await fetch('/api/matter/commission', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ node_id: nid, cancel: true })
        });
        const j = await r.json().catch(() => ({}));
        toast(t('ov.cancel_sent_prefix') + (j.status || (r.ok ? 'ok' : t('ov.failed'))));
    } catch (e) {
        toast(t('ov.cancel_commission_failed'));
    }
    if (typeof loadMeta === 'function') await loadMeta();
    if (typeof loadPoints === 'function') await loadPoints();
}

async function submitMatterAdd() {
    const code = document.getElementById('matter-code').value.trim();
    const node = document.getElementById('matter-node').value.trim();
    const eps = document.getElementById('matter-eps').value.trim() || '1';
    const discovery = matterDiscoveryValue();
    const ssidEl = document.getElementById('matter-wifi-ssid');
    const passEl = document.getElementById('matter-wifi-pass');
    const ssid = ssidEl && !ssidEl.disabled ? ssidEl.value.trim() : '';
    const wpass = passEl && !passEl.disabled ? passEl.value : '';
    const hint = document.getElementById('matter-add-hint');
    const submitBtn = document.getElementById('matter-add-submit');
    if (!code) {
        hint.hidden = false;
        hint.textContent = t('ov.pairing_code_required');
        return;
    }
    if (discovery === 'ble_wifi' && !ssid) {
        hint.hidden = false;
        hint.textContent = t('ov.ssid_required');
        return;
    }
    toast(t('ov.adding'));
    const body = {
        pairing_code: code,
        endpoints: eps,
        merge: true,
        reload: true,
        discovery
    };
    if (node) body.node_id = node;
    if (discovery === 'ble_wifi') {
        body.wifi_ssid = ssid;
        if (wpass) body.wifi_password = wpass;
        body.transport = 'wifi';
    } else {
        body.transport = 'thread';
    }
    if (submitBtn) submitBtn.disabled = true;
    try {
        const r = await fetch('/api/matter/devices', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify(body)
        });
        const j = await r.json().catch(() => ({}));
        if (r.ok && j.result === 'ok' && j.commission === 'skipped') {
            closeMatterAdd();
            await loadMeta();
            await loadPoints();
            toast(t('ov.added_node', { node: j.node_id }));
            return;
        }
        if (r.ok && j.result === 'ok' && j.commission === 'started') {
            closeMatterAdd();
            await loadMeta();
            await loadPoints();
            toast(t('ov.added_waiting'));
            pollMatterCommissionCard(j.node_id);
            return;
        }
        /* Start failed after eager upsert — card may show 配网失败. */
        closeMatterAdd();
        await loadPoints();
        const msg = j.message || j.commission || j.error || ('HTTP ' + r.status);
        toast(t('ov.commission_failed') + ': ' + String(msg).slice(0, 80));
    } catch (e) {
        toast(t('ov.network_error'));
    } finally {
        if (submitBtn) submitBtn.disabled = false;
    }
}
async function deleteMatterDevice(pointId) {
    if (!confirm(t('ov.delete_confirm', { id: pointId }))) return;
    toast(t('ov.deleting'));
    try {
        const r = await fetch('/api/matter/devices', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify({
                delete: true,
                point_id: pointId,
                reload: true
            })
        });
        const j = await r.json().catch(() => ({}));
        if (r.ok && (j.result === 'ok' || j.ok === true)) {
            /* Drop locally first — sync hub_reload used to block httpd forever. */
            if (Array.isArray(pts)) {
                pts = pts.filter(function (p) { return !p || p.point_id !== pointId; });
            }
            if (typeof renderOverview === 'function') renderOverview();
            await loadPoints();
            toast(t('ov.deleted'));
        } else {
            toast(t('ov.delete_failed') + ': ' + String(j.message || j.error || ('HTTP ' + r.status)).slice(0, 60));
        }
    } catch (e) {
        toast(t('ov.network_error'));
    }
}
async function deleteZigbeeDevice(pointId) {
    if (!confirm(t('ov.delete_zigbee_confirm', { id: pointId }))) return;
    toast(t('ov.deleting'));
    try {
        const r = await fetch('/api/zigbee/devices', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify({
                delete: true,
                point_id: pointId
            })
        });
        const j = await r.json().catch(() => ({}));
        if (r.ok && j.result === 'ok') {
            await loadPoints();
            toast(t('ov.deleted'));
        } else {
            toast(t('ov.delete_failed'));
        }
    } catch (e) {
        toast(t('ov.network_error'));
    }
}
async function deleteBleMeshDevice(pointId) {
    if (!confirm(t('ov.delete_confirm', { id: pointId }))) return;
    toast(t('ov.deleting'));
    try {
        const r = await fetch('/api/ble_mesh/devices', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify({
                delete: true,
                point_id: pointId
            })
        });
        const j = await r.json().catch(() => ({}));
        if (r.ok && j.result === 'ok') {
            await loadPoints();
            toast(t('ov.deleted'));
        } else {
            toast(t('ov.delete_failed'));
        }
    } catch (e) {
        toast(t('ov.network_error'));
    }
}

function pointParamVal(p, name) {
    const pr = (p && p.props && typeof p.props === 'object') ? p.props : {};
    if (pr.params && typeof pr.params === 'object' && !Array.isArray(pr.params)) {
        const ent = pr.params[name];
        if (ent && ent.value != null) return ent.value;
    }
    if (Array.isArray(pr.params)) {
        const ent = pr.params.find(e => e && e.name === name);
        if (ent && ent.value != null) return ent.value;
    }
    if (pr[name] != null) return pr[name];
    return null;
}

function switchHasDetail(p) {
    const pr = (p && p.props) || {};
    if (pr.power_w != null || pr.energy_wh != null || pr.power != null || pr.energy != null) return true;
    if (pr.indicator != null) return true;
    const ex = pr.extras;
    return Array.isArray(ex) && ex.length > 0;
}
/* Looked up at render time (like matterStageLabel) so the dictionary is already
 * loaded; unknown keys fall back to the raw extras key at the call site. */
function extraLabel(key) {
    if (key === 'indicator') return t('ov.extra_indicator');
    if (key === 'motion_detection') return t('ov.extra_motion_detection');
    if (key === 'human_tracking') return t('ov.extra_human_tracking');
    if (key === 'time_watermark') return t('ov.extra_time_watermark');
    if (key === 'wdr') return t('ov.extra_wdr');
    if (key === 'dnd') return t('ov.extra_dnd');
    return '';
}

function extraSwitchOn(pr, key) {
    const v = (pr || {})[key];
    return v === true || v === 'on' || v === 'true' || v === 1 || v === '1';
}

function extrasSwitches(p) {
    const pr = (p && p.props) || {};
    const arr = Array.isArray(pr.extras) ? pr.extras : [];
    const seen = {};
    const out = [];
    arr.forEach(e => {
        if (!e || e.kind !== 'switch' || !e.key || e.key === 'volume' || e.key === 'motion_tracking') return;
        if (seen[e.key]) return;
        seen[e.key] = 1;
        out.push(e);
    });
    if (pr.indicator != null && !seen.indicator) {
        out.push({
            key: 'indicator',
            kind: 'switch'
        });
        seen.indicator = 1;
    }
    if (pr.dnd != null && !seen.dnd) {
        out.push({
            key: 'dnd',
            kind: 'switch'
        });
    }
    return out;
}

function renderExtraToggles(boxId, p, idFn) {
    const box = document.getElementById(boxId);
    if (!box) return;
    const pr = (p && p.props) || {};
    const list = extrasSwitches(p);
    const dis = !p.online ? ' disabled' : '';
    box.innerHTML = list.map(e => {
        const lab = extraLabel(e.key) || e.key;
        const on = extraSwitchOn(pr, e.key);
        const sid = e.siid || 0,
            pid = e.piid || 0;
        return '<div class="detail-power"><span class="lbl">' + esc(lab) + '</span><label class="toggle"><input type="checkbox" data-extra-key="' + escAttr(e.key) + '" data-siid="' + sid + '" data-piid="' + pid + '"' + (on ? ' checked' : '') + dis + '><span class="slider"></span></label></div>';
    }).join('');
    box.querySelectorAll('input[type=checkbox]').forEach(inp => {
        inp.onchange = () => {
            const id = idFn();
            if (!id) return;
            const key = inp.getAttribute('data-extra-key');
            const siid = parseInt(inp.getAttribute('data-siid'), 10) || 0;
            const piid = parseInt(inp.getAttribute('data-piid'), 10) || 0;
            const params = {
                key: key,
                value: !!inp.checked
            };
            if (siid > 0) params.siid = siid;
            if (piid > 0) params.piid = piid;
            cmd(id, 'set_prop', params);
        };
    });
}

/* One source of truth for "this plug reports metering": the caller hides the row
 * from this, not from the rendered text (an em-dash check broke the moment the
 * placeholder became a translatable string). */
function plugMeterBits(p) {
    const pr = (p && p.props) || {};
    const w = pr.power_w != null ? pr.power_w : pr.power;
    const wh = pr.energy_wh != null ? pr.energy_wh : pr.energy;
    const bits = [];
    if (w != null && w !== '') bits.push(t('ov.plug_power', { v: Number(w) }));
    if (wh != null && wh !== '') bits.push(t('ov.plug_energy', { v: Number(wh), unit: (pr.energy_wh != null ? ' Wh' : ' kWh') }));
    return bits;
}

function plugMeterText(p) {
    const bits = plugMeterBits(p);
    return bits.length ? bits.join(' · ') : t('ov.meter_empty');
}

function speakerVolVal(p) {
    const pr = (p && p.props) || {};
    const clamp = n => Math.min(100, Math.max(5, n));
    if (pr.volume != null && pr.volume !== '') {
        const n = parseInt(pr.volume, 10);
        if (Number.isFinite(n)) return clamp(n);
    }
    const m = String(p && p.value || '').match(/(\d+)/);
    if (m) return clamp(parseInt(m[1], 10));
    return 50;
}
let plugDetailId = null,
    cameraDetailId = null,
    speakerDetailId = null;

function closePlugDetail() {
    const m = document.getElementById('plug-detail-modal');
    if (m) m.hidden = true;
    plugDetailId = null;
}

function openPlugDetail(id, ev) {
    if (ev && ev.target && (ev.target.closest('.toggle') || ev.target.closest('button') || ev.target.closest('input') || ev.target.closest('.card-actions'))) return;
    const p = pts.find(x => x.point_id === id);
    if (!p || p.type !== 'switch') return;
    plugDetailId = id;
    document.getElementById('plug-detail-title').textContent = (TYPE_ICON.switch || '') + ' ' + (p.point_id.split('/').pop());
    document.getElementById('plug-detail-meta').textContent = p.point_id + (p.online ? ' · ' + t('ov.online') : ' · ' + t('ov.offline'));
    const noCtrl = !!(p.props && p.props.no_control);
    const prow = document.getElementById('plug-power-row');
    prow.hidden = !!noCtrl;
    if (!noCtrl) {
        const on = p.value === 'on' || p.value === 'true';
        const pow = document.getElementById('plug-power');
        pow.checked = on;
        pow.disabled = !p.online;
        pow.onchange = () => {
            if (!plugDetailId) return;
            cmd(plugDetailId, pow.checked ? 'turn_on' : 'turn_off');
        };
    }
    const meter = document.getElementById('plug-meter');
    const mt = plugMeterText(p);
    meter.textContent = mt;
    meter.hidden = plugMeterBits(p).length === 0;
    renderExtraToggles('plug-extras', p, () => plugDetailId);
    document.getElementById('plug-detail-modal').hidden = false;
}

function closeCameraDetail() {
    const m = document.getElementById('camera-detail-modal');
    if (m) m.hidden = true;
    cameraDetailId = null;
}

function refreshCameraPreview() {
    if (!cameraDetailId) return;
    const p = pts.find(x => x.point_id === cameraDetailId);
    const ad = p && p.adapter;
    const img = document.getElementById('camera-preview-img');
    const ph = document.getElementById('camera-preview-ph');
    if (!ad) {
        if (ph) {
            ph.hidden = false;
            ph.innerHTML = t('ov.cannot_preview') + '<div class="cam-preview-sub">' + t('ov.no_module_adapter') + '</div>';
        }
        if (img) img.hidden = true;
        return;
    }
    if (ph) {
        ph.hidden = false;
        ph.innerHTML = t('ov.loading_preview') + '<div class="cam-preview-sub">' + t('ov.last_snapshot') + '</div>';
    }
    if (!img) return;
    img.hidden = true;
    img.onload = function() {
        img.hidden = false;
        if (ph) ph.hidden = true;
    };
    img.onerror = function() {
        img.hidden = true;
        if (ph) {
            ph.hidden = false;
            ph.innerHTML = t('ov.no_snapshot') + '<div class="cam-preview-sub">' + t('ov.no_frame') + '</div>';
        }
    };
    img.src = '/api/modules/' + encodeURIComponent(ad) + '/preview?point_id=' + encodeURIComponent(cameraDetailId) + '&t=' + Date.now();
}

function openCameraDetail(id, ev) {
    if (ev && ev.target && (ev.target.closest('.toggle') || ev.target.closest('button') || ev.target.closest('input') || ev.target.closest('.card-actions'))) return;
    const p = pts.find(x => x.point_id === id);
    if (!p || p.type !== 'camera') return;
    cameraDetailId = id;
    document.getElementById('camera-detail-title').textContent = (TYPE_ICON.camera || '') + ' ' + (p.point_id.split('/').pop());
    document.getElementById('camera-detail-meta').textContent = p.point_id + (p.online ? ' · ' + t('ov.online') : ' · ' + t('ov.offline'));
    const noCtrl = !!(p.props && p.props.no_control);
    const prow = document.getElementById('camera-power-row');
    prow.hidden = !!noCtrl;
    if (!noCtrl) {
        const on = p.value === 'on' || p.value === 'true';
        const pow = document.getElementById('camera-power');
        pow.checked = on;
        pow.disabled = !p.online;
        pow.onchange = () => {
            if (!cameraDetailId) return;
            cmd(cameraDetailId, pow.checked ? 'turn_on' : 'turn_off');
        };
    }
    renderExtraToggles('camera-extras', p, () => cameraDetailId);
    const vb = document.getElementById('camera-voice-block');
    const hasVoice = !!((p.props && Array.isArray(p.props.extras) ? p.props.extras : []).find(e => e && (e.key === 'play_text' || e.key === 'execute_text')));
    if (vb) vb.hidden = !hasVoice;
    const sro = document.getElementById('camera-stream-ro');
    const st = cameraStreamText(p);
    if (sro) {
        sro.textContent = st;
        sro.hidden = !st;
    }
    document.getElementById('camera-detail-modal').hidden = false;
    refreshCameraPreview();
}

function closeSpeakerDetail() {
    const m = document.getElementById('speaker-detail-modal');
    if (m) m.hidden = true;
    speakerDetailId = null;
}

function openSpeakerDetail(id, ev) {
    if (ev && ev.target && (ev.target.closest('.toggle') || ev.target.closest('button') || ev.target.closest('input') || ev.target.closest('.card-actions'))) return;
    const p = pts.find(x => x.point_id === id);
    if (!p || p.type !== 'speaker') return;
    speakerDetailId = id;
    document.getElementById('speaker-detail-title').textContent = (TYPE_ICON.speaker || '') + ' ' + (p.point_id.split('/').pop());
    document.getElementById('speaker-detail-meta').textContent = p.point_id + (p.online ? ' · ' + t('ov.online') : ' · ' + t('ov.offline'));
    const vol = speakerVolVal(p);
    const vEl = document.getElementById('speaker-vol');
    vEl.value = vol;
    document.getElementById('speaker-vol-val').textContent = vol + '%';
    vEl.disabled = !p.online;
    renderExtraToggles('speaker-extras', p, () => speakerDetailId);
    document.getElementById('speaker-detail-modal').hidden = false;
}

function speakerPlayText() {
    if (!speakerDetailId) return;
    const el = document.getElementById('speaker-tts');
    const t = (el && el.value || '').trim();
    if (!t) {
        toast(t('ov.enter_text'));
        return;
    }
    cmd(speakerDetailId, 'call_action', {
        key: 'play_text',
        text: t
    });
}

function cameraPlayText() {
    if (!cameraDetailId) return;
    const el = document.getElementById('camera-tts');
    const t = (el && el.value || '').trim();
    if (!t) {
        toast(t('ov.enter_text'));
        return;
    }
    const p = pts.find(x => x.point_id === cameraDetailId);
    const ex = ((p && p.props && p.props.extras) || []);
    const hasPlay = ex.some(e => e && e.key === 'play_text');
    cmd(cameraDetailId, 'call_action', {
        key: hasPlay ? 'play_text' : 'execute_text',
        text: t
    });
}

function bindHaParityControls() {
    const v = document.getElementById('speaker-vol');
    if (v && !v._bound) {
        v._bound = 1;
        v.addEventListener('input', () => {
            const el = document.getElementById('speaker-vol-val');
            if (el) el.textContent = v.value + '%';
        });
        v.addEventListener('change', () => {
            if (!speakerDetailId) return;
            cmd(speakerDetailId, 'set_volume', {
                volume: parseInt(v.value, 10) || 0
            });
        });
    }
}

let applianceDetailId = null;

function closeApplianceDetail() {
    const m = document.getElementById('appliance-detail-modal');
    if (m) m.hidden = true;
    applianceDetailId = null;
}

function openApplianceDetail(id, ev) {
    if (ev && ev.target && (ev.target.closest('.toggle') || ev.target.closest('button') || ev.target.closest('input') || ev.target.closest('.card-actions'))) return;
    const p = pts.find(x => x.point_id === id);
    if (!p || !['fridge', 'water_purifier', 'water_heater', 'appliance'].includes(p.type)) return;
    applianceDetailId = id;
    const name = (p.props && p.props.name) || pointDisplayName(p);
    document.getElementById('appliance-detail-title').textContent = (TYPE_ICON[p.type] || '') + ' ' + name;
    document.getElementById('appliance-detail-meta').textContent = ((typeof dwTypeDisplay === 'function') ? dwTypeDisplay(p) : mhTypeLabel(p.type)) + (p.online ? ' · ' + t('ov.online') : ' · ' + t('ov.offline'));
    const on = p.value === 'on' || p.value === 'true';
    const pow = document.getElementById('appliance-power');
    const noCtrl = !!(p.props && p.props.no_control);
    pow.checked = on;
    pow.disabled = !p.online || noCtrl;
    pow.onchange = () => {
        if (!applianceDetailId) return;
        cmd(applianceDetailId, pow.checked ? 'turn_on' : 'turn_off');
    };
    const pr = p.props || {};
    const tempBlock = document.getElementById('appliance-temp-block');
    const coldB = document.getElementById('appliance-cold-block');
    const freezeB = document.getElementById('appliance-freeze-block');
    const waterB = document.getElementById('appliance-water-temp-block');
    const showFridge = p.type === 'fridge';
    const showWater = p.type === 'water_heater';
    tempBlock.hidden = !(showFridge || showWater);
    coldB.hidden = !showFridge;
    freezeB.hidden = !showFridge;
    waterB.hidden = !showWater;
    if (showFridge) {
        const cs = pr.cold_set != null ? Number(pr.cold_set) : (pr.cold_temp != null ? Number(pr.cold_temp) : 4);
        const fs = pr.freeze_set != null ? Number(pr.freeze_set) : (pr.freeze_temp != null ? Number(pr.freeze_temp) : -18);
        const cEl = document.getElementById('appliance-cold');
        const fEl = document.getElementById('appliance-freeze');
        cEl.value = Math.min(8, Math.max(2, cs || 4));
        fEl.value = Math.min(-15, Math.max(-24, fs || -18));
        document.getElementById('appliance-cold-val').textContent = cEl.value + '°C';
        document.getElementById('appliance-freeze-val').textContent = fEl.value + '°C';
        cEl.disabled = !p.online;
        fEl.disabled = !p.online;
        if (!cEl._bound) {
            cEl._bound = 1;
            cEl.addEventListener('input', () => {
                document.getElementById('appliance-cold-val').textContent = cEl.value + '°C';
            });
            cEl.addEventListener('change', () => {
                if (!applianceDetailId) return;
                cmd(applianceDetailId, 'set_temperature', {
                    zone: 'cold',
                    temperature: parseInt(cEl.value, 10)
                });
            });
        }
        if (!fEl._bound) {
            fEl._bound = 1;
            fEl.addEventListener('input', () => {
                document.getElementById('appliance-freeze-val').textContent = fEl.value + '°C';
            });
            fEl.addEventListener('change', () => {
                if (!applianceDetailId) return;
                cmd(applianceDetailId, 'set_temperature', {
                    zone: 'freeze',
                    temperature: parseInt(fEl.value, 10)
                });
            });
        }
    }
    if (showWater) {
        const wt = pr.tea_temp != null ? Number(pr.tea_temp) : (pr.temperature != null ? Number(pr.temperature) : 45);
        const wEl = document.getElementById('appliance-water-temp');
        wEl.value = Math.min(99, Math.max(40, wt || 45));
        document.getElementById('appliance-water-temp-val').textContent = wEl.value + '°C';
        wEl.disabled = !p.online;
        if (!wEl._bound) {
            wEl._bound = 1;
            wEl.addEventListener('input', () => {
                document.getElementById('appliance-water-temp-val').textContent = wEl.value + '°C';
            });
            wEl.addEventListener('change', () => {
                if (!applianceDetailId) return;
                cmd(applianceDetailId, 'set_temperature', {
                    zone: 'tea',
                    temperature: parseInt(wEl.value, 10)
                });
            });
        }
    }
    const box = document.getElementById('appliance-detail-rows');
    const rows = (Array.isArray(pr.detail_rows) ? pr.detail_rows : []).filter(r => r && r.label && !/电源/.test(r.label || ''));
    if (rows.length) {
        box.innerHTML = rows.map(r => '<div style="display:flex;justify-content:space-between;gap:12px;padding:6px 0;border-bottom:1px solid var(--border)"><span style="color:var(--muted)">' + esc(r.label || '') + '</span><span>' + esc(r.value || '') + '</span></div>').join('');
    } else {
        box.innerHTML = '<div class="modal-hint">' + t('ov.no_more_status') + '</div>';
    }
    document.getElementById('appliance-detail-modal').hidden = false;
}

let acDetailId = null,
    coverDetailId = null;

function closeAcDetail() {
    const m = document.getElementById('ac-detail-modal');
    if (m) m.hidden = true;
    acDetailId = null;
}

function closeCoverDetail() {
    const m = document.getElementById('cover-detail-modal');
    if (m) m.hidden = true;
    coverDetailId = null;
}

function acModeKey(p) {
    const pr = (p && p.props) || {};
    const m = pr.mode;
    const lab = String(pr.mode_label || '');
    if (m == 1 || lab.indexOf('制冷') >= 0) return 'cool';
    if (m == 8 || lab.indexOf('制热') >= 0) return 'heat';
    if (m == 2 || lab.indexOf('除湿') >= 0) return 'dry';
    if (m == 4 || lab.indexOf('送风') >= 0) return 'fan';
    return '';
}

function acFanKey(p) {
    const pr = (p && p.props) || {};
    const f = pr.fan_level;
    const lab = String(pr.fan_label || '');
    if (f === 0 || lab.indexOf('自动') >= 0) return 'auto';
    if (f == 1 || lab === '低') return 'low';
    if (f == 2 || lab === '中') return 'medium';
    if (f == 4 || lab === '高') return 'high';
    return '';
}

function openAcDetail(id, ev) {
    if (ev && ev.target && (ev.target.closest('.toggle') || ev.target.closest('button') || ev.target.closest('input') || ev.target.closest('.card-actions'))) return;
    const p = pts.find(x => x.point_id === id);
    if (!p || p.type !== 'air-conditioner') return;
    acDetailId = id;
    document.getElementById('ac-detail-title').textContent = (TYPE_ICON['air-conditioner'] || '') + ' ' + (p.point_id.split('/').pop());
    document.getElementById('ac-detail-meta').textContent = p.point_id + (p.online ? ' · ' + t('ov.online') : ' · ' + t('ov.offline'));
    const pr = p.props || {};
    const on = p.value === 'on' || p.value === 'true';
    const pow = document.getElementById('ac-power');
    pow.checked = on;
    pow.disabled = !p.online;
    const mk = acModeKey(p);
    document.querySelectorAll('#ac-mode-seg button').forEach(b => {
        b.classList.toggle('on', b.dataset.mode === mk);
        b.disabled = !p.online;
    });
    const tt = pr.target_temp != null ? Number(pr.target_temp) : 26;
    const tEl = document.getElementById('ac-temp');
    tEl.value = Math.min(30, Math.max(16, tt || 26));
    document.getElementById('ac-temp-val').textContent = tEl.value + '°';
    tEl.disabled = !p.online;
    const fk = acFanKey(p);
    document.querySelectorAll('#ac-fan-seg button').forEach(b => {
        b.classList.toggle('on', b.dataset.fan === fk);
        b.disabled = !p.online;
    });
    const amb = pr.temperature != null ? t('ov.room_temp', { v: pr.temperature }) : t('ov.room_temp_none');
    document.getElementById('ac-ambient').textContent = amb;
    document.getElementById('ac-detail-modal').hidden = false;
}

function acSetMode(mode) {
    if (!acDetailId) return;
    document.querySelectorAll('#ac-mode-seg button').forEach(b => b.classList.toggle('on', b.dataset.mode === mode));
    cmd(acDetailId, 'set_mode', {
        mode: mode
    });
}

function acSetFan(fan) {
    if (!acDetailId) return;
    document.querySelectorAll('#ac-fan-seg button').forEach(b => b.classList.toggle('on', b.dataset.fan === fan));
    cmd(acDetailId, 'set_fan_level', {
        fan_level: fan
    });
}

function openCoverDetail(id, ev) {
    if (ev && ev.target && (ev.target.closest('.toggle') || ev.target.closest('button') || ev.target.closest('input') || ev.target.closest('.card-actions'))) return;
    const p = pts.find(x => x.point_id === id);
    if (!p || !isCoverType(p.type)) return;
    coverDetailId = id;
    document.getElementById('cover-detail-title').textContent = (TYPE_ICON.cover || '') + ' ' + (p.point_id.split('/').pop());
    document.getElementById('cover-detail-meta').textContent = p.point_id + (p.online ? ' · ' + t('ov.online') : ' · ' + t('ov.offline'));
    const pr = p.props || {};
    let pct = parseInt(p.value, 10);
    if (!Number.isFinite(pct) && pr.position != null) pct = parseInt(pr.position, 10);
    if (!Number.isFinite(pct)) pct = 0;
    pct = Math.min(100, Math.max(0, pct));
    document.getElementById('cover-pos').value = pct;
    document.getElementById('cover-pos-val').textContent = pct + '%';
    document.getElementById('cover-pos-label').textContent = pct + '%';
    document.getElementById('cover-pos').disabled = !p.online;
    ['cover-open', 'cover-close', 'cover-stop'].forEach(id0 => {
        const el = document.getElementById(id0);
        if (el) el.disabled = !p.online;
    });
    document.getElementById('cover-detail-modal').hidden = false;
}

function coverCmd(c) {
    if (!coverDetailId) return;
    cmd(coverDetailId, c);
}

function bindAcCoverControls() {
    const pow = document.getElementById('ac-power');
    const temp = document.getElementById('ac-temp');
    const pos = document.getElementById('cover-pos');
    if (pow && !pow._bound) {
        pow._bound = 1;
        pow.addEventListener('change', () => {
            if (!acDetailId) return;
            cmd(acDetailId, pow.checked ? 'turn_on' : 'turn_off');
        });
    }
    if (temp && !temp._bound) {
        temp._bound = 1;
        temp.addEventListener('input', () => {
            document.getElementById('ac-temp-val').textContent = temp.value + '°';
        });
        temp.addEventListener('change', () => {
            if (!acDetailId) return;
            const t = parseInt(temp.value, 10) || 26;
            cmd(acDetailId, 'set_temperature', {
                temperature: t,
                target_temp: t
            });
        });
    }
    if (pos && !pos._bound) {
        pos._bound = 1;
        pos.addEventListener('input', () => {
            document.getElementById('cover-pos-val').textContent = pos.value + '%';
            document.getElementById('cover-pos-label').textContent = pos.value + '%';
        });
        pos.addEventListener('change', () => {
            if (!coverDetailId) return;
            const pct = parseInt(pos.value, 10) || 0;
            cmd(coverDetailId, 'set_position', {
                position: pct
            });
        });
    }
}
try {
    bindAcCoverControls();
} catch (e) {
    console.warn('ac/cover controls', e);
}
try {
    bindHaParityControls();
} catch (e) {
    console.warn('ha-parity controls', e);
}
/* Optimistic power feedback for the overview toggle: flip the card right away so
 * a slow round-trip (or a missed refresh) is not felt as lag. The post-command
 * snapshot reconciles it, and reverts if the command fails. */
function cardPowerOn(p) {
    if (!p) return false;
    const v = pointParamVal(p, 'power');
    return v === 'on' || v === 'true' || v === true ||
        p.value === 'on' || p.value === 'true' || p.value === 'open';
}
function cardSetPower(id, on) {
    const p = pts.find(x => x.point_id === id);
    if (!p) return false;
    const val = on ? 'on' : 'off';
    const pr = p.props;
    if (pr && pr.params && typeof pr.params === 'object' && !Array.isArray(pr.params)) {
        if (!pr.params.power || typeof pr.params.power !== 'object') pr.params.power = {};
        pr.params.power.value = val;
    } else if (pr && Array.isArray(pr.params)) {
        const ent = pr.params.find(e => e && e.name === 'power');
        if (ent) ent.value = val;
        else pr.params.push({ name: 'power', value: val });
    } else {
        p.value = val;
    }
    return true;
}

async function cmd(id, c, params) {
    const p = pts.find(x => x.point_id === id);
    if (p && !p.online) {
        toast(t('ov.device_offline'));
        return;
    }
    /* Power toggles get instant local feedback; the snapshot that follows reconciles. */
    if (c === 'turn_on' || c === 'turn_off' || c === 'toggle') {
        const on = c === 'turn_on' ? true : (c === 'turn_off' ? false : !cardPowerOn(p));
        if (cardSetPower(id, on)) renderOverview();
    }
    try {
        const body = {
            point_id: id,
            cmd: c
        };
        if (params && typeof params === 'object') body.params = params;
        const r = await fetch('/api/command', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify(body)
        });
        if (r.ok) {
            toast(t('ov.sent'));
        } else {
            const raw = await r.text().catch(function () { return ''; });
            toast(t('dw.control_failed', { error: mhHttpErrorText(r.status, raw) }));
        }
        refreshAfterCommand();
    } catch (e) {
        toast(t('ov.network_error'));
    }
}
async function permitZigbeeJoin() {
    if (radioProv.zigbee_permit_join) {
        toast(t('ov.zigbee_join_already_on'));
        return;
    }
    if (radioProv.ble_mesh_provisioning || radioProv.ble_mesh_permit_provision) {
        toast(t('ov.ble_mesh_busy_later'));
        return;
    }
    toast(t('ov.opening_join'));
    try {
        const r = await fetch('/api/zigbee/permit_join', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify({
                seconds: 120
            })
        });
        const j = await r.json().catch(() => ({}));
        if (r.ok && j.result === 'ok') {
            radioProv.zigbee_permit_join = true;
            radioProv.busy = true;
            if (view === 'overview') renderOverview();
            toast(t('ov.zigbee_join_started'));
        } else {
            toast(t('ov.open_failed') + (j.error ? ': ' + j.error : (j.result ? ': ' + j.result : '')));
        }
    } catch (e) {
        toast(t('ov.network_error'));
    }
}
async function permitBleMeshProvision() {
    if (radioProv.ble_mesh_permit_provision || radioProv.ble_mesh_provisioning) {
        toast(t('ov.ble_mesh_prov_already_on'));
        return;
    }
    if (radioProv.zigbee_permit_join) {
        toast(t('ov.zigbee_busy_later'));
        return;
    }
    toast(t('ov.opening_prov'));
    try {
        const r = await fetch('/api/ble_mesh/permit_provision', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify({
                seconds: 120
            })
        });
        const j = await r.json().catch(() => ({}));
        if (r.ok && j.result === 'ok') {
            radioProv.ble_mesh_permit_provision = true;
            radioProv.ble_mesh_provisioning = true;
            radioProv.busy = true;
            if (view === 'overview') renderOverview();
            toast(t('ov.ble_mesh_prov_started'));
            loadPoints();
        } else {
            toast(t('ov.open_failed') + (j.error ? ': ' + j.error : (j.result ? ': ' + j.result : '')));
        }
    } catch (e) {
        toast(t('ov.network_error'));
    }
}
let radioProvLoading = false;
async function refreshRadioProv() {
    if (radioProvLoading) return;
    radioProvLoading = true;
    try {
        const rp = await fetch('/api/radio/provision');
        if (!rp.ok) return;
        const j = await rp.json();
        const changed = (!!radioProv.busy !== !!j.busy) || (!!radioProv.zigbee_permit_join !== !!j.zigbee_permit_join) || (!!radioProv.ble_mesh_provisioning !== !!j.ble_mesh_provisioning) || (!!radioProv.ble_mesh_permit_provision !== !!j.ble_mesh_permit_provision);
        radioProv = j;
        if (view === 'overview' && changed) renderOverview();
    } catch (e) {} finally {
        radioProvLoading = false;
    }
}
async function stopZigbeeJoin() {
    toast(t('ov.closing_join'));
    try {
        const r = await fetch('/api/zigbee/permit_join', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify({
                seconds: 0
            })
        });
        const j = await r.json().catch(() => ({}));
        if (r.ok && j.result === 'ok') {
            await refreshRadioProv();
            toast(t('ov.zigbee_join_stopped'));
        } else {
            toast(t('ov.close_failed') + (j.error ? ': ' + j.error : ''));
        }
    } catch (e) {
        toast(t('ov.network_error'));
    }
}
async function stopBleMeshProvision() {
    toast(t('ov.closing_prov'));
    try {
        const r = await fetch('/api/ble_mesh/permit_provision', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify({
                seconds: 0
            })
        });
        const j = await r.json().catch(() => ({}));
        if (r.ok && j.result === 'ok') {
            await refreshRadioProv();
            toast(t('ov.ble_mesh_prov_stopped'));
        } else {
            toast(t('ov.close_failed') + (j.error ? ': ' + j.error : ''));
        }
    } catch (e) {
        toast(t('ov.network_error'));
    }
}(function() {
    const q = document.getElementById('q');
    if (q) q.oninput = e => {
        query = e.target.value;
        renderOverview();
    };
    const sq = document.getElementById('store-q');
    if (sq) sq.oninput = e => {
        storeQ = e.target.value;
        renderStore();
    };
})();
async function fetchSystemFresh() {
    try {
        const r = await fetch('/api/system?_=' + Date.now(), {
            cache: 'no-store'
        });
        return r.ok ? await r.json() : null;
    } catch (e) {
        return null;
    }
}
async function syncLoginUrlToCurrentIp(moduleId) {
    const j = await fetchSystemFresh();
    if (!j || !j.local_ip) return false;
    const ip = j.local_ip;
    const base = (j.web_base && j.web_base !== '/') ? j.web_base : '';
    const target = location.protocol + '//' + ip + base + '?login=' + encodeURIComponent(moduleId);
    const host = location.hostname;
    if (host === 'localhost' || host === '127.0.0.1') return false;
    const isIp = /^\d{1,3}(\.\d{1,3}){3}$/.test(host);
    if (isIp && host !== ip) {
        location.replace(target);
        return true;
    }
    if (location.href.split('#')[0] !== target) {
        history.replaceState(null, '', target);
    }
    return false;
}
async function applyLoginHash() {
    try {
        const fromQuery = (new URLSearchParams(location.search || '')).get('login');
        const raw = (location.hash || '').replace(/^#/, '');
        const fromHash = raw ? (new URLSearchParams(raw.includes('=') ? raw : '')).get('login') : null;
        const id = fromQuery || fromHash;
        if (!id) return;
        if (await syncLoginUrlToCurrentIp(id)) return;
        const go = () => {
            if (typeof openModuleLogin === 'function') openModuleLogin(id);
        };
        if (Array.isArray(modules) && modules.some(m => m && m.id === id)) go();
        else loadModulesForAuth().then(go);
    } catch (e) {}
}
