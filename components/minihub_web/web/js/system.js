let sysIface = 'eth',
    sysSnap = null;
let sysHintProbed = false;

function fmtBytes(n) {
    n = Number(n) || 0;
    if (n < 1024) return n + ' B';
    if (n < 1048576) return (n / 1024).toFixed(1) + ' KB';
    if (n < 1073741824) return (n / 1048576).toFixed(1) + ' MB';
    return (n / 1073741824).toFixed(2) + ' GB';
}

function sysCard(title, val, sub) {
    return '<div class="sys-card"><h4>' + esc(title) + '</h4><div class="v">' + esc(val) + '</div>' + (sub ? '<div class="s">' + esc(sub) + '</div>' : '') + '</div>';
}

function firstText() {
    for (let i = 0; i < arguments.length; i++) {
        const v = arguments[i] == null ? '' : String(arguments[i]).trim();
        if (v) return v;
    }
    return '';
}

function sysVerCard(title, val, subs) {
    let html = '<div class="sys-card"><h4>' + esc(title) + '</h4><div class="v">' + esc(val || t('sys.none')) + '</div>';
    (subs || []).forEach(function (line) {
        if (line) html += '<div class="s">' + esc(line) + '</div>';
    });
    return html + '</div>';
}

function showReleaseNote(id, note) {
    const el = document.getElementById(id);
    if (!el) return;
    const raw = note == null ? '' : String(note).trim();
    el.textContent = raw || t('sys.none');
}

async function loadUpdateNotes() {
    try {
        const r = await fetch('/api/update/notes');
        if (!r.ok) return {};
        const j = await r.json();
        return j && typeof j === 'object' ? j : {};
    } catch (e) {
        return {};
    }
}

/* Re-render the global update banner (boot.js) right after a check/apply, so it
 * does not keep announcing a version we just installed until the next 60s poll. */
function refreshUpdateBanner() {
    if (typeof pollUpdateHint === 'function') {
        try { pollUpdateHint(); } catch (e) {}
    }
}

function fmtChip(chip) {
    if (!chip) return '-';
    const L = {
        binary_sensor: t('sys.door_window_alarm'),
        smoke: t('sys.smoke_detector'),
        router: t('sys.router'),
        car: t('sys.vehicle')
    };
    return L[chip] || String(chip).toUpperCase();
}

function sysWifiOn() {
    const st = sysSnap && sysSnap.net && sysSnap.net.wifi;
    if (st && st.present === false) return false;
    if (typeof adapterEnabled === 'function') return adapterEnabled('wifi');
    return !!(st && st.present);
}

function sysSyncWifiTab() {
    const on = sysWifiOn();
    const btn = document.getElementById('sys-if-wifi');
    const seg = document.getElementById('sys-if-seg');
    if (btn) btn.hidden = !on;
    if (seg) seg.hidden = !on;
    if (!on && sysIface === 'wifi') sysIface = 'eth';
    const eth = document.getElementById('sys-if-eth');
    if (eth) eth.classList.toggle('on', sysIface === 'eth');
    if (btn) btn.classList.toggle('on', on && sysIface === 'wifi');
}

function sysSelectIface(ifn) {
    if (ifn === 'wifi' && !sysWifiOn()) ifn = 'eth';
    sysIface = ifn;
    sysSyncWifiTab();
    sysFillNet();
}

function sysToggleDhcp() {
    const dhcp = document.getElementById('sys-dhcp').value === '1';
    document.getElementById('sys-static-fields').style.opacity = dhcp ? 0.45 : 1;
    document.getElementById('sys-static-fields').style.pointerEvents = dhcp ? 'none' : 'auto';
}

