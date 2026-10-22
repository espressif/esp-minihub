/*
 * device_widgets.js - data-driven device control surface.
 *
 * HA-like mapping (no per-type C structs):
 *   MiniHub type (e.g. light)     ≈ HA domain
 *   parameters.csv + widget API   ≈ EntityDescription defaults
 *   plugin devices.json           ≈ params inside the device point's props
 *   live visible param            ≈ supported_features (brightness → slider)
 *   instance card (one device)    ≈ HA device (one point, params in JSON)
 *
 * Display is looked up dynamically:
 *   1. Live instances from GET /api/points — preferred shape is one point per
 *      device: point_id = <protocol>.<endpoint>, type = data-model type,
 *      props.params.<name> = { value, access?, visible?, min?, max?, ... }.
 *      Legacy <protocol>.<endpoint>.<parameter> points are still grouped.
 *      Ids that are not <protocol>.<device> (switch/bm_000a) stay one card;
 *      the slash prefix is not treated as a protocol or adapter.
 *   2. Type catalog from GET /api/device_widgets (labels, widgets, bounds,
 *      device avatar URL). Parameter controls are CSS classes in
 *      device_widgets.css — not /img/parameters mockups.
 * The UI intersects them: only registered, visible params of a live instance
 * are shown. Unreported catalog names stay off the card and the detail.
 * label/widget come from the catalog keyed by (type, parameter).
 * min/max/step/unit/access default from the catalog style; instance param
 * props override those fields when present (devices.json need not copy the
 * whole style blob).
 *
 * Acting on a control POSTs to /api/devices/control with the data-model TYPE
 * as `device`, plus the live `point_id` so multiple instances of the same type
 * stay distinct. Backend prefers that point_id when it exists and matches the
 * type; otherwise falls back to minihub_point_resolve_by_type_param. The
 * `parameter` name is passed in the command so the adapter updates props.params.
 *
 * Device-class avatars (devices/*.svg) appear in the card header.
 * Param rows are label-first; sliders use a refined mid-weight bar
 * (soft fill or spectrum track + small thumb); drag + POST unchanged.
 */

let dwCatalog = null;

let dwInstances = [];

let deviceDetailId = null;

let deviceDetailPanel = 'controls';

let dwExpandedKeys = Object.create(null);

const DW_PRIMARY_PARAM = 'power';

/* Catalog key order is not guaranteed (may arrive sorted); media rows keep
 * the canonical remote layout: mute, play, prev, next (play before prev). */
const DW_MEDIA_RANK = { mute: 0, unmute: 0, play_pause: 1, play_resume: 1, play_prev: 2, play_next: 3 };

function dwMediaRank(pname) {
    return Object.prototype.hasOwnProperty.call(DW_MEDIA_RANK, pname)
        ? DW_MEDIA_RANK[pname] : 50;
}

function dwParseStyle(style) {
    try {
        return style ? JSON.parse(style) : {};
    } catch (e) {
        return {};
    }
}

function dwIsCorruptParam(p) {
    if (!p) return true;
    const id = String(p.parameter || '');
    if (!id || id.indexOf('/') >= 0 || id.indexOf('{') >= 0) return true;
    return false;
}

function dwLabel(p) {
    const s = dwParseStyle(p.style);
    const raw = (s.label || p.label || p.parameter || '').trim();
    if (!raw || raw.indexOf('/') >= 0 || raw.indexOf('{') >= 0) {
        return p.parameter || '';
    }
    return raw;
}

function dwWidget(p) {
    let w = '';
    if (p && p.widget) w = String(p.widget);
    if (!w) {
        const s = dwParseStyle(p && p.style);
        w = s.widget ? String(s.widget) : '';
    }

    if (w === 'hue-slider' || w === 'hue_slider' || w === 'hue-circle' ||
        w === 'hue_circle') {
        w = 'slider';
    }
    return w;
}

function dwUiToWidget(ui) {
    const s = String(ui || '').trim();
    if (!s) return '';
    return s.indexOf('ui.') === 0 ? s.slice(3).replace(/-/g, '_') : s;
}

function dwIconUrl(icon) {
    const s = String(icon || '').trim();
    if (!s) return '';
    if (s.indexOf('./images/') === 0) return '/img/' + s.slice('./images/'.length);
    return s;
}

function dwCssClass(css) {
    const s = String(css || '').trim();
    if (!s) return '';
    return s.split(/\s+/).filter(function (c) {
        return /^[A-Za-z0-9][A-Za-z0-9_-]*$/.test(c);
    }).join(' ');
}

function dwCatalogIndex() {
    const map = Object.create(null);
    if (!dwCatalog || !Array.isArray(dwCatalog.devices)) return map;
    dwCatalog.devices.forEach(function (d) {
        if (!d || !d.device) return;
        const params = Object.create(null);
        (d.params || []).forEach(function (p) {
            if (dwIsCorruptParam(p)) return;
            params[p.parameter] = p;
        });
        map[d.device] = { type: d.device, image: d.image || '', params: params };
    });
    return map;
}

function dwLiveFromParamObj(name, ent, pointId) {
    const pr = (ent && typeof ent === 'object') ? ent : {};
    return {
        name: name,
        value: pr.value != null ? String(pr.value) : '',
        visible: !(pr.visible === false),
        access: (pr.access != null) ? String(pr.access) : null,
        min: (pr.min != null) ? pr.min : null,
        max: (pr.max != null) ? pr.max : null,
        step: (pr.step != null) ? pr.step : null,
        unit: (pr.unit != null && pr.unit !== '') ? String(pr.unit) : null,
        requires: dwNormalizeRequires(pr.requires),

        icon: (pr.icon != null && pr.icon !== '') ? String(pr.icon) : null,
        css: (pr.css != null && pr.css !== '') ? String(pr.css) : null,
        ui_element: (pr.ui_element != null && pr.ui_element !== '')
            ? String(pr.ui_element) : null,
        point_id: pointId
    };
}

function dwParamsBlobNonEmpty(paramsBlob) {
    if (!paramsBlob || typeof paramsBlob !== 'object') return false;
    if (Array.isArray(paramsBlob)) return paramsBlob.length > 0;
    return Object.keys(paramsBlob).length > 0;
}

function dwIngestParamsBlob(inst, paramsBlob, pointId) {
    if (Array.isArray(paramsBlob)) {
        paramsBlob.forEach(function (ent) {
            if (!ent || !ent.name) return;
            inst.params[ent.name] = dwLiveFromParamObj(ent.name, ent, pointId);
        });
        return;
    }
    Object.keys(paramsBlob).forEach(function (pname) {
        inst.params[pname] = dwLiveFromParamObj(pname, paramsBlob[pname], pointId);
    });
}

