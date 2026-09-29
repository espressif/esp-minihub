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
    const status = esc((usb ? 'USB 已挂载' : 'USB 未挂载') + ' · ' + (rec ? '录制中' : '已停止') + (file ? ' · ' + file : '') + ((rec || st.bytes_written) ? ' · ' + bytes : ''));
    const preview = '<div id="rtsp-cam-preview" style="aspect-ratio:16/9;max-width:28rem;margin:0 auto 12px;border-radius:10px;border:1px solid var(--border);background:var(--card);display:flex;align-items:center;justify-content:center;color:var(--muted);font-size:.85rem">暂无预览</div>';
    let acts = '';
    if (!rec) {
        acts += '<button class="btn-primary" type="button" ' + (usb ? '' : 'disabled ') + 'onclick="rtspCamStart()">开始</button>';
    } else {
        acts += '<button class="btn-sm bad" type="button" onclick="rtspCamStop()">停止</button>';
    }
    acts += '<a class="btn-sm" style="text-decoration:none;display:inline-block" href="/modules/rtsp_cam/web/">详情</a>';
    return '<div class="cta" id="rtsp-cam-card"><div style="font-weight:700">摄像头录像</div><p style="color:var(--muted);margin:12px 0">' + status + '</p>' + preview + '<div style="display:flex;flex-wrap:wrap;gap:8px;justify-content:center;align-items:center">' + acts + '</div>' + err + '</div>';
}
async function rtspCamStart() {
    toast('开始录制…');
    try {
        await moduleRpc('rtsp_cam', 'start');
        for (let i = 0; i < 25; i++) {
            await loadRtspCamStatus();
            if (rtspCamStatus && rtspCamStatus.recording) {
                toast('已开始');
                if (view === 'overview') renderOverview();
                return;
            }
            await new Promise(res => setTimeout(res, 200));
            if (view === 'overview') renderOverview();
        }
        toast((rtspCamStatus && rtspCamStatus.last_error) || '开始失败');
    } catch (e) {
        toast('网络错误');
        await loadRtspCamStatus();
    }
    if (view === 'overview') renderOverview();
}
async function rtspCamStop() {
    toast('停止录制…');
    try {
        const r = await moduleRpc('rtsp_cam', 'stop');
        if (!(r.ok && r.json && r.json.result === 'ok')) {
            toast((r.json && (r.json.error || r.json.last_error)) || '停止失败');
        } else {
            for (let i = 0; i < 20; i++) {
                await loadRtspCamStatus();
                if (!(rtspCamStatus && rtspCamStatus.recording)) {
                    toast('已停止');
                    break;
                }
                await new Promise(res => setTimeout(res, 200));
            }
            if (rtspCamStatus && rtspCamStatus.recording) toast('仍在录制，请重试');
        }
    } catch (e) {
        toast('网络错误');
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
        attribution: '© 高德地图'
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
    let h = '<button class="chip' + (filter === 'all' ? ' active' : '') + '" data-f="all">全部</button>';
    Object.keys(enabledAdapters).filter(adapterEnabled).sort().forEach(ad => {
        h += '<button class="chip' + (filter === ad ? ' active' : '') + '" data-f="' + ad + '">' + esc(adapterLabel(ad)) + '</button>';
    });
    document.getElementById('chips').innerHTML = h;
    bindChips();
    if (filter !== 'all' && !adapterEnabled(filter)) filter = 'all';
}

function localName() {
    return localStorage.getItem('minihub_display_name') || '用户';
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
    const kind = (cat && cat.kind === 'pack') ? 'pack' : 'module';
    let label = '升级';
    if (cat && cat.version) label += ' ' + cat.version;
    return '<button class="btn-sm on section-mod-upg" type="button" title="模块升级（MiniHub Store）" onclick="storeUpgrade(\'' +
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
        const acct = st.logged_in ? '<span class="mod-acct">' + esc(st.user_id || '-') + (st.region ? ' · ' + esc(st.region) : '') + '</span>' : (showActs ? '<span class="mod-acct off">' + esc(lbl(mod, 'logged_out', '未登录')) + '</span>' : '');
        const syncBadge = st.syncing ? '<span class="mod-acct busy">同步中…</span>' : '';
        let acts = '';
        if (showActs) {
            if (st.logged_in) {
                if (hasModAction(mod, 'sync')) acts += '<button class="btn-sm on" type="button" onclick="syncModule(\'' + mid + '\')">' + esc(lbl(mod, 'sync', '同步')) + '</button>';
                if (hasModAction(mod, 'homes')) acts += '<button class="btn-sm on" type="button" onclick="openHomePicker(\'' + mid + '\')">' + esc(lbl(mod, 'change_home', (st.home_name ? '切换家庭' : '选择家庭'))) + '</button>';
                if (hasModAction(mod, 'logout')) acts += '<button class="btn-sm bad" type="button" onclick="logoutModule(\'' + mid + '\')">' + esc(lbl(mod, 'logout', '退出')) + '</button>';
            } else {
                if (hasModAction(mod, 'login') && hasCredLogin(mod)) {
                    acts += '<button class="btn-sm on" type="button" onclick="openModuleLogin(\'' + mid + '\')">' + esc(lbl(mod, 'login', '登录')) + '</button>';
                }
                if (hasModAction(mod, 'session')) {
                    acts += '<button class="btn-sm" type="button" onclick="openModuleSession(\'' + mid + '\')">' + esc(lbl(mod, 'session', '导入会话')) + '</button>';
                }
            }
        }
        return '<div class="section-h auth-section-h"><span>' + esc(adapterLabel(ad)) + '</span>' + verTag + acct + syncBadge + '<span>' + count + '</span>' + upg + (acts ? '<span class="section-actions">' + acts + '</span>' : '') + '</div>';
    }
    if (ad === 'matter') {
        const st = matterStatus.ready ? '<span class="mod-acct">控制器就绪</span>' : (showActs ? '<span class="mod-acct off">未就绪</span>' : '');
        const running = matterStatus.commission === 'running';
        const acts = showActs ? (running
            ? '<button class="btn-sm bad" type="button" onclick="cancelMatterCommission()">取消配网</button>'
            : '<button class="btn-sm on" type="button" onclick="openMatterAdd()">添加</button>') : '';
        return '<div class="section-h matter-section-h"><span>' + (ADAPTER_LABEL[ad] || ad) + '</span>' + verTag + st + '<span>' + count + '</span>' + upg + (acts ? '<span class="section-actions">' + acts + '</span>' : '') + '</div>';
    }
    if (ad === 'zigbee') {
        const zbOn = !!radioProv.zigbee_permit_join;
        const bmBusy = !!(radioProv.ble_mesh_provisioning || radioProv.ble_mesh_permit_provision);
        const st = zbOn ? '<span class="mod-acct">正在入网</span>' : (bmBusy ? '<span class="mod-acct off">BLE Mesh 配网中</span>' : '');
        const acts = showActs ? (zbOn ? '<button class="btn-sm" type="button" onclick="stopZigbeeJoin()">停止入网</button>' : (bmBusy ? '<button class="btn-sm on" type="button" disabled title="BLE Mesh 配网中">允许入网</button>' : '<button class="btn-sm on" type="button" onclick="permitZigbeeJoin()">允许入网</button>')) : '';
        return '<div class="section-h radio-section-h"><span>' + esc(adapterLabel(ad)) + '</span>' + verTag + st + '<span>' + count + '</span>' + upg + (acts ? '<span class="section-actions">' + acts + '</span>' : '') + '</div>';
    }
    if (ad === 'ble_mesh') {
        const bmPermit = !!radioProv.ble_mesh_permit_provision;
        const bmProv = !!radioProv.ble_mesh_provisioning;
        const bmOn = bmPermit || bmProv;
        const zbBusy = !!radioProv.zigbee_permit_join;
        const st = bmOn ? '<span class="mod-acct">正在配网</span>' : (zbBusy ? '<span class="mod-acct off">Zigbee 入网中</span>' : '');
        const acts = showActs ? (bmPermit ? '<button class="btn-sm" type="button" onclick="stopBleMeshProvision()">停止配网</button>' : (bmProv ? '<button class="btn-sm on" type="button" disabled>正在配网</button>' : (zbBusy ? '<button class="btn-sm on" type="button" disabled title="Zigbee 入网中">允许配网</button>' : '<button class="btn-sm on" type="button" onclick="permitBleMeshProvision()">允许配网</button>'))) : '';
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
        detail.innerHTML = '<button class="btn-sm" type="button" onclick="rulesBack()">← 返回列表</button>' +
            '<button class="btn-sm on" type="button" style="margin-left:8px" onclick="rulesEditOpen(\'' + escAttr(rule.id) + '\')">编辑</button>' +
            '<div class="dcard" style="margin-top:12px">' +
            '<div class="card-top"><div class="card-name">' + esc(rule.name || rule.id || '规则') + '</div>' + (dis ? '' : '<span class="badge sensor">启用</span>') + '</div>' +
            (dis ? '<div class="card-meta"><span>已禁用</span></div>' : '') +
            '<div class="rule-flow">' + ruleStepHtml('当 · 触发', w) + '<div class="rule-arrow">↓</div>' + ruleStepHtml('且 · 条件', i) + '<div class="rule-arrow">↓</div>' + ruleStepHtml('则 · 动作', th) + '</div>' +
            '<details class="rule-tech"><summary>技术详情（JSON）</summary>' +
            '<h4 class="rule-sec">when</h4><pre class="rule-pre">' + esc(JSON.stringify(rule.when || {}, null, 2)) + '</pre>' +
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
        list.innerHTML = '<div class="empty">暂无规则 — 点击「添加规则」用选框创建，或 URL 导入</div>';
        return;
    }
    list.className = 'grid';
    list.innerHTML = arr.map(function(r) {
        const dis = r.enabled === false;
        return '<div class="dcard" style="cursor:pointer" data-rule-id="' + escAttr(r.id || '') + '">' +
            '<div class="card-top"><div class="card-name">' + esc(r.name || r.id || '(unnamed)') + '</div>' + (dis ? '' : '<span class="badge sensor">启用</span>') + '</div>' +
            '<div class="card-meta"><span>' + esc(whenSummary(r.when)) + ' → ' + esc(thenSummary(r.then)) + '</span>' + (dis ? '<span>禁用</span>' : '') + '</div></div>';
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
    const skip = new Set(['extras', 'params', 'owner', 'indicator', 'volume', 'dnd', 'motion_detection', 'motion_tracking', 'time_watermark', 'wdr', 'play_text', 'execute_text', 'open', 'zone_status', 'tamper', 'battery_low', 'name', 'room_name', 'room_id', 'home_name', 'home_id', 'appliance_id', 'category', 'product', 'product_name', 'no_control', 'transport', 'commissioning', 'commission_phase', 'detail_rows', 'ieee', 'uuid', 'node_id', 'manufacturer', 'vendor', 'manufacturer_name', 'model', 'model_id', 'model_identifier', 'sw_version', 'fw_version', 'firmware_version', 'app_version', 'device_version', 'hw_version', 'hardware_version', 'ota', 'cold_temp', 'cold_set', 'freeze_temp', 'freeze_set', 'left_zone_temp', 'right_zone_temp', 'tea_temp', 'milk_temp', 'target_temp', 'mode_label', 'card_summary', 'in_tds', 'out_tds', 'filter_ro_life', 'filter_ro_max', 'water_total_l', 'status_text', 'battery', 'stream_video_codec', 'stream_audio_codec', 'supports_brightness', 'supports_color_temp', 'supports_color', 'color_mode', 'color', 'mode_label', 'fan_label', 'mode', 'target_temp', 'temperature', 'fan_level', 'brightness', 'color_temp', 'position', 'target_position'].concat(meterOrder).concat(otherOrder));
    const labels = {
        power_w: '功率',
        voltage_v: '电压',
        current_a: '电流',
        energy_wh: '电量',
        humidity: '湿度',
        app_version: '应用版本',
        hw_version: '硬件版本'
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
            n = v ? '是' : '否';
        }
        return '<span>' + esc(labels[k] || k) + ' ' + esc(String(n)) + (units[k] || '') + '</span>';
    }).join('') + '</div>';
}