function sysFillNet() {
    if (!sysSnap || !sysSnap.net) return;
    sysSyncWifiTab();
    const st = sysSnap.net[sysIface] || {};
    const linked = !!(st.ip);
    const note = document.getElementById('sys-if-note');
    if (!st.present) {
        note.textContent = t('sys.iface_not_enabled');
        note.hidden = false;
    } else if (!linked) {
        note.textContent = (sysIface === 'wifi' && st.ssid) ? t('sys.connecting') : t('sys.not_connected');
        note.hidden = false;
    } else {
        note.textContent = t('sys.current_ip', { ip: st.ip });
        note.hidden = false;
    }
    document.getElementById('sys-dhcp').value = st.dhcp !== false ? '1' : '0';
    const dns = linked ? (st.dns || []) : [];
    document.getElementById('sys-ip').value = linked ? (st.ip || '') : '';
    document.getElementById('sys-mask').value = linked ? (st.netmask || '') : '';
    document.getElementById('sys-gw').value = linked ? (st.gw || '') : '';
    document.getElementById('sys-dns1').value = dns[0] || '';
    document.getElementById('sys-dns2').value = dns[1] || '';
    const cred = document.getElementById('sys-wifi-cred');
    if (cred) cred.hidden = sysIface !== 'wifi';
    const ssidEl = document.getElementById('sys-wifi-ssid');
    const passEl = document.getElementById('sys-wifi-pass');
    if (sysIface === 'wifi' && ssidEl) ssidEl.value = st.ssid || '';
    if (passEl && sysIface === 'wifi') {
        passEl.value = '';
        passEl.type = 'password';
        const eye = passEl.parentElement && passEl.parentElement.querySelector('.secret-toggle');
        if (eye) {
            eye.textContent = t('sys.show');
            eye.setAttribute('aria-label', t('sys.show_password'));
        }
    }
    sysToggleDhcp();
}

let otaTimer = null;

function sysOtaRender(j) {
    const st = document.getElementById('sys-ota-status');
    const bar = document.getElementById('sys-ota-bar');
    if (!st || !bar) return;
    const state = j.state | 0;
    const prog = j.progress;
    const src = j.source || 'none';
    const err = j.error || '';
    /* Resolved here, not in a module-level table: the dictionary is not loaded when
     * this file is parsed. */
    const stateKeys = {
        0: 'sys.state_idle',
        1: 'sys.state_upgrading',
        2: 'sys.state_success',
        3: 'sys.failed'
    };
    const stateLabel = stateKeys[state] ? t(stateKeys[state]) : state;
    let line = t('sys.ota_status_line', {
        state: stateLabel,
        src: src
    });
    if (prog != null && prog >= 0) line += ' · ' + prog + '%';
    if (j.bytes_written) line += ' · ' + j.bytes_written + ' B';
    if (err) line += ' · ' + err;
    st.textContent = line;
    bar.style.width = (prog != null && prog >= 0) ? (prog + '%') : (state === 1 ? '15%' : '0%');
    if (state === 1) {
        if (!otaTimer) otaTimer = setInterval(sysOtaPoll, ONE_SECOND_MS);
    } else if (otaTimer) {
        clearInterval(otaTimer);
        otaTimer = null;
    }
    if (state === 2) toast(t('sys.ota_success_reboot'));
}
async function sysOtaPoll() {
    try {
        const r = await fetch('/api/ota');
        if (!r.ok) return;
        sysOtaRender(await r.json());
    } catch (e) {}
}
async function sysOtaStartUrl() {
    const url = (document.getElementById('sys-ota-url').value || '').trim();
    if (!url) {
        toast(t('sys.enter_firmware_url'));
        return;
    }
    toast(t('sys.starting_url_update'));
    try {
        const r = await fetch('/api/ota', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify({
                url
            })
        });
        if (r.status === 409) {
            toast(t('sys.update_busy'));
            return;
        }
        if (!r.ok) {
            toast(t('sys.start_failed'));
            return;
        }
        toast(t('sys.download_started'));
        sysOtaPoll();
    } catch (e) {
        toast(t('sys.network_error'));
    }
}
async function sysOtaUpload() {
    const inp = document.getElementById('sys-ota-file');
    const f = inp && inp.files && inp.files[0];
    if (!f) {
        toast(t('sys.choose_bin_file'));
        return;
    }
    toast(t('sys.uploading_file', { name: f.name }));
    try {
        const r = await fetch('/api/ota/upload', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/octet-stream'
            },
            body: f
        });
        if (r.status === 409) {
            toast(t('sys.update_busy'));
            return;
        }
        if (!r.ok) {
            /* Named `detail`, not `t`: a local `t` would shadow the i18n helper. */
            const detail = await r.text().catch(() => '');
            toast(t('sys.upload_failed', { detail: detail || r.status }));
            sysOtaPoll();
            return;
        }
        toast(t('sys.upload_done_reboot'));
        sysOtaPoll();
    } catch (e) {
        toast(t('sys.upload_error'));
        sysOtaPoll();
    }
}

window.hubTzOffsetMin = 0;

