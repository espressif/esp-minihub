/* MiniHub web — startup */
window.mhWebBuild = '2026-09-26-1s-refresh';

/* Session gone → /login. A 401 is not enough: fetch follows a 302 to /login
 * and the final status is 200, and an already-open console keeps its last
 * paint until something notices logged_in is false. */
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
        location.replace('/login');
    }
    function responseNeedsLogin(r, url) {
        if (!r || shouldIgnore(url)) {
            return false;
        }
        if (r.status === 401) {
            return true;
        }
        /* 302 /login followed by fetch: document stays on the console. */
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
    const n = prompt('设置本地显示名称', localName());
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
}); /* Adapters → modules (auth UI) → status → points. Catalog is non-blocking. */
window.addEventListener('hashchange', applyLoginHash);
loadAdapters().then(async () => {
    await loadPoints();
    loadModulesForAuth().then(() => {
        loadMeta();
        applyLoginHash();
    });
    loadRtspCamStatusSafe();
    loadStoreData();
});
renderLocalUser();
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

/* Push path: coalesce point.updated bursts (soft reload registers many points). */
(function connectEventsWs() {
    let ws;
    let retryMs = 2000;
    let pointsTimer = null;
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
                if (msg && msg.event_type === 'point.updated') {
                    scheduleLoadPoints();
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
})();
