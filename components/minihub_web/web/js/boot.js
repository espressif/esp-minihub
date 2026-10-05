/* MiniHub web — startup */
window.mhWebBuild = '2026-10-02-no-window-stop-keepalive';
window.mhWebVersion = '0.1.0';

function toggleSecret(btn) {
    const field = btn && btn.closest ? btn.closest('.secret-field') : null;
    const input = field ? field.querySelector('input') : null;
    if (!input) return;
    const show = input.type === 'password';
    input.type = show ? 'text' : 'password';
    btn.textContent = show ? t('boot.hide') : t('boot.show');
    btn.setAttribute('aria-label', show ? t('boot.hide_password') : t('boot.show_password'));
}

(function installAuthFetchGuard() {
    const orig = window.fetch.bind(window);
    let redirecting = false;
    let checking = false;
    function shouldIgnore(url) {
        const u = String(url || '');
        return u.indexOf('/api/auth/login') >= 0 ||
            u.indexOf('/api/auth/setup') >= 0 ||
            u.indexOf('/api/auth/status') >= 0 ||
            u.indexOf('/login') >= 0;
    }
    function isLoginPath(url) {
        try {
            const path = new URL(url, location.origin).pathname;
            return path === '/login' || path.indexOf('/login/') === 0;
        } catch (e) {
            return false;
        }
    }
    function goLogin() {
        if (redirecting) {
            return;
        }
        if (isLoginPath(location.href)) {
            return;
        }
        redirecting = true;
        /* Hop to /login?reason=expired; handle_login_page() clears the session, so
         * there is no client-side /api/auth/logout (its response could be committed
         * as the document). window.stop() is deliberately absent: the browser drops
         * these requests when the navigation commits anyway. It was NOT the blank
         * login page — that was the server appending a second response after the
         * 401; see api_auth() in minihub_web.c. */
        location.replace('/login?reason=expired&r=' + Date.now());
    }
    function responseNeedsLogin(r, url) {
        if (!r || shouldIgnore(url)) {
            return false;
        }
        if (r.status === 401) {
            return true;
        }

        if (r.redirected && isLoginPath(r.url)) {
            return true;
        }
        return false;
    }
    function sessionEnded(j) {
        return !!(j && j.login_auth && !j.logged_in);
    }
    function checkSession() {
        if (redirecting || checking || isLoginPath(location.href)) {
            return;
        }
        checking = true;
        orig('/api/auth/status', { cache: 'no-store' }).then(function (r) {
            if (r && r.status === 401) {
                goLogin();
                return null;
            }
            if (!r || !r.ok) {
                return null;
            }
            return r.json();
        }).then(function (j) {
            if (sessionEnded(j)) {
                goLogin();
            }
        }).catch(function () {}).then(function () {
            checking = false;
        });
    }
    window.fetch = function (input, init) {
        const url = (typeof input === 'string') ? input : (input && input.url);
        return orig(input, init).then(function (r) {
            if (responseNeedsLogin(r, url)) {
                goLogin();
            }
            return r;
        });
    };
    checkSession();
    setInterval(checkSession, 10000);
    document.addEventListener('visibilitychange', function () {
        if (!document.hidden) {
            checkSession();
        }
    });
    window.addEventListener('pageshow', checkSession);
    window.addEventListener('focus', checkSession);
})();

document.querySelectorAll('.nav-item').forEach(n => n.onclick = () => switchView(n.dataset.view));
document.getElementById('menu-btn').onclick = () => document.getElementById('sidebar').classList.toggle('open');

document.getElementById('user-row').onclick = () => {
    const n = prompt(t('ov.name_prompt'), localName());
    if (n && n.trim()) {
        localStorage.setItem('minihub_display_name', n.trim());
        renderLocalUser();
    }
};