function hubTzLabel(id) {
    if (!id || id === 'UTC') return t('sys.tz.utc');
    const m = /^UTC([+-])(\d{1,2})(?::(\d{2}))?$/.exec(String(id));
    // 东/西N区 only names whole hours; a half or quarter hour speaks for itself
    if (!m || m[3]) return String(id);
    const hour = parseInt(m[2], 10);
    const cn = ['', '一', '二', '三', '四', '五', '六', '七', '八', '九', '十', '十一', '十二', '十三', '十四'];
    const zh = window.mhLang && String(window.mhLang).indexOf('zh') === 0;
    const n = (zh && cn[hour]) ? cn[hour] : String(hour);
    return t(m[1] === '+' ? 'sys.tz.east' : 'sys.tz.west', { n: n, id: id });
}

function hubWallNow() {
    const off = Number(window.hubTzOffsetMin) || 0;
    const d = new Date(Date.now() + off * 60000);
    const p = x => String(x).padStart(2, '0');
    return d.getUTCFullYear() + '-' + p(d.getUTCMonth() + 1) + '-' + p(d.getUTCDate()) +
        ' ' + p(d.getUTCHours()) + ':' + p(d.getUTCMinutes());
}

function applyTimezonePayload(j) {
    if (!j) return;
    const off = Number(j.offset_min);
    window.hubTzOffsetMin = Number.isFinite(off) ? off : 0;
    const sel = document.getElementById('sys-tz-id');
    if (sel && Array.isArray(j.zones)) {
        const cur = j.id || 'UTC';
        sel.innerHTML = j.zones.map(function (id) {
            const text = hubTzLabel(id);
            return '<option value="' + esc(id) + '"' + (id === cur ? ' selected' : '') + '>' + esc(text) + '</option>';
        }).join('');
    }
    const now = document.getElementById('sys-tz-now');
    if (now) now.textContent = t('sys.tz_now', { time: hubWallNow() });
}

async function loadTimezone() {
    try {
        const r = await fetch('/api/system/timezone');
        if (!r.ok) return;
        applyTimezonePayload(await r.json());
    } catch (e) {}
}

async function sysSaveTimezone() {
    const sel = document.getElementById('sys-tz-id');
    if (!sel || !sel.value) return;
    try {
        const r = await fetch('/api/system/timezone', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ id: sel.value })
        });
        const j = await r.json().catch(() => ({}));
        if (!r.ok) {
            toast(t('sys.failed'));
            return;
        }
        applyTimezonePayload(j);
        toast(t('sys.tz_saved'));
        if (typeof renderOverview === 'function') renderOverview();
    } catch (e) {
        toast(t('sys.network_error'));
    }
}