/*
 * Build one card per device from /api/points.
 * Preferred: point_id <protocol>.<endpoint> + props.params object/array
 *   (protocol may be sim / thread / zigbee / … — catalog join uses type, not prefix).
 * Legacy: <protocol>.<endpoint>.<parameter> points grouped by protocol.endpoint
 *   so thread.bulb.power|brightness|… → one thread.bulb card.
 * Unscoped ids (no dot, e.g. switch/bm_000a) are one instance with an empty
 * protocol — callers must not file them under an adapter derived from '/'.
 */
function dwBuildInstances(points) {
    const byKey = Object.create(null);
    if (!Array.isArray(points)) return [];

    function ensureInst(key, protocol, endpoint, pt) {
        if (!byKey[key]) {
            byKey[key] = {
                key: key,
                protocol: protocol,
                endpoint: endpoint,
                type: pt.type || '',
                online: false,
                point_id: key,
                params: Object.create(null)
            };
        }
        const inst = byKey[key];
        if (!inst.type && pt.type) inst.type = pt.type;
        if (pt.online) inst.online = true;
        if (pt.adapter && !inst.adapter) inst.adapter = pt.adapter;
        if (pt.props && typeof pt.props === 'object') inst.props = pt.props;
        return inst;
    }

    points.forEach(function (pt) {
        if (!pt || typeof pt.point_id !== 'string') return;
        const pr = (pt.props && typeof pt.props === 'object') ? pt.props : null;
        const paramsBlob = pr && pr.params;
        const seg = pt.point_id.split('.');
        const hasParams = dwParamsBlobNonEmpty(paramsBlob);
        const scoped = (typeof pointIdScoped === 'function')
            ? pointIdScoped(pt.point_id)
            : (seg.length >= 2 && !!seg[0] && seg.slice(1).some(function (s) { return !!s; }));

        /* Not <protocol>.<device>: one card. Protocol stays empty (slash is not an adapter). */
        if (!scoped) {
            if (!pt.point_id) return;
            const tail = pt.point_id.split('/').filter(function (s) { return !!s; });
            const endpoint = tail.length ? tail[tail.length - 1] : pt.point_id;
            const inst = ensureInst(pt.point_id, '', endpoint, pt);
            inst.point_id = pt.point_id;
            if (hasParams) dwIngestParamsBlob(inst, paramsBlob, pt.point_id);
            if (!Object.keys(inst.params).length && pt.value != null && String(pt.value) !== '') {
                inst.params.power = {
                    name: 'power',
                    value: String(pt.value),
                    visible: true,
                    access: (pr && pr.access) ? String(pr.access) : null,
                    min: null,
                    max: null,
                    step: null,
                    unit: null,
                    point_id: pt.point_id
                };
            }
            return;
        }

        /*
         * Device-level: non-empty props.params, or a 2-segment id (sim.js_kitchen).
         * Do NOT treat 3-segment ids with an empty params {} as device-level —
         * that used to create per-param cards named "power"/"brightness" with
         * no controls (Devices looked empty while Overview still listed rows).
         */
        if (hasParams || seg.length === 2) {
            let endpoint;
            let protocol;
            let key;
            if (seg.length === 2) {
                protocol = seg[0];
                endpoint = seg[1];
                key = pt.point_id;
            } else if (hasParams) {

                endpoint = seg[seg.length - 1];
                protocol = seg.slice(0, -1).join('.');
                key = pt.point_id;
            } else {
                endpoint = null;
            }
            if (endpoint) {
                const inst = ensureInst(key, protocol, endpoint, pt);
                inst.point_id = pt.point_id;
                if (hasParams) dwIngestParamsBlob(inst, paramsBlob, pt.point_id);

                if (!Object.keys(inst.params).length && pt.value != null && String(pt.value) !== '') {
                    inst.params.power = {
                        name: 'power',
                        value: String(pt.value),
                        visible: true,
                        access: (pr && pr.access) ? String(pr.access) : null,
                        min: null,
                        max: null,
                        step: null,
                        unit: null,
                        point_id: pt.point_id
                    };
                }
                return;
            }
        }

        if (seg.length < 3) return;
        const parameter = seg[seg.length - 1];
        const endpoint = seg[seg.length - 2];
        const protocol = seg.slice(0, -2).join('.');
        const key = protocol + '.' + endpoint;
        const inst = ensureInst(key, protocol, endpoint, pt);

        if (parameter === 'power') {
            inst.point_id = pt.point_id;
            inst._has_power = true;
        }
        inst.params[parameter] = {
            name: parameter,
            value: pt.value != null ? String(pt.value) : '',
            visible: !(pr && pr.visible === false),
            access: (pr && pr.access) ? String(pr.access) : null,
            min: (pr && pr.min != null) ? pr.min : null,
            max: (pr && pr.max != null) ? pr.max : null,
            step: (pr && pr.step != null) ? pr.step : null,
            unit: (pr && pr.unit != null && pr.unit !== '') ? String(pr.unit) : null,
            point_id: pt.point_id
        };
    });
    return Object.keys(byKey).map(function (k) { return byKey[k]; })
        .sort(function (a, b) {
            return (a.endpoint || '').localeCompare(b.endpoint || '');
        });
}

async function ensureDwCatalog() {
    if (dwCatalog) return dwCatalog;
    const wr = await fetch(window.mhCatalogUrl('/api/device_widgets'));
    if (!wr.ok) throw new Error('http ' + wr.status);
    dwCatalog = await wr.json();
    return dwCatalog;
}

async function loadDeviceWidgets() {
    const el = document.getElementById('devices-content');
    if (!el) return;
    try {
        const [wr, pr] = await Promise.all([
            fetch(window.mhCatalogUrl('/api/device_widgets')),
            fetch('/api/points')
        ]);
        if (!wr.ok) throw new Error('http ' + wr.status);
        dwCatalog = await wr.json();
        let points = [];
        if (pr.ok) {
            const next = await pr.json();
            if (Array.isArray(next)) points = next;
        }

        if (!points.length && typeof pts !== 'undefined' && Array.isArray(pts) && pts.length) {
            points = pts;
        }
        dwInstances = dwBuildInstances(points);
    } catch (e) {
        el.innerHTML = '<div class="modal-hint">' + t('dw.catalog_load_failed', { error: esc(String(e)) }) + '</div>';
        return;
    }
    renderDeviceWidgets();
}

function dwFindInstance(pointId, points) {
    if (!pointId) return null;
    const list = dwBuildInstances(points != null ? points : (typeof pts !== 'undefined' ? pts : []));
    let inst = list.find(function (i) {
        return i.point_id === pointId || i.key === pointId;
    });
    if (inst) return inst;
    const seg = String(pointId).split('.');
    if (seg.length >= 3) {
        const key = seg.slice(0, -1).join('.');
        inst = list.find(function (i) { return i.key === key; });
    }
    return inst || null;
}