function climateSummary(p) {
    const pr = p.props || {};
    if (pr.card_summary) return '<div class="card-summary">' + esc(String(pr.card_summary)) + '</div>';
    const bits = [];
    if (p.type === 'air-conditioner') {
        if (pr.mode_label) bits.push(pr.mode_label);
        if (pr.target_temp != null) bits.push('设定 ' + pr.target_temp + '°C');
        if (pr.fan_label) bits.push(pr.fan_label);
        if (pr.temperature != null) bits.push('室内 ' + pr.temperature + '°C');
    } else if (p.type === 'fridge') {
        if (pr.cold_temp != null) bits.push('冷藏 ' + pr.cold_temp + '°C');
        if (pr.freeze_temp != null) bits.push('冷冻 ' + pr.freeze_temp + '°C');
    } else if (p.type === 'water_purifier') {
        if (pr.out_tds != null) bits.push('出水水质 TDS ' + pr.out_tds);
        if (pr.filter_ro_life != null) bits.push('RO滤芯约剩' + pr.filter_ro_life + '个月');
    } else if (p.type === 'water_heater') {
        if (pr.temperature != null) bits.push('当前水温 ' + pr.temperature + '°C');
    }
    return bits.length ? '<div class="card-summary">' + esc(bits.join(' · ')) + '</div>' : '';
}