document.querySelectorAll('#store-tabs .chip').forEach(c => {
    c.onclick = () => {
        storeTab = c.dataset.tab;
        renderStore();
    };
});
window.addEventListener('hashchange', applyLoginHash);
/* A failed first hop (hub reload, reboot) used to leave the stats at "-" until
 * the 30 s adapters tick. Retry the chain, and say so in the footer — the 1 s
 * loadPoints run overwrites that line as soon as the Hub answers again. */
function bootConsole() {
    loadAdapters().then(async (ok) => {
        if (!ok) {
            const foot = document.getElementById('foot');
            if (foot) foot.textContent = t('boot.reloading');
            setTimeout(bootConsole, 2000);
            return;
        }
        if (typeof ensureDwCatalog === 'function') {
            try { await ensureDwCatalog(); } catch (e) {}
        }
        await loadPoints();
        loadModulesForAuth().then(() => {
            loadMeta();
            applyLoginHash();
        });
        loadRtspCamStatusSafe();
        loadStoreData();
    });
}
/* Wait for the dictionary before the first render, otherwise the console paints in
 * one language and swaps a frame later. A rejected dictionary still boots: the
 * helper falls back to the key text and the page is already visible by then. */
function startConsolePolling() {
    /* 1 s: drives the 更新 stamp and is also the fast path for noticing a session
     * that a newer login replaced (single-slot session → 401 → guard → /login). */
    setInterval(loadPoints, 1000);
    setInterval(loadRtspCamStatusSafe, 12000);
    setInterval(() => loadAdapters().then(async () => {
        await loadModulesForAuth();
        await loadMeta();
    }), 30000);
    setInterval(() => {
        if (!(adapterEnabled('zigbee') || adapterEnabled('ble_mesh'))) return;
        refreshRadioProv();
    }, 5000);
    connectEventsWs();
}

window.i18nReady.catch(function () {}).then(function () {
    /* The console document is only served to a live session. Start device
     * loads immediately; a 401 still hops to /login via the fetch guard. */
    bootConsole();
    startConsolePolling();
});
renderLocalUser();

function connectEventsWs() {
    let ws;
    let retryMs = 2000;
    let pointsTimer = null;
    let adaptersTimer = null;
    let storeTimer = null;
    function scheduleLoadPoints() {
        if (pointsTimer) {
            return;
        }
        pointsTimer = setTimeout(function () {
            pointsTimer = null;
            if (typeof loadPoints === 'function') {
                loadPoints();
            }
        }, 450);
    }
    function scheduleLoadAdapters() {
        if (adaptersTimer) {
            return;
        }
        adaptersTimer = setTimeout(function () {
            adaptersTimer = null;
            if (typeof loadAdapters === 'function') {
                loadAdapters();
            }
        }, 450);
    }
    function scheduleLoadStore() {
        if (storeTimer) {
            return;
        }
        storeTimer = setTimeout(function () {
            storeTimer = null;
            if (typeof loadStoreData === 'function') {
                loadStoreData();
            }
        }, 450);
    }
    function connect() {
        try {
            const proto = (location.protocol === 'https:') ? 'wss://' : 'ws://';
            ws = new WebSocket(proto + location.host + '/ws/events');
        } catch (e) {
            setTimeout(connect, retryMs);
            return;
        }
        ws.onopen = function () { retryMs = 2000; };
        ws.onmessage = function (ev) {
            try {
                const msg = JSON.parse(ev.data);
                if (!msg) return;
                /* hub.reloaded = a full reload finished, so the whole point set
                 * may have changed. Removals (uninstall/disable) publish no
                 * point.updated of their own, so this is the only signal.
                 * The store list needs it too: /api/modules serves REGISTERED
                 * modules, so an uninstalled one lingered there (and therefore
                 * in the "installed" tab) long after it was off disk. */
                if (msg.event_type === 'point.updated' || msg.event_type === 'hub.reloaded') {
                    scheduleLoadPoints();
                }
                if (msg.event_type === 'device_widgets.updated' || msg.event_type === 'hub.reloaded') {
                    scheduleLoadAdapters();
                }
                if (msg.event_type === 'hub.reloaded' || msg.event_type === 'store.module.installed') {
                    scheduleLoadStore();
                }
            } catch (e) {}
        };
        ws.onclose = function () {
            setTimeout(connect, retryMs);
            if (retryMs < 15000) retryMs *= 1.5;
        };
        ws.onerror = function () {
            try { ws.close(); } catch (e) {}
        };
    }
    connect();
}


