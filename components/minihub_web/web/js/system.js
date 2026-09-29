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

function fmtChip(t) {
    if (!t) return '-';
    const L = {
        binary_sensor: '门窗报警器',
        smoke: '烟感',
        router: '路由器',
        car: '汽车'
    };
    return L[t] || String(t).toUpperCase();
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
        note.textContent = '此固件未启用该接口';
        note.hidden = false;
    } else if (!linked) {
        note.textContent = (sysIface === 'wifi' && st.ssid) ? '连接中…' : '未连接';
        note.hidden = false;
    } else {
        note.textContent = '当前 ' + st.ip;
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
            eye.textContent = '显示';
            eye.setAttribute('aria-label', '显示密码');
        }
    }
    sysToggleDhcp();
}

let otaTimer = null;
const OTA_STATE = ['空闲', '升级中', '成功', '失败'];

function sysOtaRender(j) {
    const st = document.getElementById('sys-ota-status');
    const bar = document.getElementById('sys-ota-bar');
    if (!st || !bar) return;
    const state = j.state | 0;
    const prog = j.progress;
    const src = j.source || 'none';
    const err = j.error || '';
    let line = '状态：' + (OTA_STATE[state] || state) + ' · 来源 ' + src;
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
    if (state === 2) toast('升级成功，即将重启');
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
        toast('请填写固件 URL');
        return;
    }
    toast('开始 URL 升级…');
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
            toast('升级忙碌中');
            return;
        }
        if (!r.ok) {
            toast('启动失败');
            return;
        }
        toast('已开始下载');
        sysOtaPoll();
    } catch (e) {
        toast('网络错误');
    }
}
async function sysOtaUpload() {
    const inp = document.getElementById('sys-ota-file');
    const f = inp && inp.files && inp.files[0];
    if (!f) {
        toast('请选择 .bin 文件');
        return;
    }
    toast('上传中 ' + f.name + '…');
    try {
        const r = await fetch('/api/ota/upload', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/octet-stream'
            },
            body: f
        });
        if (r.status === 409) {
            toast('升级忙碌中');
            return;
        }
        if (!r.ok) {
            const t = await r.text().catch(() => '');
            toast('上传失败 ' + (t || r.status));
            sysOtaPoll();
            return;
        }
        toast('上传完成，即将重启');
        sysOtaPoll();
    } catch (e) {
        toast('上传错误');
        sysOtaPoll();
    }
}

async function loadSystem() {
    try {
        const r = await fetch('/api/system');
        if (!r.ok) {
            toast('加载系统信息失败');
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
        grid.innerHTML = '<div class="sys-card"><h4>固件版本</h4><div class="v">' + esc(ver) + '</div>' + (idfLine ? '<div class="s">' + esc(idfLine) + '</div>' : '') + (commitLine ? '<div class="s">' + esc(commitLine) + '</div>' : '') + '</div>' + sysCard('芯片类型', fmtChip(sysSnap.chip), [sysSnap.chip_cores != null ? ('Cores: ' + sysSnap.chip_cores) : '', sysSnap.chip_revision != null ? ('Revision: ' + sysSnap.chip_revision) : ''].filter(Boolean).join(', ')) + sysCard('Flash', fmtBytes(sysSnap.flash_size), '总容量') + sysCard('内部 RAM', fmtBytes(ram.internal_free), '共 ' + fmtBytes(ram.internal_total)) + sysCard('PSRAM', fmtBytes(ram.psram_free), '共 ' + fmtBytes(ram.psram_total)) + sysCard('Storage', fmtBytes(st.free), '剩余 · 共 ' + fmtBytes(st.total) + ' · 已用 ' + fmtBytes(st.used)) + sysCard('Modstore', fmtBytes(ms.free), '剩余 · 共 ' + fmtBytes(ms.total) + ' · 已用 ' + fmtBytes(ms.used)) + sysCard('运行时间', (Number(sysSnap.uptime_s) || 0) + ' s', '');
        const hv = document.getElementById('hub-ver');
        if (hv && sysSnap.version) hv.textContent = 'v' + sysSnap.version;
        sysFillNet();
    } catch (e) {
        toast('网络错误');
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
        toast('请填写 IP / 掩码 / 网关');
        return;
    }
    toast('正在应用…');
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
            toast('已应用');
            if (!dhcp && body.ip && body.ip !== location.hostname) {
                toast('请改用 http://' + body.ip + '/');
            }
            setTimeout(loadSystem, 800);
        } else {
            toast('失败' + (j.error ? ': ' + j.error : ''));
        }
    } catch (e) {
        toast('网络错误（若已改 IP，请用新地址打开）');
    }
}

async function sysReboot() {
    if (!window.confirm('重启 MiniHub？配置和设备数据都会保留。')) {
        return;
    }
    toast('正在重启…');
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
            toast('设备即将重启');
        } else {
            toast('失败' + (j.error ? ': ' + j.error : ''));
        }
    } catch (e) {
        toast('若设备已重启，请稍后重新打开页面');
    }
}

async function sysApplyWifi() {
    const ssid = (document.getElementById('sys-wifi-ssid').value || '').trim();
    const password = document.getElementById('sys-wifi-pass').value || '';
    const open = document.getElementById('sys-wifi-open').checked;
    if (!ssid) {
        toast('请填写 SSID');
        return;
    }
    if (!window.confirm('保存后设备会重连到「' + ssid + '」。当前页面可能会断开，继续？')) {
        return;
    }
    toast('正在保存 Wi-Fi…');
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
            toast('已保存，设备正在重连');
            document.getElementById('sys-wifi-pass').value = '';
            /* GOT_IP is often 3–5 s after reconnect; one early refresh stays blank. */
            for (let i = 0; i < 12; i++) {
                await new Promise(res => setTimeout(res, 1000));
                await loadSystem();
                const st = sysSnap && sysSnap.net && sysSnap.net.wifi;
                if (st && st.ip) {
                    toast('Wi-Fi 已连接 ' + st.ip);
                    return;
                }
            }
            toast('仍未拿到地址，可点刷新');
        } else {
            toast('失败' + (j.error ? ': ' + j.error : ''));
        }
    } catch (e) {
        toast('网络已断开的话，请连上新的 Wi-Fi 再打开控制台');
    }
}