function isCoverType(type) {
    return type === 'cover' || type === 'curtain' || type === 'airer';
}

function coverSummary(p) {
    if (!isCoverType(p.type)) return '';
    const pct = parseInt(p.value, 10);
    const s = Number.isFinite(pct) ? ('开合 ' + pct + '%') : '';
    return s ? '<div class="card-summary">' + esc(s) + '</div>' : '';
}

function batterySummaryBit(p) {
    const b = (p.props || {}).battery;
    return (b != null && b !== '') ? ('电量 ' + b + '%') : '';
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
    if (pr.stream_video_codec != null) bits.push('视频编码 ' + pr.stream_video_codec);
    if (pr.stream_audio_codec != null) bits.push('音频编码 ' + pr.stream_audio_codec);
    return bits.length ? ('码流 · ' + bits.join(' · ')) : '';
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
    if (pr.fault != null && Number(pr.fault) !== 0) bits.push('故障' + pr.fault);
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
    if (v === 'detected') return '有人移动';
    if (v === 'clear') return '无人';
    if (v === 'open' || v === 'left_open') return '打开';
    if (v === 'closed') return '关闭';
    if (p && p.type === 'button') return (newv && newv !== 'idle') ? String(newv) : '按键';
    if (oldv != null && newv != null) return esc(String(oldv)) + ' → ' + esc(String(newv));
    return esc(String(v || '事件'));
}

function fmtDeviceLogTime(ts) {
    const n = Number(ts);
    if (!Number.isFinite(n) || n <= 0) return '—';

    if (n >= 1577836800000) {
        const d = new Date(n);
        const p = x => String(x).padStart(2, '0');
        return d.getFullYear() + '-' + p(d.getMonth() + 1) + '-' + p(d.getDate()) + ' ' + p(d.getHours()) + ':' + p(d.getMinutes()) + ':' + p(d.getSeconds());
    }
    const sec = Math.floor(n / 1000);
    const h = Math.floor(sec / 3600),
        m = Math.floor((sec % 3600) / 60),
        s = sec % 60;
    const p = x => String(x).padStart(2, '0');
    return '运行 ' + h + ':' + p(m) + ':' + p(s);
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
        list.innerHTML = '<div class="modal-hint">暂无记录（本地仅在 Hub 在线期间的状态变化；云端需模块上报历史）</div>';
        return;
    }
    list.innerHTML = rows.map(r => {
        const lab = esc(r.label || '');
        const src = r.source === 'cloud' ? '云端' : (r.source === 'local' ? '本地' : '');
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
        const lab = it.label || '事件';
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
    if (title) title.textContent = (pointDisplayName(p) || id) + ' · 事件记录';
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
            if (hint) hint.textContent = '无适配器，仅显示本地记录';
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
                if (j2.cloud_error) hint.textContent = '云端：' + j2.cloud_error + (j2.message ? (' · ' + j2.message) : '');
                else if (!cloud.length) hint.textContent = '云端暂无补充记录';
                else hint.textContent = '';
            }
        } else if (hint) {
            hint.textContent = '云端日志请求失败';
        }
    } catch (e) {
        if (hint) hint.textContent = '云端日志不可用';
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
        ? '<button class="btn-sm" type="button" style="pointer-events:auto;position:relative;z-index:2" onclick="event.preventDefault();event.stopPropagation();cancelMatterCommission(\'' + String((p.props && p.props.node_id) || '') + '\')">取消配网</button>'
        : '';
    const delBtn = (p.adapter === 'matter') ? '<button class="btn-sm bad" type="button" style="pointer-events:auto;position:relative;z-index:2" onclick="event.preventDefault();event.stopPropagation();deleteMatterDevice(\'' + pidJs + '\')">删除</button>' : (p.adapter === 'zigbee') ? '<button class="btn-sm bad" type="button" style="pointer-events:auto;position:relative;z-index:2" onclick="event.preventDefault();event.stopPropagation();deleteZigbeeDevice(\'' + pidJs + '\')">删除</button>' : (p.adapter === 'ble_mesh') ? '<button class="btn-sm bad" type="button" style="pointer-events:auto;position:relative;z-index:2" onclick="event.preventDefault();event.stopPropagation();deleteBleMeshDevice(\'' + pidJs + '\')">删除</button>' : '';
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
        if (cPhase === 'waiting' || (commissioning && cPhase !== 'failed')) return matterCommissionLabel(p, cPhase) || '等待配网中';
        if (cPhase === 'failed') return '配网失败';
        if (p.type === 'button') return (p.online ? '就绪' : '离线');
        if (isCover && Number.isFinite(pct)) return pct + '%';
        if (p.value === 'detected') return '有人';
        if (p.value === 'clear') return '无人';
        if (p.value === 'closed') return '关闭';
        if (p.value === 'open' && p.type === 'binary_sensor') return '打开';
        if (p.value === 'online' || p.value === 'offline') return p.value === 'online' ? '在线' : '离线';
        if (noCtrl && (p.value === 'on' || p.value === 'off' || powerVal === 'on' || powerVal === 'off')) return on ? '合闸' : '分闸';
        if (p.value === 'on' || p.value === 'off' || powerVal === 'on' || powerVal === 'off') return on ? '开' : '关';
        if (hasParams && (powerVal != null || p.value === 'on' || p.value === 'off')) return on ? '开' : '关';
        if (p.value === 'unknown' || p.value === 'idle') return p.online ? '—' : '离线';
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
    else if (cPhase === 'failed') onlineLab = '配网失败' + (proto ? (' · ' + esc(proto)) : '');
    else onlineLab = (p.online ? '在线' : '离线') + (proto ? (' · ' + esc(proto)) : '');
    const valCls = cPhase === 'waiting' ? 'pending' : (cPhase === 'failed' ? 'off' : (on ? 'on' : 'off'));
    const cardClickCls = commissioning ? '' : clickCls;
    return '<div class="dcard' + cardClickCls + ((p.online || commissioning) ? '' : ' offline') + (commissioning ? ' commissioning' : '') + '"' + clickAttr + '><div class="card-top">' + iconHtml + '<div class="card-name">' + esc(short) + '</div><span class="badge ' + p.type + '">' + esc((typeof dwTypeDisplay === 'function') ? dwTypeDisplay(p) : (TYPE_LABEL[p.type] || p.type)) + '</span></div>' + '<div class="card-meta"><span>' + onlineLab + '</span>' + (room ? '<span>' + esc(room) + '</span>' : '') + '</div>' + '<div class="card-val ' + valCls + '">' + esc(String(valShow)) + '</div>' + climateSummary(p) + coverSummary(p) + magnetSummary(p) + meterSummary(p) + propsHtml(p) + '<div class="card-actions">' + (showDeviceLogBtn(p) ? '<button class="btn-sm" type="button" onclick="openDeviceLog(\'' + p.point_id + '\',event)">日志</button>' : '') + cancelBtn + delBtn + ctrl + '</div></div>';
}