const UPDATE_HINT_DISMISS_KEY = 'mhUpdateHintDismiss';

function updateHintSignature(data) {
    const fw = (data && data.firmware && data.firmware.version) ? String(data.firmware.version) : '';
    const web = (data && data.web && data.web.version) ? String(data.web.version) : '';
    const assets = (data && data.assets && data.assets.pending > 0) ? ('a' + String(data.assets.pending)) : '';
    const plugins = ((data && data.plugins) || []).map(function (p) {
        const id = (p && (p.id || p.name)) || '';
        return String(id) + '@' + String((p && p.version) || '');
    }).sort();
    return [fw, web, assets, plugins.join(',')].join('|');
}

function renderUpdateHint(data) {
    const el = document.getElementById('update-hint');
    const text = document.getElementById('update-hint-text');
    if (!el || !text) return;
    const parts = [];
    const fw = data && data.firmware;
    if (fw && fw.version) {
        parts.push(t('sys.hint_firmware', { version: fw.version }) +
            (fw.current ? t('sys.hint_installed', { version: fw.current }) : ''));
    }
    const plugins = (data && data.plugins) || [];
    plugins.forEach(function (p) {
        const name = (p && (p.name || p.id)) || t('sys.hint_plugin_default');
        const ver = (p && p.version) || '';
        parts.push(t('sys.hint_plugin', { name: name, version: ver }) +
            (p && p.installed ? t('sys.hint_installed', { version: p.installed }) : ''));
    });
    const web = data && data.web;
    if (web && web.version) {
        parts.push(t('sys.hint_web', { version: web.version }) +
            (web.current ? t('sys.hint_installed', { version: web.current }) : ''));
    }
    const assets = data && data.assets;
    if (assets && assets.pending > 0) {
        parts.push(t('sys.hint_assets', { n: assets.pending }));
    }
    if (!parts.length) {
        el.hidden = true;
        text.textContent = '';
        delete el.dataset.sig;
        return;
    }
    const sig = updateHintSignature(data);
    let dismissed = '';
    try { dismissed = localStorage.getItem(UPDATE_HINT_DISMISS_KEY) || ''; } catch (e) {}
    if (dismissed === sig) {
        el.hidden = true;
        text.textContent = '';
        delete el.dataset.sig;
        return;
    }
    text.textContent = parts.join(t('sys.hint_sep')) + t('sys.hint_sep') + t('sys.hint_manual');
    el.dataset.sig = sig;
    el.hidden = false;
}

function dismissUpdateHint() {
    const el = document.getElementById('update-hint');
    if (!el || !el.dataset.sig) return;
    try { localStorage.setItem(UPDATE_HINT_DISMISS_KEY, el.dataset.sig); } catch (e) {}
    el.hidden = true;
}

async function pollUpdateHint() {
    try {
        /* t() returns the raw key until /web/i18n/*.json lands, and this runs at
         * parse time (before i18n.js's fetch settles). Compose the banner only
         * after the dictionary is in, or it sticks as "sys.hint_web…" until the
         * next 60s tick (the i18n-pending reveal can beat the fetch). */
        if (window.i18nReady) {
            try { await window.i18nReady; } catch (e) {}
        }
        const r = await fetch('/api/update/hints');
        if (!r.ok) return;
        renderUpdateHint(await r.json());
    } catch (e) {}
}

const updateHintClose = document.getElementById('update-hint-x');
if (updateHintClose) updateHintClose.addEventListener('click', dismissUpdateHint);

pollUpdateHint();
setInterval(pollUpdateHint, 60000);