function dwInstanceParamsHtml(inst) {
    if (!inst) return '';
    const byType = dwCatalogIndex();
    const cat = dwCatalogForType(byType, inst.type);
    const built = dwInstanceCardBody(inst, cat);
    if (!built.visibleCount) {
        return '<div class="modal-hint">' + t('dw.no_visible_params') + '</div>';
    }

    return '<div class="dw-params">' + built.primary + built.extra + '</div>';
}

function closeDeviceDetail() {
    const m = document.getElementById('device-detail-modal');
    if (m) m.hidden = true;
    deviceDetailId = null;
    deviceDetailPanel = 'controls';
    dwSyncDetailInfoBtn();
}

function dwSyncDetailInfoBtn() {
    const btn = document.getElementById('device-detail-info-btn');
    if (!btn) return;
    btn.textContent = deviceDetailPanel === 'info' ? t('dw.back') : t('dw.details');
}

function toggleDeviceDetailInfo() {
    if (!deviceDetailId) return;
    deviceDetailPanel = deviceDetailPanel === 'info' ? 'controls' : 'info';
    refreshDeviceDetail();
}

function dwFindModuleForInst(inst) {
    const list = (typeof modules !== 'undefined' && Array.isArray(modules)) ? modules : [];
    if (!inst || !list.length) return null;
    const ad = inst.adapter ? String(inst.adapter) : '';
    const endpoint = inst.endpoint ? String(inst.endpoint) : '';
    let m = ad ? list.find(function (x) {
        return x && (x.adapter === ad || x.id === ad);
    }) : null;
    if (m) return m;
    if (endpoint) {
        m = list.find(function (x) {
            return x && (x.id === endpoint || x.adapter === endpoint);
        });
    }
    return m || null;
}

function dwInfoRowHtml(label, value, opts) {
    opts = opts || {};
    const cls = 'device-info-row' +
        (opts.muted ? ' is-muted' : '') +
        (opts.disabled ? ' is-disabled' : '');
    const empty = (opts.empty != null) ? String(opts.empty) : t('dw.unknown');
    const v = (value != null && String(value) !== '') ? String(value) : empty;
    return '<div class="' + cls + '"><span class="k">' + esc(label) +
        '</span><span class="v">' + esc(v) + '</span></div>';
}

/* Top-level identity props (not catalog params). Empty renders as 未知. */
function dwPropStr(pr, keys) {
    if (!pr || typeof pr !== 'object') return '';
    for (let i = 0; i < keys.length; i++) {
        const v = pr[keys[i]];
        if (v != null && String(v) !== '') return String(v);
    }
    return '';
}

async function dwTriggerDeviceOta(pointId) {
    if (!pointId) return;
    try {
        const r = await fetch('/api/devices/ota', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ point_id: pointId })
        });
        const raw = await r.text().catch(function () { return ''; });
        if (!r.ok) {
            const msg = t('dw.control_failed', { error: mhHttpErrorText(r.status, raw) });
            if (typeof toast === 'function') toast(msg);
            else alert(msg);
            return;
        }
        if (typeof toast === 'function') toast(t('dw.ota_triggered'));
    } catch (e) {
        if (typeof toast === 'function') toast(String(e && e.message ? e.message : e));
        else alert(String(e));
    }
}