async function storeAct(kind, id, enabled) {
    const url = kind === 'module' ? '/api/modules' : '/api/packs';
    toast(enabled ? '启用中...' : '禁用中...');
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
        toast(r.ok ? (enabled ? '已启用' : '已禁用') : '失败');
        await loadStoreData();
        await loadAdapters();
        await loadPoints();
    } catch (e) {
        toast('网络错误');
    }
}
async function storeInstall(kind, id) {
    toast('下载安装 ' + id + '...');
    try {
        const r = await fetch('/api/store/install', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify({
                kind,
                id,
                reload: true
            })
        });
        toast(r.ok ? '安装成功' : (r.status === 409 ? '已安装' : '安装失败'));
        await loadStoreData();
        await loadAdapters();
        await loadPoints();
    } catch (e) {
        toast('网络错误');
    }
}
async function storeUpgrade(kind, id) {
    toast('升级 ' + id + '...');
    try {
        const r = await fetch('/api/store/upgrade', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify({
                kind,
                id,
                reload: true
            })
        });
        toast(r.ok ? '升级成功' : (r.status === 409 ? '已是最新' : '升级失败'));
        await loadStoreData();
        await loadAdapters();
        await loadPoints();
    } catch (e) {
        toast('网络错误');
    }
}
async function storeUninstall(kind, id) {
    if (!confirm('确定卸载 ' + id + '？')) return;
    toast('卸载中...');
    try {
        const r = await fetch('/api/store/uninstall', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify({
                kind,
                id,
                reload: true
            })
        });
        toast(r.ok ? '已卸载' : '失败');
        await loadStoreData();
        await loadAdapters();
        await loadPoints();
    } catch (e) {
        toast('网络错误');
    }
}
async function storeDel(kind, id) {
    if (kind === 'module') {
        return storeUninstall('module', id);
    }
    if (!confirm('确定删除 ' + id + '？')) return;
    toast('删除中...');
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
        toast(r.ok ? '已删除' : '失败');
        await loadStoreData();
        await loadAdapters();
        await loadPoints();
    } catch (e) {
        toast('网络错误');
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
    document.getElementById('auth-login-title').textContent = sessionMode ? lbl(mod, 'session', '导入会话') : lbl(mod, 'login_title', lbl(mod, 'login', '登录'));
    document.getElementById('auth-user-label').textContent = lbl(mod, 'username', '账号');
    document.getElementById('auth-pass-label').textContent = lbl(mod, 'password', '密码');
    const hostBlk = document.getElementById('auth-host-block');
    if (hostBlk) {
        const showHost = !sessionMode && isHostLogin(mod);
        hostBlk.hidden = !showHost;
        document.getElementById('auth-host-label').textContent = lbl(mod, 'host', '主机');
        const hi = document.getElementById('auth-host');
        if (hi) {
            hi.placeholder = lbl(mod, 'host_placeholder', '192.168.3.1');
        }
    }
    document.getElementById('auth-keyid-label').textContent = lbl(mod, 'key_id', 'API Key ID');
    document.getElementById('auth-apikey-label').textContent = lbl(mod, 'api_key', 'API Key');
    document.getElementById('auth-ticket-label').textContent = lbl(mod, 'ticket', '验证码');
    document.getElementById('auth-ticket').placeholder = lbl(mod, 'ticket_placeholder', '验证码');
    document.getElementById('auth-captcha-label').textContent = lbl(mod, 'captcha', '图形验证码');
    document.getElementById('auth-send-code').textContent = lbl(mod, 'send_code', '发送验证码');
    document.getElementById('auth-cancel').textContent = lbl(mod, 'cancel', '取消');
    document.getElementById('auth-submit').textContent = sessionMode ? lbl(mod, 'session', '导入会话') : lbl(mod, 'login', '登录');
    document.getElementById('auth-user').hidden = sessionMode;
    document.getElementById('auth-user-label').hidden = sessionMode;
    document.getElementById('auth-pass').hidden = sessionMode;
    document.getElementById('auth-pass-label').hidden = sessionMode;
    document.getElementById('auth-apikey-block').hidden = sessionMode || !isApiKeyLogin(mod);
    document.getElementById('auth-session-block').hidden = !sessionMode;
    document.getElementById('auth-2fa-block').hidden = true;
    document.getElementById('auth-ticket').value = '';
    document.getElementById('auth-login-hint').textContent = sessionMode ? lbl(mod, 'session_hint', '用电脑脚本拿到 token 后粘贴（见模块 references/fetch_wyze_tokens.py）') : '';
    loadAuthDraft(mod, mode);
    const uEl = document.getElementById('auth-user');
    if (uEl) uEl.placeholder = '';
}

