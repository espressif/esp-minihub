let sysIface = 'eth',
    sysSnap = null;

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
        if (!otaTimer) otaTimer = setInterval(sysOtaPoll, 1000);
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

async function loadSystem() {
    try {
        const r = await fetch('/api/system');
        if (!r.ok) {
            toast(t('sys.load_failed'));
            return;
        }
        sysSnap = await r.json();
        const ram = sysSnap.ram || {};
        const st = sysSnap.storage || {};
        const ms = sysSnap.modstore || {};
        const grid = document.getElementById('sys-info-grid');
        const ver = 'v' + (sysSnap.version || '0.1.0');
        const idfLine = sysSnap.idf_version ? ('IDF: ' + sysSnap.idf_version) : '';
        const commitLine = sysSnap.commit ? ('MiniHub: ' + sysSnap.commit) : '';
        grid.innerHTML = '<div class="sys-card"><h4>' + esc(t('sys.firmware_version')) + '</h4><div class="v">' + esc(ver) + '</div>' + (idfLine ? '<div class="s">' + esc(idfLine) + '</div>' : '') + (commitLine ? '<div class="s">' + esc(commitLine) + '</div>' : '') + '</div>' + sysCard(t('sys.chip_type'), fmtChip(sysSnap.chip), [sysSnap.chip_cores != null ? ('Cores: ' + sysSnap.chip_cores) : '', sysSnap.chip_revision != null ? ('Revision: ' + sysSnap.chip_revision) : ''].filter(Boolean).join(', ')) + sysCard('Flash', fmtBytes(sysSnap.flash_size), t('sys.total_capacity')) + sysCard(t('sys.internal_ram'), fmtBytes(ram.internal_free), t('sys.total_value', { total: fmtBytes(ram.internal_total) })) + sysCard('PSRAM', fmtBytes(ram.psram_free), t('sys.total_value', { total: fmtBytes(ram.psram_total) })) + sysCard('Storage', fmtBytes(st.free), t('sys.free_total_used', { free: fmtBytes(st.free), total: fmtBytes(st.total), used: fmtBytes(st.used) })) + sysCard('Modstore', fmtBytes(ms.free), t('sys.free_total_used', { free: fmtBytes(ms.free), total: fmtBytes(ms.total), used: fmtBytes(ms.used) })) + sysCard(t('sys.uptime'), (Number(sysSnap.uptime_s) || 0) + ' s', '');
        const hv = document.getElementById('hub-ver');
        if (hv && sysSnap.version) hv.textContent = 'v' + sysSnap.version;
        sysFillNet();
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
                await new Promise(res => setTimeout(res, 1000));
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