async function loadSystem() {
    try {
        const r = await fetch('/api/system');
        if (!r.ok) {
            toast(t('sys.load_failed'));
            return;
        }
        sysSnap = await r.json();
        const notes = await loadUpdateNotes();
        const ram = sysSnap.ram || {};
        const st = sysSnap.storage || {};
        const ms = sysSnap.modstore || {};
        const hostVer = firstText(notes.firmware_version, sysSnap.commit);
        const webVer = firstText(notes.web_version, window.mhWebVersion);
        const idfLine = sysSnap.idf_version ? ('IDF: ' + sysSnap.idf_version) : '';
        const commitLine = sysSnap.commit ? ('MiniHub: ' + sysSnap.commit) : '';
        const productLine = sysSnap.version ? t('sys.product_version', { version: sysSnap.version }) : '';
        const verRows = document.getElementById('sys-ver-rows');
        if (verRows) {
            let hints = {};
            try {
                const hr = await fetch('/api/update/hints');
                if (hr.ok) hints = await hr.json();
            } catch (e) {}
            const fwHint = (hints && hints.firmware) || {};
            const webHint = (hints && hints.web) || {};
            const assetsHint = (hints && hints.assets) || {};
            /* Both rows are symmetric: the "新版本 vX" badge opens that channel's
             * release notes (sysShowNotes), the 更新… button applies the new
             * bundle, and 重新安装 force re-pulls the same version (force=true). */
            const fwNew = fwHint.version ? '<button type="button" class="badge-new badge-btn" onclick="sysShowNotes(\'firmware\')">' + t('sys.new_version', { v: fwHint.version }) + '</button>' : '';
            const fwUp = fwHint.version ?
                '<button class="btn-sm on" type="button" onclick="sysUpdateApply(\'firmware\')">' + t('sys.update_firmware') + '</button>' : '';
            const webNew = webHint.version ? '<button type="button" class="badge-new badge-btn" onclick="sysShowNotes(\'web\')">' + t('sys.new_version', { v: webHint.version }) + '</button>' : '';
            const webUp = webHint.version ?
                '<button class="btn-sm on" type="button" onclick="sysUpdateApply(\'web\')">' + t('sys.update_web') + '</button>' : '';
            const fwReinstall = '<button class="btn-sm" type="button" onclick="sysUpdateApply(\'firmware\', true)">' + t('sys.reinstall') + '</button>';
            const webReinstall = '<button class="btn-sm" type="button" onclick="sysUpdateApply(\'web\', true)">' + t('sys.reinstall') + '</button>';
            const assetReinstall = '<button class="btn-sm" type="button" onclick="sysUpdateApply(\'asset\', true)">' + t('sys.reinstall') + '</button>';
            const assetBadge = (assetsHint && assetsHint.pending > 0) ?
                '<span class="badge-new">' + t('sys.assets_pending', { n: assetsHint.pending }) + '</span>' : '';
            const assetRow = assetBadge ?
                '<div class="ver-row"><div class="ver-meta"><div class="ver-title">' + t('sys.card_device_assets') + '</div></div>' +
                '<div class="ver-actions">' + assetBadge +
                '<button class="btn-sm" type="button" onclick="sysUpdateApply(\'asset\')">' + t('sys.update_assets') + '</button>' +
                assetReinstall + '</div></div>' : '';
            verRows.innerHTML =
                '<div class="ver-row"><div class="ver-meta"><div class="ver-title">' + t('sys.card_firmware_version') + '</div>' +
                '<div class="ver-val">' + esc(hostVer || t('sys.none')) + '</div>' +
                '<div class="ver-extra">' + esc([idfLine, commitLine, productLine].filter(Boolean).join(' · ')) + '</div></div>' +
                '<div class="ver-actions">' + fwNew + fwUp + fwReinstall + '</div></div>' +
                '<div class="ver-row"><div class="ver-meta"><div class="ver-title">' + t('sys.card_web_version') + '</div>' +
                '<div class="ver-val">' + esc(webVer || t('sys.none')) + '</div></div>' +
                '<div class="ver-actions">' + webNew + webUp + webReinstall + '</div></div>' +
                assetRow;
        }
        showReleaseNote('sys-web-note', notes.web_note);
        showReleaseNote('sys-fw-note', notes.firmware_note);
        const grid = document.getElementById('sys-info-grid');
        grid.innerHTML = sysCard(t('sys.chip_type'), fmtChip(sysSnap.chip), [sysSnap.chip_cores != null ? ('Cores: ' + sysSnap.chip_cores) : '', sysSnap.chip_revision != null ? ('Revision: ' + sysSnap.chip_revision) : ''].filter(Boolean).join(', ')) + sysCard('Flash', fmtBytes(sysSnap.flash_size), t('sys.total_capacity')) + sysCard(t('sys.internal_ram'), fmtBytes(ram.internal_free), t('sys.total_value', { total: fmtBytes(ram.internal_total) })) + sysCard('PSRAM', fmtBytes(ram.psram_free), t('sys.total_value', { total: fmtBytes(ram.psram_total) })) + sysCard('Storage', fmtBytes(st.free), t('sys.free_total_used', { free: fmtBytes(st.free), total: fmtBytes(st.total), used: fmtBytes(st.used) })) + sysCard('Modstore', fmtBytes(ms.free), t('sys.free_total_used', { free: fmtBytes(ms.free), total: fmtBytes(ms.total), used: fmtBytes(ms.used) })) + sysCard(t('sys.uptime'), (Number(sysSnap.uptime_s) || 0) + ' s', '');
        const hv = document.getElementById('hub-ver');
        if (hv && sysSnap.version) hv.textContent = 'v' + sysSnap.version;
        sysFillNet();
        if (sysSnap.timezone) applyTimezonePayload(sysSnap.timezone);
        /* The version rows show whatever the device's last probe cached, which is
         * empty right after a boot or a flash. Re-probe once per page load so
         * opening / refreshing the System view reflects the deploy server. The
         * probe runs on the hub's hint task, so POST answers "pending" at once:
         * wait for it before re-reading, or we would show the stale cache. */
        if (!sysHintProbed) {
            sysHintProbed = true;
            try {
                if (await sysProbeHints(60000)) {
                    await loadSystem();
                }
            } catch (e) {}
        }
    } catch (e) {
        toast(t('sys.network_error'));
    }
}
async function sysApplyNet() {
    const dhcp = document.getElementById('sys-dhcp').value === '1';
    const body = {
        iface: sysIface,
        dhcp: dhcp,
        ip: document.getElementById('sys-ip').value.trim(),
        netmask: document.getElementById('sys-mask').value.trim(),
        gw: document.getElementById('sys-gw').value.trim(),
        dns: [document.getElementById('sys-dns1').value.trim(), document.getElementById('sys-dns2').value.trim()].filter(Boolean)
    };
    if (!dhcp && (!body.ip || !body.netmask || !body.gw)) {
        toast(t('sys.enter_ip_mask_gw'));
        return;
    }
    toast(t('sys.applying'));
    try {
        const r = await fetch('/api/net/config', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify(body)
        });
        const j = await r.json().catch(() => ({}));
        if (r.ok && j.result === 'ok') {
            toast(t('sys.applied'));
            if (!dhcp && body.ip && body.ip !== location.hostname) {
                toast(t('sys.use_new_address', { url: 'http://' + body.ip + '/' }));
            }
            setTimeout(loadSystem, 800);
        } else {
            toast(t('sys.failed') + (j.error ? ': ' + j.error : ''));
        }
    } catch (e) {
        toast(t('sys.network_error_ip_changed'));
    }
}