function openModuleLogin(id) {
    const mod = authModById(id);
    if (!mod || !hasCredLogin(mod)) {
        toast('该模块无登录表单');
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
        toast('该模块无会话导入');
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

function revealAuthChallenge(opts) {
    opts = opts || {};
    const two = document.getElementById('auth-2fa-block');
    two.hidden = false;
    if (opts.captcha_b64) {
        showAuthCaptcha(opts.captcha_b64, opts.captcha_mime);
    } else if (opts.showCaptcha) {
        document.getElementById('auth-captcha-block').hidden = false;
    }
    if (opts.hideCaptcha) {
        document.getElementById('auth-captcha-block').hidden = true;
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
    const focusId = opts.focus || (opts.captcha_b64 || opts.showCaptcha ? 'auth-icode' : 'auth-ticket');
    const el = document.getElementById(focusId);
    if (el) try {
        el.focus();
    } catch (e) {}
}
async function sendAuthVerify() {
    toast('发送验证码...');
    const hint = document.getElementById('auth-login-hint');
    const icodeRaw = document.getElementById('auth-icode').value.trim().replace(/[^0-9A-Za-z]/g, '');
    document.getElementById('auth-icode').value = icodeRaw;
    if (icodeRaw) {
        hint.textContent = '提交图形码: ' + icodeRaw;
    }
    try {
        const r = await moduleRpc(activeAuthModule, 'verify_send', icodeRaw ? {
            icode: icodeRaw
        } : {});
        const j = r.json;
        if (j && j.icode_used) {
            hint.textContent = 'Hub收到图形码: ' + j.icode_used + (j.result === 'ok' ? '，短信已发送' : '');
        }
        if (j && j.result === 'captcha' && j.captcha_b64) {
            revealAuthChallenge({
                captcha_b64: j.captcha_b64,
                captcha_mime: j.captcha_mime,
                hint: (j.icode_used ? '图形码 "' + j.icode_used + '" 错误，' : '') + (j.message || j.hint || '请按新图输入后再次发送'),
                focus: 'auth-icode'
            });
            document.getElementById('auth-icode').value = '';
            toast('需要图形验证码');
            return;
        }
        if (j && j.captcha_b64) {
            revealAuthChallenge({
                captcha_b64: j.captcha_b64,
                captcha_mime: j.captcha_mime,
                hint: j.hint || j.message || '请输入图形验证码后再次发送',
                focus: 'auth-icode'
            });
            document.getElementById('auth-icode').value = '';
            toast('需要图形验证码');
            return;
        }
        toast(r.ok && j && j.result === 'ok' ? '验证码已发送' : '发送失败');
        if (r.ok && j && j.result === 'ok') {
            document.getElementById('auth-captcha-block').hidden = true;
            revealAuthChallenge({
                hideCaptcha: true,
                hint: j.hint || '短信已发送，请填写验证码后点登录',
                focus: 'auth-ticket'
            });
        }
        if (!r.ok || !j || j.result !== 'ok') {
            if (!j || !j.icode_used) hint.textContent = (j && j.message) || (j && j.error) || '发送失败';
        }
    } catch (e) {
        toast('网络错误');
    }
}
async function submitAuthLogin() {
    const user = document.getElementById('auth-user').value.trim();
    const pass = document.getElementById('auth-pass').value;
    const ticket = document.getElementById('auth-ticket').value.trim();
    const hint = document.getElementById('auth-login-hint');
    const twoFaVisible = !document.getElementById('auth-2fa-block').hidden;
    if (ticket && twoFaVisible) {
        toast('验证中...');
        try {
            const r = await moduleRpc(activeAuthModule, 'verify', {
                ticket
            });
            const j = r.json;
            if (r.ok && j && j.result === 'ok') {
                closeAuthLogin();
                loadMeta();
                let m = '登录成功';
                if (j.sync === 'pending') {
                    pollAuthSyncWhileBusy();
                    toast(m + ' · 正在同步设备…');
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
                                toast(m + ' · 请选择家庭');
                                openHomePicker(activeAuthModule);
                            } else if (sj.sync === 'ok') {
                                let sm = m + ' · 已同步 ' + (sj.synced || 0) + ' 台';
                                if (sj.lan_tokens > 0) sm += ' · ' + sj.lan_tokens + ' 个可局域网控制';
                                toast(sm);
                            }
                            return;
                        }
                    }
                    await loadMeta();
                    await loadPoints();
                    if (view === 'overview') renderOverview();
                    toast(m + ' · 同步仍在进行');
                    return;
                }
                await loadPoints();
                if (j.sync === 'need_home') {
                    toast(m + ' · 请选择家庭');
                    openHomePicker(activeAuthModule);
                    return;
                }
                if (j.sync === 'ok' && j.synced != null) {
                    m += ' · 已同步 ' + j.synced + ' 台';
                    if (j.lan_tokens > 0) m += ' · ' + j.lan_tokens + ' 个可局域网控制';
                }
                toast(m);
                return;
            }
            hint.textContent = j.message || j.error || '验证失败';
            toast('验证失败');
            return;
        } catch (e) {
            toast('网络错误');
            return;
        }
    }
    const sessionMode = !document.getElementById('auth-session-block').hidden;
    if (sessionMode) {
        const access = document.getElementById('auth-access').value.trim();
        const refresh = document.getElementById('auth-refresh').value.trim();
        const phoneId = document.getElementById('auth-phoneid').value.trim();
        if (!access || !refresh) {
            hint.textContent = '请输入 access_token 与 refresh_token';
            return;
        }
        toast('导入会话...');
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
                pollAuthSyncWhileBusy();
                await loadPoints();
                let m = '会话已导入';
                if (j.sync === 'need_home') {
                    toast(m + ' · 请选择家庭');
                    openHomePicker(activeAuthModule);
                    return;
                }
                if (j.sync === 'ok' && j.synced != null) m += ' · 已同步 ' + j.synced + ' 台';
                toast(m);
                return;
            }
            hint.textContent = (j && j.message) || (j && j.error) || '导入失败';
            toast('导入失败');
        } catch (e) {
            toast('网络错误');
        }
        return;
    }
    if (!user || !pass) {
        hint.textContent = '请输入账号和密码';
        return;
    }
    const keyId = document.getElementById('auth-keyid').value.trim();
    const apiKey = document.getElementById('auth-apikey').value.trim();
    const mod = authModById(activeAuthModule);
    if (isApiKeyLogin(mod) && (!keyId || !apiKey)) {
        hint.textContent = '请填写 API Key ID 与 API Key';
        return;
    }
    toast('登录中...');
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
            pollAuthSyncWhileBusy();
            await loadPoints();
            let m = '登录成功';
            if (j.sync === 'need_home') {
                toast(m + ' · 请选择家庭');
                openHomePicker(activeAuthModule);
                return;
            }
            if (j.sync === 'ok' && j.synced != null) {
                m += ' · 已同步 ' + j.synced + ' 台';
                if (j.lan_tokens > 0) m += ' · ' + j.lan_tokens + ' 个可局域网控制';
            }
            toast(m);
            return;
        }
        if (j && j.result === 'captcha') {
            revealAuthChallenge({
                captcha_b64: j.captcha_b64,
                captcha_mime: j.captcha_mime,
                showCaptcha: !j.captcha_b64,
                hint: j.hint || j.message || '请输入图形验证码后点击发送验证码',
                focus: 'auth-icode'
            });
            toast('需要图形验证码');
            return;
        }
        if (j && (j.result === 'verify' || j.error === 'ESP_ERR_INVALID_STATE')) {
            if (j.code_sent) {
                revealAuthChallenge({
                    hideCaptcha: true,
                    hint: j.hint || '短信已发送，请填写验证码后点登录',
                    focus: 'auth-ticket'
                });
                toast('验证码已发送');
                return;
            }
            revealAuthChallenge({
                hideCaptcha: true,
                hint: '正在发送验证码…',
                focus: 'auth-ticket'
            });
            toast('需要二次验证');
            await sendAuthVerify();
            return;
        }
        hint.textContent = (j && j.message) || (j && j.error) || '登录失败';
        toast('登录失败');
    } catch (e) {
        toast('网络错误');
    }
}
async function logoutModule(id) {
    if (!confirm('确定退出云账号？')) return;
    toast('退出中...');
    try {
        const r = await moduleRpc(id || activeAuthModule, 'logout');
        const j = r.json;
        if (r.ok && j && j.result === 'ok') {
            await loadMeta();
            await loadPoints();
            toast('已退出');
        } else {
            toast('退出失败');
        }
    } catch (e) {
        toast('网络错误');
    }
}
async function syncModule(id, homeId) {
    const mid = id || activeAuthModule;
    pollAuthSyncWhileBusy();
    toast(homeId ? '切换家庭中…' : '同步中...');
    try {
        const params = {
            merge: true,
            reload: true
        };
        if (homeId) params.home_id = String(homeId);
        const r = await moduleRpc(mid, 'sync', params);
        const j = r.json;
        if (r.ok && j && j.result === 'need_home') {
            toast('请选择家庭');
            openHomePicker(mid);
            return;
        }
        if (r.ok && j && (j.result === 'pending' || j.syncing)) {
            toast(j.message || (homeId ? '正在切换家庭…' : '正在同步设备…'));
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
                toast('请选择家庭');
                openHomePicker(mid);
                return;
            }
            if (last && last.sync === 'ok') {
                let m = (last.sync_home_name ? ('已切换到 ' + last.sync_home_name + ' · ') : '已同步 ') +
                    (last.synced || 0) + ' 台';
                if (last.lan_tokens > 0) m += ' · ' + last.lan_tokens + ' 个可局域网控制';
                toast(m);
                return;
            }
            if (last && last.syncing) {
                toast('同步仍在进行，请稍候刷新');
                return;
            }
            toast((last && last.sync_error) || (last && last.message) || '同步失败');
            return;
        }
        if (r.ok && j && j.result === 'ok') {
            let m = (j.home_name ? ('已切换到 ' + j.home_name + ' · ') : '已同步 ') + (j.synced || 0) + ' 台';
            if (j.lan_tokens > 0) m += ' · ' + j.lan_tokens + ' 个可局域网控制';
            toast(m);
        } else toast(j && j.message || j && j.error || '同步失败');
        await loadMeta();
        await loadPoints();
        await loadAdapters();
        if (view === 'overview') renderOverview();
    } catch (e) {
        toast('失败');
    }
}