function dwInstanceInfoHtml(inst) {
    if (!inst) return '<div class="modal-hint">' + t('dw.point_not_found') + '</div>';
    const points = (typeof pts !== 'undefined' && Array.isArray(pts)) ? pts : [];
    const pt = points.find(function (p) {
        return p && (p.point_id === inst.point_id || p.point_id === inst.key);
    }) || null;
    const pr = (pt && pt.props && typeof pt.props === 'object') ? pt.props : {};
    const typeLab = (typeof dwTypeDisplay === 'function')
        ? dwTypeDisplay({ type: inst.type, props: pr })
        : mhTypeLabel(inst.type);
    const protocol = inst.protocol ||
        (typeof pointProtocol === 'function'
            ? pointProtocol(pt || { point_id: inst.point_id })
            : '') ||
        '';
    const adapter = inst.adapter || (pt && pt.adapter) || '';
    const pid = inst.point_id || inst.key || '';

    const mfr = dwPropStr(pr, ['manufacturer', 'vendor', 'manufacturer_name']);
    const model = dwPropStr(pr, ['model', 'model_id', 'model_identifier']);
    const swVer = dwPropStr(pr, ['sw_version', 'fw_version', 'firmware_version',
        'app_version', 'device_version']);
    const hwVer = dwPropStr(pr, ['hw_version', 'hardware_version']);

    const ota = (pr.ota && typeof pr.ota === 'object') ? pr.ota : null;
    const otaAvail = !!(ota && ota.available === true);
    const otaVer = ota && ota.version != null ? String(ota.version) : '';

    let html = '<div class="device-info-rows">';
    html += dwInfoRowHtml(t('dw.device_id'), pid);
    html += dwInfoRowHtml(t('dw.category'), typeLab || inst.type || '');
    html += dwInfoRowHtml(t('dw.protocol'), protocol);
    html += dwInfoRowHtml(t('dw.adapter'), adapter);
    html += dwInfoRowHtml(t('dw.status'), inst.online ? t('dw.online') : t('dw.offline'));
    html += dwInfoRowHtml(t('dw.manufacturer'), mfr);
    html += dwInfoRowHtml(t('dw.model'), model);
    html += dwInfoRowHtml(t('dw.sw_version'), swVer);
    html += dwInfoRowHtml(t('dw.hw_version'), hwVer);

    if (otaAvail) {
        const hint = otaVer ? t('dw.ota_target', { version: otaVer }) : '';
        html += '<div class="device-info-row"><span class="k">OTA</span><span class="v">' +
            (hint ? esc(hint) + ' · ' : '') +
            '<button class="btn-sm on" type="button" onclick="dwTriggerDeviceOta(\'' +
            String(pid).replace(/\\/g, '\\\\').replace(/'/g, "\\'") +
            '\')">' + t('dw.trigger_ota') + '</button></span></div>';
    } else {
        html += dwInfoRowHtml('OTA', '', { muted: true, disabled: true, empty: '—' });
    }

    const extraRows = Array.isArray(pr.detail_rows) ? pr.detail_rows : [];
    extraRows.forEach(function (r) {
        if (!r || !r.label) return;
        html += dwInfoRowHtml(String(r.label), r.value != null ? String(r.value) : '');
    });
    html += '</div>';
    return html;
}

/* Combined media toggles (mute / play) render their state into the icon and
   the onclick command, and their state params are visible:false — invisible to
   dwPatchDetailReadonly. Patch them in place so an open modal follows state
   while a focused slider blocks the full re-render. */
function dwPatchDetailMediaButtons(inst) {
    const body = document.getElementById('device-detail-body');
    if (!body || !inst || !inst.params) return;
    [['mute', 'muted'], ['unmute', 'muted'],
     ['play_pause', 'playing'], ['play_resume', 'playing']].forEach(function (pair) {
        const row = body.querySelector('.dw-param[data-param="' + pair[0] + '"]');
        if (!row) return;
        const btn = row.querySelector('button.dw-media-btn');
        if (!btn) return;
        const isOn = dwMediaStateOn(inst.params[pair[1]]);
        const isMuteRow = pair[0] === 'mute' || pair[0] === 'unmute';
        const cmd = isMuteRow ? (isOn ? 'unmute' : 'mute')
            : (isOn ? 'play_pause' : 'play_resume');
        const label = isMuteRow
            ? (isOn ? t('dw.unmute') : t('dw.mute'))
            : (isOn ? t('dw.pause') : t('dw.play'));
        const icon = isMuteRow ? (isOn ? '🔇' : '🔈')
            : (isOn ? '⏸' : '▶');
        btn.classList.toggle('is-on', isOn);
        const ic = btn.querySelector('.ic');
        if (ic && ic.textContent !== icon) ic.textContent = icon;
        if (btn.getAttribute('aria-label') !== label) btn.setAttribute('aria-label', label);
        if (btn.getAttribute('title') !== label) btn.setAttribute('title', label);
        const oc = btn.getAttribute('onclick') || '';
        if (oc) {
            const next = oc.replace(/'(play_pause|play_resume|mute|unmute)'/,
                "'" + cmd + "'");
            if (next !== oc) btn.setAttribute('onclick', next);
        }
    });
}

/* Patch read-only param values in an open detail without tearing down focused controls. */
function dwPatchDetailReadonly(inst) {
    const body = document.getElementById('device-detail-body');
    if (!body || !inst || !inst.params) return;
    const byType = dwCatalogIndex();
    const cat = dwCatalogForType(byType, inst.type);
    Object.keys(inst.params).forEach(function (pname) {
        const live = inst.params[pname];
        if (!live || !live.visible) return;
        const p = (cat && cat.params && cat.params[pname]) || { parameter: pname, style: '{}' };
        const s = dwMergedStyle(p, live);
        const access = (live.access) ? String(live.access)
            : (s.access ? String(s.access) : 'RW');
        if (String(access).toUpperCase() !== 'R') return;
        const row = body.querySelector('.dw-param[data-param="' +
            String(pname).replace(/\\/g, '\\\\').replace(/"/g, '\\"') + '"]');
        if (!row) return;
        const valEl = row.querySelector('.dw-val');
        if (!valEl) return;
        const unit = s.unit || '';
        const next = (live.value != null ? String(live.value) : '') + unit;
        if (valEl.textContent !== next) valEl.textContent = next;
    });
    const meta = document.getElementById('device-detail-meta');
    if (meta) {
        const typeLab = (typeof dwTypeDisplay === 'function')
            ? dwTypeDisplay(inst)
            : mhTypeLabel(inst.type);
        meta.textContent = (inst.point_id || deviceDetailId) +
            (typeLab ? ' · ' + typeLab : '') +
            (inst.online ? ' · ' + t('dw.online') : ' · ' + t('dw.offline'));
    }
}

function refreshDeviceDetail() {
    if (!deviceDetailId) return;
    const modal = document.getElementById('device-detail-modal');
    if (!modal || modal.hidden) return;
    const body = document.getElementById('device-detail-body');
    const inst = dwFindInstance(deviceDetailId);
    const ae = document.activeElement;
    let focusedParam = '';
    if (ae && ae.closest) {
        const row = ae.closest('.dw-param');
        if (row) focusedParam = row.getAttribute('data-param') || '';
    }

    const editing = deviceDetailPanel !== 'info' && ae && body && body.contains(ae) &&
        (ae.tagName === 'INPUT' || ae.tagName === 'SELECT' || ae.tagName === 'TEXTAREA') &&
        !dwParamIsPrerequisite(focusedParam, inst);
    if (!inst) {
        document.getElementById('device-detail-title').textContent = t('dw.device');
        document.getElementById('device-detail-meta').textContent = deviceDetailId + ' · ' + t('dw.offline_or_removed');
        if (body) body.innerHTML = '<div class="modal-hint">' + t('dw.point_not_found') + '</div>';
        dwSyncDetailInfoBtn();
        return;
    }
    if (editing) {
        dwPatchDetailReadonly(inst);
        dwPatchDetailMediaButtons(inst);
        return;
    }
    deviceDetailId = inst.key || inst.point_id;
    const title = document.getElementById('device-detail-title');
    const meta = document.getElementById('device-detail-meta');
    const name = (typeof pointDisplayName === 'function')
        ? pointDisplayName({ point_id: inst.point_id, props: inst.props })
        : (inst.endpoint || inst.point_id);
    if (title) {
        const icon = (typeof TYPE_ICON !== 'undefined' && TYPE_ICON[inst.type]) ? TYPE_ICON[inst.type] + ' ' : '';
        title.textContent = icon + name;
    }
    if (meta) {
        const typeLab = (typeof dwTypeDisplay === 'function')
            ? dwTypeDisplay(inst)
            : mhTypeLabel(inst.type);
        meta.textContent = (inst.point_id || deviceDetailId) +
            (typeLab ? ' · ' + typeLab : '') +
            (inst.online ? ' · ' + t('dw.online') : ' · ' + t('dw.offline'));
    }
    if (body) {
        body.innerHTML = deviceDetailPanel === 'info'
            ? dwInstanceInfoHtml(inst)
            : dwInstanceParamsHtml(inst);
    }
    dwSyncDetailInfoBtn();
}

async function openDeviceDetail(id, ev) {
    if (ev && ev.target && (ev.target.closest('.toggle') || ev.target.closest('button') ||
        ev.target.closest('input') || ev.target.closest('.card-actions'))) return;
    try {
        await ensureDwCatalog();
    } catch (e) {
        toast(t('dw.catalog_load_failed_short'));
        return;
    }

    if (typeof loadPoints === 'function') {
        try {
            await loadPoints();
        } catch (e) {}
    }
    const inst = dwFindInstance(id);
    if (!inst) {
        toast(t('dw.device_not_found'));
        return;
    }
    deviceDetailId = inst.key || inst.point_id;
    deviceDetailPanel = 'controls';

    const modal = document.getElementById('device-detail-modal');
    if (modal) modal.hidden = false;
    refreshDeviceDetail();
}

/* Icons live in the device's own /data/images tree, which the deploy server
 * fills on demand. A type whose SVG is missing from that tree must degrade to
 * the glyph badge instead of showing a broken-image box. */
function dwIconFallback(el) {
    if (!el || el.dataset.fallback) return;
    el.dataset.fallback = '1';
    const m = /\/img\/devices\/([^/.]+)\.svg/.exec(el.getAttribute('src') || '');
    const type = m ? m[1] : '';
    const glyph = (typeof TYPE_ICON !== 'undefined' && TYPE_ICON[type]) ||
        (type ? type.charAt(0).toUpperCase() : '·');
    const span = document.createElement('span');
    span.className = 'dw-device-icon-fallback';
    span.textContent = glyph;
    if (el.parentNode) el.parentNode.replaceChild(span, el);
}

function dwDeviceIconHtml(imageUrl, powerOn) {
    if (!imageUrl) return '';
    let cls = 'dw-device-icon';
    if (powerOn === true) cls += ' is-on';
    else if (powerOn === false) cls += ' is-off';
    return '<img class="' + cls + '" src="' + escAttr(imageUrl) +
        '" alt="" decoding="async" draggable="false" onerror="dwIconFallback(this)">';
}

function dwTypeImageUrl(type) {
    if (!type) return '';
    const cat = dwCatalogForType(dwCatalogIndex(), type);
    if (cat && cat.image) return cat.image;
    const safe = String(type).toLowerCase().replace(/[^a-z0-9_-]/g, '');
    return safe ? ('/img/devices/' + safe + '.svg') : '';
}

function dwCatalogForType(byType, type) {
    if (!type) return null;
    if (byType[type]) return byType[type];
    const lower = String(type).toLowerCase();
    if (byType[lower]) return byType[lower];
    return null;
}

function dwFallbackCatalogParam(pname) {
    const pc = dwCatalog && dwCatalog.parameters;
    if (pc && Object.prototype.hasOwnProperty.call(pc, pname)) {
        const base = pc[pname];
        return {
            parameter: pname,
            label: base.label != null ? base.label : pname,
            widget: base.widget || '',
            style: base.style || '{}',
            image: base.image || '',
            _fallback: true
        };
    }
    return { parameter: pname, label: pname, widget: '', style: '{}', _fallback: true };
}

function dwLiveIsOn(live) {
    if (!live) return null;
    const v = live.value;
    if (v === true || v === 1) return true;
    if (v === false || v === 0) return false;
    const s = String(v == null ? '' : v).toLowerCase();
    if (s === 'on' || s === 'true' || s === '1') return true;
    if (s === 'off' || s === 'false' || s === '0') return false;
    return null;
}

function dwRequireNameList(raw) {
    if (!Array.isArray(raw)) return null;
    const names = [];
    for (let i = 0; i < raw.length; i++) {
        const n = String(raw[i] == null ? '' : raw[i]).trim();
        if (!n || names.indexOf(n) >= 0) return null;
        names.push(n);
    }
    return names.length ? names : null;
}

function dwRequireSpec(requires) {
    if (requires == null || requires === '') return null;
    if (typeof requires === 'string') {
        const text = requires.trim();
        if (!text) return null;
        if (text.charAt(0) === '{') {
            try {
                return dwRequireSpec(JSON.parse(text));
            } catch (e) {
                return null;
            }
        }
        return { op: 'all', names: [text], not: [] };
    }
    if (typeof requires !== 'object' || Array.isArray(requires)) return null;
    const keys = Object.keys(requires);
    for (let i = 0; i < keys.length; i++) {
        if (keys[i] !== 'all' && keys[i] !== 'any' && keys[i] !== 'not') return null;
    }
    const hasAll = Object.prototype.hasOwnProperty.call(requires, 'all');
    const hasAny = Object.prototype.hasOwnProperty.call(requires, 'any');
    const hasNot = Object.prototype.hasOwnProperty.call(requires, 'not');
    if (hasAll && hasAny) return null;
    if (!hasAll && !hasAny && !hasNot) return null;
    const names = hasAll ? dwRequireNameList(requires.all)
        : (hasAny ? dwRequireNameList(requires.any) : []);
    const negated = hasNot ? dwRequireNameList(requires.not) : [];
    if (names == null || negated == null) return null;
    if (!names.length && !negated.length) return null;
    for (let i = 0; i < negated.length; i++) {
        if (names.indexOf(negated[i]) >= 0) return null;
    }
    return {
        op: hasAll ? 'all' : (hasAny ? 'any' : null),
        names: names,
        not: negated
    };
}

function dwNormalizeRequires(requires) {
    const spec = dwRequireSpec(requires);
    if (!spec) return null;
    if (spec.op === 'all' && spec.names.length === 1 && !spec.not.length) return spec.names[0];
    const out = {};
    if (spec.op) out[spec.op] = spec.names.slice();
    if (spec.not.length) out.not = spec.not.slice();
    return out;
}

function dwRequiresSpec(p, live) {
    if (live && live.requires != null) {
        const spec = dwRequireSpec(live.requires);
        if (spec) return spec;
    }
    const s = dwParseStyle(p && p.style);
    return dwRequireSpec(s && s.requires);
}

function dwRequireNotBlocks(spec, inst) {
    for (let i = 0; i < spec.not.length; i++) {
        if (dwLiveIsOn(inst.params[spec.not[i]]) === true) return true;
    }
    return false;
}

function dwControlGated(p, inst, readOnly) {
    if (readOnly) return false;
    const live = (inst && inst.params && p) ? inst.params[p.parameter] : null;
    const spec = dwRequiresSpec(p, live);
    if (!spec || !inst || !inst.params) return false;
    if (dwRequireNotBlocks(spec, inst)) return true;
    if (!spec.names.length) return false;
    if (spec.op === 'any') {
        let explicitOff = 0;
        for (let i = 0; i < spec.names.length; i++) {
            const on = dwLiveIsOn(inst.params[spec.names[i]]);
            if (on === true) return false;
            if (on === false) explicitOff++;
        }
        return explicitOff === spec.names.length;
    }
    for (let i = 0; i < spec.names.length; i++) {
        if (dwLiveIsOn(inst.params[spec.names[i]]) === false) return true;
    }
    return false;
}

function dwSpecMentions(spec, want) {
    if (!spec) return false;
    return spec.names.indexOf(want) >= 0 || spec.not.indexOf(want) >= 0;
}

function dwParamIsPrerequisite(name, inst) {
    if (!name) return false;
    const want = String(name);
    if (inst && inst.params) {
        const keys = Object.keys(inst.params);
        for (let i = 0; i < keys.length; i++) {
            if (dwSpecMentions(dwRequireSpec(inst.params[keys[i]].requires), want)) return true;
        }
    }
    if (!dwCatalog || !Array.isArray(dwCatalog.devices)) return false;
    for (let i = 0; i < dwCatalog.devices.length; i++) {
        const params = dwCatalog.devices[i] && dwCatalog.devices[i].params;
        if (!params) continue;
        for (let j = 0; j < params.length; j++) {
            if (dwSpecMentions(dwRequiresSpec(params[j], null), want)) return true;
        }
    }
    return false;
}

function dwParamRowHtml(type, p, live, opts) {
    opts = opts || {};

    let widget = (live && live.ui_element) ? dwUiToWidget(live.ui_element) : dwWidget(p);
    if (widget === 'hidden') return '';
    const s = dwMergedStyle(p, live);
    const access = (live && live.access) ? String(live.access)
        : (s.access ? String(s.access) : 'RW');
    const readOnly = String(access).toUpperCase() === 'R';
    if (!widget && !readOnly && (s.min != null || s.max != null || s.step != null ||
        (live && (live.min != null || live.max != null || live.step != null)))) {
        widget = 'slider';
    }
    const isBar = !readOnly && (widget === 'slider' || widget === 'stepper');
    const gated = dwControlGated(p, opts.inst, readOnly);
    const extraCss = dwCssClass(live && live.css);
    const paramIcon = dwIconUrl(live && live.icon);
    /* The merged play/pause toggle is a state control, not a "pause" command. */
    const rowLabel = (p.parameter === 'play_pause' || p.parameter === 'play_resume')
        ? t('dw.playing_state') : dwLabel(p);
    return '<div class="dw-param' + (isBar ? ' dw-param-tile' : '') +
        (gated ? ' is-gated' : '') +
        (extraCss ? ' ' + escAttr(extraCss) : '') +
        '" data-param="' + escAttr(p.parameter || '') + '">' +
        '<div class="dw-param-label">' +
        (paramIcon
            ? '<img class="dw-param-icon" src="' + escAttr(paramIcon) + '" alt="">'
            : '') +
        '<span class="k">' + esc(rowLabel) + '</span></div>' +
        '<div class="dw-ctl">' +
        dwParamControl(type || '', p, live, gated) +
        '</div></div>';
}

function dwInstanceCardBody(inst, cat) {
    let visibleCount = 0;
    let fallbackCount = 0;
    let primary = '';
    let extra = '';
    const order = cat && cat.params ? Object.keys(cat.params) : Object.keys(inst.params || {});
    const seen = Object.create(null);
    order.sort(function (a, b) {
        return dwMediaRank(a) - dwMediaRank(b);
    });

    if (!cat) {
        console.warn('[device_widgets] type not in catalog:',
            (inst.type || '(empty type)'), (inst.point_id || inst.key || ''));
    }

    function addRow(pname, p) {
        const live = inst.params[pname];
        if (!live || !live.visible) return;

        /* playing / muted are state readbacks surfaced by the combined
           play/pause and mute/unmute buttons; never draw them as rows. */
        if (pname === 'playing' || pname === 'muted') return;

        /* Merge paired media commands into one toggle button (reads state). */
        if ((pname === 'play_resume' || pname === 'unmute') && cat && cat.params &&
                cat.params[pname === 'play_resume' ? 'play_pause' : 'mute']) {
            return;
        }

        if (dwWidget(p) === 'hidden') return;
        seen[pname] = true;
        const row = dwParamRowHtml(inst.type || '', p, live, { inst: inst });
        if (!row) return;
        visibleCount++;
        if (p && p._fallback) fallbackCount++;
        if (pname === DW_PRIMARY_PARAM) primary += row;
        else extra += row;
    }

    order.forEach(function (pname) {
        const p = (cat && cat.params && cat.params[pname]) || dwFallbackCatalogParam(pname);
        addRow(pname, p);
    });

    Object.keys(inst.params || {}).forEach(function (pname) {
        if (seen[pname]) return;
        addRow(pname, dwFallbackCatalogParam(pname));
    });
    return {
        visibleCount: visibleCount,
        fallbackCount: fallbackCount,
        primary: primary,
        extra: extra
    };
}

function dwToggleCardExpand(key, ev) {
    if (ev) {
        const t = ev.target;
        if (t && (t.closest('.dw-ctl') || t.closest('.toggle') || t.closest('input') ||
            t.closest('button.chip') || t.closest('select') || t.closest('label'))) {
            return;
        }
        ev.preventDefault();
        ev.stopPropagation();
    }
    if (!key) return;
    if (dwExpandedKeys[key]) delete dwExpandedKeys[key];
    else dwExpandedKeys[key] = true;
    const sel = '.dcard.dw-card[data-key="' +
        String(key).replace(/\\/g, '\\\\').replace(/"/g, '\\"') + '"]';
    const card = document.querySelector(sel);
    if (card) {
        const open = !!dwExpandedKeys[key];
        card.classList.toggle('expanded', open);
        const extra = card.querySelector('.dw-params-extra');
        if (extra) extra.hidden = !open;
        const chev = card.querySelector('.dw-expand');
        if (chev) {
            chev.setAttribute('aria-expanded', open ? 'true' : 'false');
            chev.title = open ? t('dw.collapse_params') : t('dw.expand_params');
        }
    }
}

function renderDeviceWidgets() {
    const el = document.getElementById('devices-content');
    if (!el) return;
    const byType = dwCatalogIndex();
    if (!dwInstances.length) {
        el.innerHTML = '<div class="modal-hint">' + t('dw.no_devices') + '</div>';
        return;
    }

    let html = '';
    let shown = 0;
    dwInstances.forEach(function (inst) {
        const cat = dwCatalogForType(byType, inst.type);
        const built = dwInstanceCardBody(inst, cat);
        if (!built.visibleCount) return;
        shown++;
        /* Device icon: plugin override (props.icon) > catalog > /img/devices/<type>.svg. */
        const devIcon = dwIconUrl(inst.props && inst.props.icon);
        const key = inst.key || inst.point_id || inst.endpoint || '';
        const expanded = !!dwExpandedKeys[key];
        const hasExtra = !!built.extra;
        const offline = inst.online ? '' : ' offline';
        const image = devIcon || (cat ? cat.image : '');
        /* Type not in the catalog: rows are built from raw parameter names.
         * Say so on the card rather than looking like a corrupt catalog. */
        const missBadge = built.fallbackCount
            ? '<span class="badge warn" title="' + escAttr(t('dw.type_unregistered_tip')) + '">' + t('dw.type_unregistered') + '</span>'
            : '';
        const expandBtn = hasExtra
            ? ('<button type="button" class="dw-expand" aria-expanded="' +
                (expanded ? 'true' : 'false') + '" title="' +
                (expanded ? t('dw.collapse_params') : t('dw.expand_params')) +
                '" onclick="dwToggleCardExpand(\'' + escAttr(key) + '\', event)"></button>')
            : '';
        html += '<div class="dcard dw-card' + offline + (expanded ? ' expanded' : '') +
            '" data-key="' + escAttr(key) + '"' +
            (hasExtra ? ' onclick="dwToggleCardExpand(\'' + escAttr(key) + '\', event)"' : '') +
            '><div class="card-top">' +
            dwDeviceIconHtml(image) +
            '<div class="card-name">' +
            esc((typeof pointDisplayName === 'function')
                ? pointDisplayName({ point_id: inst.point_id, props: inst.props })
                : (inst.endpoint || inst.key)) + '</div>' +
            '<span class="badge">' + esc(inst.type || inst.protocol || 'device') +
            ' · ' + built.visibleCount + '</span>' +
            missBadge +
            expandBtn +
            '</div>';

        if (built.primary) {
            html += '<div class="dw-params dw-params-primary">' + built.primary + '</div>';
        }
        if (hasExtra) {
            html += '<div class="dw-params dw-params-extra"' +
                (expanded ? '' : ' hidden') + '>' + built.extra + '</div>';
        }
        html += '</div>';
    });

    if (!shown) {
        el.innerHTML = '<div class="modal-hint">' + t('dw.all_params_hidden') + '</div>';
        return;
    }
    el.innerHTML = html;
}

function dwMergedStyle(p, live) {
    const s = Object.assign({}, dwParseStyle(p && p.style));
    if (!live) return s;
    if (live.min != null) s.min = Number(live.min);
    if (live.max != null) s.max = Number(live.max);
    if (live.step != null) s.step = Number(live.step);
    if (live.unit != null && live.unit !== '') s.unit = live.unit;
    return s;
}

function dwSliderTone(parameter) {
    const n = String(parameter || '').toLowerCase();
    if (n === 'hue' || n.indexOf('hue') >= 0) return 'tone-hue';
    if (n.indexOf('bright') >= 0 || n === 'intensity') return 'tone-bright';
    if (n.indexOf('sat') >= 0) return 'tone-sat';
    if (n.indexOf('cct') >= 0 || n.indexOf('color_temp') >= 0) return 'tone-cct';
    if (n.indexOf('voltage') >= 0) return 'tone-voltage';
    if (n.indexOf('current') >= 0) return 'tone-current';
    if (n === 'power_w' || n.indexOf('power_w') >= 0) return 'tone-power';
    return 'tone-default';
}

function dwSliderIsSpectrum(tone, isHue) {
    if (isHue) return true;
    return tone === 'tone-sat' || tone === 'tone-cct' || tone === 'tone-hue';
}

function dwRangePct(min, max, val) {
    const lo = Number(min);
    const hi = Number(max);
    const v = Number(val);
    if (!(hi > lo) || isNaN(v)) return 0;
    const pct = ((v - lo) / (hi - lo)) * 100;
    if (pct < 0) return 0;
    if (pct > 100) return 100;
    return pct;
}

function dwDisabledAttr(gated) {
    return gated ? ' disabled' : '';
}

function dwTileRangeHtml(o) {
    const min = o.min;
    const max = o.max;
    const step = (o.step != null) ? o.step : 1;
    const unit = o.unit || '';
    const val = o.val;
    const pct = dwRangePct(min, max, val);
    const vid = 'val-' + o.type + '-' + o.parameter;
    const tone = o.tone || 'tone-default';
    const spectrum = !!o.spectrum || dwSliderIsSpectrum(tone, o.hue);
    const hueCls = o.hue ? ' is-hue' : '';
    const specCls = spectrum ? ' is-spectrum' : '';
    const pidArg = ',\'' + escAttr(o.point_id || '') + '\'';
    const oninput = spectrum
        ? ('dwTilePreview(this,\'' + escAttr(vid) + '\',\'' + escAttr(unit) + '\',true)')
        : ('dwTilePreview(this,\'' + escAttr(vid) + '\',\'' + escAttr(unit) + '\')');
    const pctStyle = spectrum ? '' : (' style="--pct:' + pct + '%"');
    return '<div class="dw-tile-slider ' + escAttr(tone) + hueCls + specCls + '"' +
        pctStyle + '>' +
        '<input type="range" class="dw-tile-range" min="' + min + '" max="' + max +
        '" step="' + step + '" value="' + val + '"' +
        dwDisabledAttr(o.gated) +
        ' aria-label="' + escAttr(o.label || '') + '"' +
        ' oninput="' + oninput + '"' +
        ' onchange="dwSend(\'' + escAttr(o.type) + '\',\'' +
        escAttr(o.parameter) + '\', Number(this.value)' + pidArg + ')">' +
        '<span class="dw-val" id="' + escAttr(vid) + '">' +
        esc(String(val)) + esc(unit) + '</span></div>';
}

function dwParamControl(type, p, live, gated) {
    const s = dwMergedStyle(p, live);
    let widget = dwWidget(p);
    const valStr = (live && live.value != null) ? String(live.value) : '';

    const access = (live && live.access) ? String(live.access)
        : (s.access ? String(s.access) : 'RW');
    const readOnly = String(access).toUpperCase() === 'R';
    const pointId = (live && live.point_id) ? String(live.point_id) : '';
    const pidArg = ',\'' + escAttr(pointId) + '\'';

    if (!widget && !readOnly && (s.min != null || s.max != null || s.step != null ||
        (live && (live.min != null || live.max != null || live.step != null)))) {
        widget = 'slider';
    }

    if (widget === 'hidden') return '';

    if (readOnly) {
        const unit = s.unit || '';
        let display = valStr;
        if (valStr !== '' && !isNaN(Number(valStr))) {
            const n = Number(valStr);
            const step = (s.step != null) ? Number(s.step) : null;
            if (step != null && step > 0 && step < 1) {
                const dec = Math.min(6, (String(step).split('.')[1] || '').length || 2);
                display = n.toFixed(dec);
            } else if (step != null && step >= 1) {
                display = String(Math.round(n));
            } else if (!Number.isInteger(n)) {
                display = String(Math.round(n * 1000) / 1000);
            }
        } else if (unit && valStr !== '' && isNaN(Number(valStr))) {
            /* Numeric param (has a unit) carrying a non-numeric token — e.g. a
             * device that returned boolean false when off. Show 0, not raw. */
            display = '0';
        }
        return '<span class="dw-val">' + esc(display) + esc(unit) + '</span>';
    }

    if (widget === 'slider' || widget === 'stepper') {
        const min = (s.min != null) ? s.min : 0;
        const max = (s.max != null) ? s.max : 100;
        const step = (s.step != null) ? s.step : 1;
        const unit = s.unit || '';
        let val = min;
        if (valStr !== '' && !isNaN(Number(valStr))) val = Number(valStr);
        const tone = dwSliderTone(p.parameter);
        const isHue = (tone === 'tone-hue');
        return dwTileRangeHtml({
            type: type,
            parameter: p.parameter,
            label: dwLabel(p),
            min: min,
            max: max,
            step: step,
            unit: unit,
            val: val,
            tone: tone,
            hue: isHue,
            spectrum: dwSliderIsSpectrum(tone, isHue),
            point_id: pointId,
            gated: !!gated
        });
    }
    if (widget === 'button') {
        return dwMediaButtonHtml(type, p, live, pointId, pidArg);
    }
    if (widget === 'dropdown') {
        const opts = Array.isArray(s.options) ? s.options : [];
        let body = '';
        opts.forEach(function (o) {
            const val = (o && typeof o === 'object') ? o.value : o;
            const L = (o && o.labels) ? o.labels : {};
            const lbl = L[window.mhCatalogLang()] || L.zh || (o && o.label) || (o && o.value) || o;
            const sel = (valStr !== '' && String(val) === valStr) ? ' selected' : '';
            body += '<option value="' + escAttr(String(val)) + '"' + sel + '>' +
                esc(lbl) + '</option>';
        });
        return '<select class="dw-dropdown" aria-label="' + escAttr(dwLabel(p)) + '"' +
            dwDisabledAttr(gated) +
            ' onchange="dwSend(\'' + escAttr(type) + '\',\'' +
            escAttr(p.parameter) + '\', dwSendValue(this.value)' + pidArg + ')">' +
            body + '</select>';
    }
    if (widget === 'text') {
        return '<input type="text" class="dw-text" value="' + escAttr(valStr) +
            '" aria-label="' + escAttr(dwLabel(p)) + '"' +
            dwDisabledAttr(gated) +
            ' onchange="dwSend(\'' + escAttr(type) + '\',\'' +
            escAttr(p.parameter) + '\', this.value' + pidArg + ')">';
    }
    if (widget === 'toggle' || !widget) {
        const on = valStr === '1' || valStr === 'true' || valStr === 'on' ||
            valStr === 'ON';
        return '<label class="toggle"><input type="checkbox"' + (on ? ' checked' : '') +
            dwDisabledAttr(gated) +
            ' aria-label="' + escAttr(dwLabel(p)) + '"' +
            ' onchange="dwSend(\'' + escAttr(type) + '\',\'' +
            escAttr(p.parameter) + '\', this.checked' + pidArg +
            ')"><span class="slider"></span></label>';
    }

    return '<input type="text" class="dw-text" value="' + escAttr(valStr) +
        '" aria-label="' + escAttr(dwLabel(p)) + '"' +
        dwDisabledAttr(gated) +
        ' onchange="dwSend(\'' + escAttr(type) + '\',\'' +
        escAttr(p.parameter) + '\', this.value' + pidArg + ')">';
}

function dwMediaStateOn(live) {
    if (!live || live.value == null) return false;
    const v = live.value;
    return v === true || v === '1' || v === 'on' || v === 'ON' || v === 1;
}

function dwMediaButtonHtml(type, p, live, pointId, pidArg) {
    const param = p.parameter;
    const inst = dwFindInstance(pointId);

    /* pause/resume combined toggle reads `playing` */
    if (param === 'play_pause' || param === 'play_resume') {
        const playing = inst && inst.params && inst.params.playing;
        const isPlaying = dwMediaStateOn(playing);
        const cmd = isPlaying ? 'play_pause' : 'play_resume';
        const label = isPlaying ? t('dw.pause') : t('dw.play');
        const icon = isPlaying ? '⏸' : '▶';
        return '<button type="button" class="dw-media-btn' + (isPlaying ? ' is-on' : '') +
            '" aria-label="' + escAttr(label) + '" title="' + escAttr(label) + '"' +
            ' onclick="dwSend(\'' + escAttr(type) + '\',\'' + escAttr(cmd) +
            '\',1' + pidArg + ')"><span class="ic">' + icon +
            '</span></button>';
    }
    /* mute/unmute combined toggle reads `muted` */
    if (param === 'mute' || param === 'unmute') {
        const muted = inst && inst.params && inst.params.muted;
        const isMuted = dwMediaStateOn(muted);
        const cmd = isMuted ? 'unmute' : 'mute';
        const label = isMuted ? t('dw.unmute') : t('dw.mute');
        const icon = isMuted ? '🔇' : '🔈';
        return '<button type="button" class="dw-media-btn' + (isMuted ? ' is-on' : '') +
            '" aria-label="' + escAttr(label) + '" title="' + escAttr(label) + '"' +
            ' onclick="dwSend(\'' + escAttr(type) + '\',\'' + escAttr(cmd) +
            '\',1' + pidArg + ')"><span class="ic">' + icon +
            '</span></button>';
    }
    /* plain command button (next/prev render as symbols) */
    const cmdLabel = dwLabel(p);
    const cmdIcon = param === 'play_next' ? '⏭' :
        (param === 'play_prev' ? '⏮' : '');
    return '<button type="button" class="dw-media-btn"' +
        ' aria-label="' + escAttr(cmdLabel) + '" title="' + escAttr(cmdLabel) + '"' +
        ' onclick="dwSend(\'' + escAttr(type) + '\',\'' + escAttr(param) +
        '\',1' + pidArg + ')">' +
        (cmdIcon
            ? '<span class="ic">' + cmdIcon + '</span>'
            : '<span>' + esc(cmdLabel) + '</span>') +
        '</button>';
}

function dwSendValue(v) {
    const s = String(v);
    if (s !== '' && /^-?\d+(\.\d+)?$/.test(s)) return Number(s);
    return s;
}

async function dwSend(device, parameter, value, pointId) {
    try {
        const body = {
            device: device,
            parameter: parameter,
            value: value,
            seq: (Date.now() & 0xffffffff)
        };
        if (pointId) {
            body.point_id = pointId;
        }
        const r = await fetch('/api/devices/control', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(body)
        });
        if (!r.ok) {
            const raw = await r.text().catch(function () { return ''; });
            toast(t('dw.control_failed', { error: mhHttpErrorText(r.status, raw) }));
        } else if (typeof refreshAfterCommand === 'function') {
            refreshAfterCommand();
        } else if (typeof loadPoints === 'function') {
            loadPoints();
        }
    } catch (e) {
        toast(t('dw.control_failed', { error: e }));
    }
}

function dwHuePreview(id, hue) {
    const el = document.getElementById(id);
    if (el) el.style.background = 'hsl(' + hue + ',100%,50%)';
}

function dwRangePreview(id, unit, value) {
    const el = document.getElementById(id);
    if (el) el.textContent = value + (unit || '');
}

function dwTilePreview(input, vid, unit, isSpectrum) {
    if (!input) return;
    const wrap = input.closest ? input.closest('.dw-tile-slider') : null;
    if (wrap && !isSpectrum) {
        wrap.style.setProperty('--pct',
            dwRangePct(input.min, input.max, input.value) + '%');
    }
    dwRangePreview(vid, unit, input.value);
}