async function sysReboot() {
    if (!window.confirm(t('sys.reboot_confirm'))) {
        return;
    }
    toast(t('sys.rebooting'));
    try {
        const r = await fetch('/api/system/reboot', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify({
                confirm: true
            })
        });
        const j = await r.json().catch(() => ({}));
        if (r.ok && j.result === 'ok') {
            toast(t('sys.device_rebooting'));
        } else {
            toast(t('sys.failed') + (j.error ? ': ' + j.error : ''));
        }
    } catch (e) {
        toast(t('sys.reopen_later'));
    }
}

async function sysFactoryReset() {
    if (!window.confirm(t('sys.factory_reset_confirm'))) {
        return;
    }
    toast(t('sys.factory_resetting'));
    try {
        const r = await fetch('/api/system/reset', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify({
                confirm: true
            })
        });
        const j = await r.json().catch(() => ({}));
        if (r.ok && j.result === 'ok') {
            toast(t('sys.relogin_later'));
        } else {
            toast(t('sys.failed') + (j.error ? ': ' + j.error : ''));
        }
    } catch (e) {
        toast(t('sys.reopen_later'));
    }
}

async function sysChangePassword() {
    const oldP = document.getElementById('sys-pw-old').value || '';
    const newP = document.getElementById('sys-pw-new').value || '';
    const confirmP = document.getElementById('sys-pw-confirm').value || '';
    const hint = document.getElementById('sys-pw-hint');
    hint.textContent = t('mdl.pw_hint');
    if (!oldP || !newP) {
        hint.textContent = t('mdl.pw_err_empty');
        return;
    }
    if (newP !== confirmP) {
        hint.textContent = t('mdl.pw_err_match');
        return;
    }
    if (newP.length < 6 || newP.length > 64) {
        hint.textContent = t('mdl.pw_err_len');
        return;
    }
    try {
        const r = await fetch('/api/auth/change', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ username: 'admin', old_password: oldP, new_password: newP })
        });
        if (r.ok) {
            hint.textContent = t('mdl.pw_ok');
            document.getElementById('sys-pw-old').value = '';
            document.getElementById('sys-pw-new').value = '';
            document.getElementById('sys-pw-confirm').value = '';
            return;
        }
        if (r.status === 401) {
            hint.textContent = t('mdl.pw_err_old');
            return;
        }
        if (r.status === 400) {
            hint.textContent = t('mdl.pw_err_bad');
            return;
        }
        hint.textContent = t('mdl.pw_err_other');
    } catch (e) {
        hint.textContent = t('mdl.pw_err_net');
    }
}