function closeHomePicker() {
    const m = document.getElementById('home-picker-modal');
    if (m) m.hidden = true;
}
async function openHomePicker(id) {
    if (!id) {
        toast('未指定模块');
        return;
    }
    homePickerModule = id;
    homePickerSelected = '';
    const mod = authModById(homePickerModule) || modules.find(x => x.id === homePickerModule);
    const title = document.getElementById('home-picker-title');
    const hint = document.getElementById('home-picker-hint');
    if (title) title.textContent = lbl(mod, 'home_picker_title', '选择家庭');
    if (hint) hint.textContent = lbl(mod, 'home_picker_hint', '点选家庭后点下方按钮，或直接点「切换到此家庭」');
    const conf = document.getElementById('home-picker-confirm');
    if (conf) {
        conf.textContent = lbl(mod, 'home_picker_confirm', '同步此家庭');
        conf.disabled = true;
    }
    const listEl = document.getElementById('home-picker-list');
    if (listEl) listEl.innerHTML = '<div class="modal-hint">加载中…</div>';
    document.getElementById('home-picker-modal').hidden = false;
    toast('加载家庭…');
    try {
        const r = await moduleRpc(homePickerModule, 'homes');
        const j = r.json;
        const homes = (j && j.homes) || [];
        if (!r.ok || !j || j.result !== 'ok') {
            const msg = esc((j && j.message) || (j && j.error) || '加载失败');
            const needLogin = !!(j && j.need_login) || /过期|未登录/.test((j && j.message) || '');
            if (listEl) listEl.innerHTML = '<div class="modal-hint">' + msg +
                (needLogin ? '<div style="margin-top:12px"><button class="btn-sm on" type="button" id="home-picker-relogin">重新登录</button></div>' : '') +
                '</div>';
            if (needLogin) {
                const btn = document.getElementById('home-picker-relogin');
                if (btn) btn.onclick = () => {
                    closeHomePicker();
                    openModuleLogin(homePickerModule);
                };
            }
            toast(needLogin ? '登录已过期' : '加载家庭失败');
            return;
        }
        if (!homes.length) {
            if (listEl) listEl.innerHTML = '<div class="modal-hint">没有可用家庭</div>';
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
                    '<div style="flex:1"><div style="font-weight:600">' + esc(h.name || id) + (on ? ' · 当前' : '') + '</div>' +
                    '<div class="meta">' + (h.room_count || 0) + ' 个房间 · ' + (h.device_count || 0) + ' 台设备</div></div>' +
                    '<button type="button" class="btn-sm' + (on ? '' : ' on') + '" data-home-switch="' + escAttr(id) + '">' +
                    (on ? '重新同步' : '切换到此家庭') + '</button></div>';
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
        if (listEl) listEl.innerHTML = '<div class="modal-hint">网络错误</div>';
        toast('网络错误');
    }
}
async function confirmHomePicker() {
    const checked = document.querySelector('#home-picker-list .home-item.on');
    const hid = (checked && checked.getAttribute('data-home-id')) || homePickerSelected;
    if (!hid) {
        toast('请选择一个家庭');
        return;
    }
    const mid = homePickerModule;
    if (!mid) {
        toast('未指定模块');
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
            ? '经 BLE + DNS-SD 配网，并写入设备 STA 的 SSID 与密码。'
            : '经 BLE + DNS-SD 配网，并写入 Hub 的 Thread 网络数据集。';
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
    document.getElementById('matter-wifi-pass').placeholder = pref.has_password ? '已保存密码，可改写或留空沿用' : 'Wi-Fi 密码';
    const submitBtn = document.getElementById('matter-add-submit');
    if (submitBtn) submitBtn.disabled = false;
    refreshMatterAddUi();
    document.getElementById('matter-add-modal').hidden = false;
    document.getElementById('matter-code').focus();
}

function closeMatterAdd() {
    document.getElementById('matter-add-modal').hidden = true;
}

const MATTER_STAGE_LABEL = { discovering: '设备发现中', binding: '设备绑定中', activating: '设备激活中' };
const MATTER_STAGE_STEP = { discovering: 1, binding: 2, activating: 3 };
const MATTER_STAGE_TOTAL = 3;

/* A waiting row is only "配网中" while the Hub is actually running that node;
 * the live stage comes from /api/matter/status, not from adapters.json. */
function matterCommissionLabel(p, cPhase) {
    if (cPhase !== 'waiting') return '';
    const st = (typeof matterStatus === 'undefined') ? null : matterStatus;
    const running = st && st.commission === 'running' && st.commission_node_id != null &&
        String(st.commission_node_id) === String((p.props || {}).node_id);
    if (!running) return '等待配网中';
    const step = MATTER_STAGE_STEP[st.commission_stage];
    return '配网中 · ' + (MATTER_STAGE_LABEL[st.commission_stage] || '进行中') +
        (step ? ('（步骤 ' + step + '/' + MATTER_STAGE_TOTAL + '）') : '');
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
                toast((MATTER_STAGE_LABEL[stage] || stage) + '… node ' + (st.commission_node_id || nodeId));
            }
            if (st.commission === 'ok') {
                await loadMeta();
                await loadPoints();
                toast('配网成功 node ' + (st.commission_node_id || nodeId));
                return;
            }
            if (st.commission === 'failed') {
                await loadPoints();
                toast('配网失败' + (lastStage ? ('（' + (MATTER_STAGE_LABEL[lastStage] || lastStage) + '）') : '') +
                    ': ' + String(st.last_error || st.commission_result || '').slice(0, 80));
                return;
            }
        } catch (e) {
            /* keep polling */
        }
    }
    await loadPoints();
    toast('配网超时');
}

async function cancelMatterCommission(nodeId) {
    const nid = (nodeId != null && nodeId !== '')
        ? String(nodeId)
        : (matterStatus && matterStatus.commission_node_id != null
            ? String(matterStatus.commission_node_id) : null);
    if (!nid) {
        toast('当前没有进行中的配网');
        return;
    }
    try {
        const r = await fetch('/api/matter/commission', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ node_id: nid, cancel: true })
        });
        const j = await r.json().catch(() => ({}));
        toast('已发送取消配网：' + (j.status || (r.ok ? 'ok' : '失败')));
    } catch (e) {
        toast('取消配网请求失败');
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
        hint.textContent = '请输入 pairing_code';
        return;
    }
    if (discovery === 'ble_wifi' && !ssid) {
        hint.hidden = false;
        hint.textContent = '请填写 STA SSID';
        return;
    }
    toast('正在添加…');
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
            toast('已添加 node ' + j.node_id);
            return;
        }
        if (r.ok && j.result === 'ok' && j.commission === 'started') {
            closeMatterAdd();
            await loadMeta();
            await loadPoints();
            toast('已添加，等待配网中…');
            pollMatterCommissionCard(j.node_id);
            return;
        }
        /* Start failed after eager upsert — card may show 配网失败. */
        closeMatterAdd();
        await loadPoints();
        const msg = j.message || j.commission || j.error || ('HTTP ' + r.status);
        toast('配网失败: ' + String(msg).slice(0, 80));
    } catch (e) {
        toast('网络错误');
    } finally {
        if (submitBtn) submitBtn.disabled = false;
    }
}
async function deleteMatterDevice(pointId) {
    if (!confirm('确定删除 ' + pointId + '？')) return;
    toast('删除中...');
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
            toast('已删除');
        } else {
            toast('删除失败: ' + String(j.message || j.error || ('HTTP ' + r.status)).slice(0, 60));
        }
    } catch (e) {
        toast('网络错误');
    }
}
async function deleteZigbeeDevice(pointId) {
    if (!confirm('确定删除 ' + pointId + '？\n（Hub 侧移除；设备需自行离网/长按 BOOT）')) return;
    toast('删除中...');
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
            toast('已删除');
        } else {
            toast('删除失败');
        }
    } catch (e) {
        toast('网络错误');
    }
}
async function deleteBleMeshDevice(pointId) {
    if (!confirm('确定删除 ' + pointId + '？')) return;
    toast('删除中...');
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
            toast('已删除');
        } else {
            toast('删除失败');
        }
    } catch (e) {
        toast('网络错误');
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
const EXTRA_LABEL = {
    indicator: '指示灯',
    motion_detection: '移动侦测',
    human_tracking: '人形联动追踪',
    time_watermark: '时间水印',
    wdr: '宽动态',
    dnd: '勿扰'
};

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
        const lab = EXTRA_LABEL[e.key] || e.key;
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

function plugMeterText(p) {
    const pr = (p && p.props) || {};
    const w = pr.power_w != null ? pr.power_w : pr.power;
    const wh = pr.energy_wh != null ? pr.energy_wh : pr.energy;
    const bits = [];
    if (w != null && w !== '') bits.push('功率 ' + Number(w) + ' W');
    if (wh != null && wh !== '') bits.push('电量 ' + Number(wh) + (pr.energy_wh != null ? ' Wh' : ' kWh'));
    return bits.length ? bits.join(' · ') : '功率 — · 电量 —';
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
    document.getElementById('plug-detail-meta').textContent = p.point_id + (p.online ? ' · 在线' : ' · 离线');
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
    meter.hidden = mt.indexOf('—') >= 0 && (p.props || {}).power_w == null && (p.props || {}).energy_wh == null && (p.props || {}).power == null && (p.props || {}).energy == null;
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
            ph.innerHTML = '无法预览<div class="cam-preview-sub">缺少模块适配器</div>';
        }
        if (img) img.hidden = true;
        return;
    }
    if (ph) {
        ph.hidden = false;
        ph.innerHTML = '加载预览…<div class="cam-preview-sub">最近抓拍（非实时）</div>';
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
            ph.innerHTML = '暂无抓拍<div class="cam-preview-sub">模块无可用画面</div>';
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
    document.getElementById('camera-detail-meta').textContent = p.point_id + (p.online ? ' · 在线' : ' · 离线');
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
    document.getElementById('speaker-detail-meta').textContent = p.point_id + (p.online ? ' · 在线' : ' · 离线');
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
        toast('请输入文字');
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
        toast('请输入文字');
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
    document.getElementById('appliance-detail-meta').textContent = ((typeof dwTypeDisplay === 'function') ? dwTypeDisplay(p) : (TYPE_LABEL[p.type] || p.type)) + (p.online ? ' · 在线' : ' · 离线');
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
        box.innerHTML = '<div class="modal-hint">暂无更多状态，请稍后在模块里点同步</div>';
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
    document.getElementById('ac-detail-meta').textContent = p.point_id + (p.online ? ' · 在线' : ' · 离线');
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
    const amb = pr.temperature != null ? ('室温 ' + pr.temperature + '°') : '室温 —';
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
    document.getElementById('cover-detail-meta').textContent = p.point_id + (p.online ? ' · 在线' : ' · 离线');
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
async function cmd(id, c, params) {
    const p = pts.find(x => x.point_id === id);
    if (p && !p.online) {
        toast('设备离线');
        return;
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
            toast('已发送');
        } else {
            let msg = '失败';
            try {
                const j = await r.json();
                msg = (j && (j.error || j.message || j.result)) || ('HTTP ' + r.status);
                if (typeof msg === 'object') msg = JSON.stringify(msg);
            } catch (e) {
                msg = 'HTTP ' + r.status;
            }
            toast(String(msg).slice(0, 120));
        }
        loadPoints();
    } catch (e) {
        toast('网络错误');
    }
}
async function permitZigbeeJoin() {
    if (radioProv.zigbee_permit_join) {
        toast('Zigbee 正在入网，请勿重复开启');
        return;
    }
    if (radioProv.ble_mesh_provisioning || radioProv.ble_mesh_permit_provision) {
        toast('BLE Mesh 正在配网，请稍后再试');
        return;
    }
    toast('正在打开入网窗口…');
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
            toast('Zigbee 入网已开启 120 秒');
        } else {
            toast('开启失败' + (j.error ? ': ' + j.error : (j.result ? ': ' + j.result : '')));
        }
    } catch (e) {
        toast('网络错误');
    }
}
async function permitBleMeshProvision() {
    if (radioProv.ble_mesh_permit_provision || radioProv.ble_mesh_provisioning) {
        toast('BLE Mesh 正在配网，请勿重复开启');
        return;
    }
    if (radioProv.zigbee_permit_join) {
        toast('Zigbee 正在入网，请稍后再试');
        return;
    }
    toast('正在打开配网窗口…');
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
            toast('BLE Mesh 配网已开启 120 秒');
            loadPoints();
        } else {
            toast('开启失败' + (j.error ? ': ' + j.error : (j.result ? ': ' + j.result : '')));
        }
    } catch (e) {
        toast('网络错误');
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
    toast('正在关闭入网窗口…');
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
            toast('Zigbee 入网已关闭');
        } else {
            toast('关闭失败' + (j.error ? ': ' + j.error : ''));
        }
    } catch (e) {
        toast('网络错误');
    }
}
async function stopBleMeshProvision() {
    toast('正在关闭配网窗口…');
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
            toast('BLE Mesh 配网窗口已关闭');
        } else {
            toast('关闭失败' + (j.error ? ': ' + j.error : ''));
        }
    } catch (e) {
        toast('网络错误');
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