async function sysApplyWifi() {
    const ssid = (document.getElementById('sys-wifi-ssid').value || '').trim();
    const password = document.getElementById('sys-wifi-pass').value || '';
    const open = document.getElementById('sys-wifi-open').checked;
    if (!ssid) {
        toast(t('sys.enter_ssid'));
        return;
    }
    if (!window.confirm(t('sys.wifi_reconnect_confirm', { ssid: ssid }))) {
        return;
    }
    toast(t('sys.saving_wifi'));
    try {
        const r = await fetch('/api/net/wifi', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify({
                ssid: ssid,
                password: open ? '' : password,
                open: open
            })
        });
        const j = await r.json().catch(() => ({}));
        if (r.ok && j.result === 'ok') {
            toast(t('sys.saved_reconnecting'));
            document.getElementById('sys-wifi-pass').value = '';
            /* GOT_IP is often 3–5 s after reconnect; one early refresh stays blank. */
            for (let i = 0; i < 12; i++) {
                await new Promise(res => setTimeout(res, ONE_SECOND_MS));
                await loadSystem();
                const st = sysSnap && sysSnap.net && sysSnap.net.wifi;
                if (st && st.ip) {
                    toast(t('sys.wifi_connected_ip', { ip: st.ip }));
                    return;
                }
            }
            toast(t('sys.no_address_yet'));
        } else {
            toast(t('sys.failed') + (j.error ? ': ' + j.error : ''));
        }
    } catch (e) {
        toast(t('sys.network_error_new_wifi'));
    }
}

/* Trigger a hint probe and wait for it to finish. The probe never runs on the
 * httpd task (its network I/O would stall every other request), so POST returns
 * "pending" and the result has to be polled. Returns true only when the probe
 * actually completed; false on timeout, transport error, or the hint subsystem
 * being unavailable. */
async function sysProbeHints(timeoutMs) {
    const r = await fetch('/api/update/check', { method: 'POST' });
    if (!r.ok) return false;
    let status = await r.json();
    if (status && status.result === 'unavailable') return false;
    const deadline = Date.now() + (timeoutMs || 60000);
    while (status && status.result === 'pending' && Date.now() < deadline) {
        await new Promise(res => setTimeout(res, 500));
        const sr = await fetch('/api/update/check');
        if (!sr.ok) return false;
        status = await sr.json();
        if (status && status.result === 'unavailable') return false;
    }
    return !(status && status.result === 'pending');
}

async function sysUpdateCheck() {
    toast(t('sys.checking_update'));
    try {
        const done = await sysProbeHints(60000);
        refreshUpdateBanner();
        await loadSystem();
        toast(done ? t('sys.checked_update') : t('sys.check_timeout'));
    } catch (e) {
        toast(t('sys.network_error'));
    }
}

async function sysUpdateApply(kind, force) {
    const labels = { web: t('sys.update_web'), asset: t('sys.update_assets'), firmware: t('sys.update_firmware') };
    const name = labels[kind] || kind;
    toast(t(force ? 'sys.applying_force_update' : 'sys.applying_update', { name: name }));
    try {
        const r = await fetch('/api/update/apply', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ type: kind, force: !!force })
        });
        if (!r.ok) { toast(t('sys.failed')); return; }
        toast(t('sys.applied_update', { name: name }));
        if (kind === 'firmware') {
            sysOtaPoll();
        } else {
            /* Re-probe so the cached hint state reflects what we just applied
             * instead of waiting for the next hourly pass. */
            try { await fetch('/api/update/check', { method: 'POST' }); } catch (e) {}
            refreshUpdateBanner();
            setTimeout(loadSystem, 1200);
        }
    } catch (e) {
        toast(t('sys.network_error'));
    }
}

async function sysShowNotes(kind) {
    let note = '', title = '', meta = '';
    try {
        const hr = await fetch('/api/update/hints');
        if (hr.ok) {
            const h = await hr.json();
            const obj = (kind === 'firmware') ? (h.firmware || {}) : (h.web || {});
            note = obj.note || '';
            if (kind === 'firmware') {
                title = t('sys.card_firmware_version');
                meta = t('sys.channel_firmware');
            } else {
                title = t('sys.card_web_version');
                meta = t('sys.channel_web');
            }
            const ver = obj.version || '';
            if (ver) {
                title += ' · ' + t('sys.new_version', { v: ver });
                if (obj.current) title += ' (' + t('ov.current') + ' ' + obj.current + ')';
            }
        }
    } catch (e) {}
    const ttl = document.getElementById('update-notes-title');
    const mta = document.getElementById('update-notes-meta');
    const body = document.getElementById('update-notes-body');
    if (ttl) ttl.textContent = title;
    if (mta) mta.textContent = meta;
    if (body) body.textContent = note || t('sys.none');
    const m = document.getElementById('update-notes-modal');
    if (m) m.hidden = false;
}

function closeUpdateNotes() {
    const m = document.getElementById('update-notes-modal');
    if (m) m.hidden = true;
}
