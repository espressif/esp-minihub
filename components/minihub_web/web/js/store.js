async function loadStoreData() {
    try {
        const [p, m] = await Promise.all([fetch('/api/packs'), fetch('/api/modules')].map(r => r.catch(() => null)));
        packs = p && p.ok ? await p.json() : [];
        modules = m && m.ok ? await m.json() : [];
        if (view === 'store') renderStore();
        if (view === 'overview') renderOverview();
        const c = await fetch('/api/store/catalog').catch(() => null);
        storeCatalogOk = !!(c && c.ok);
        storeCatalog = storeCatalogOk ? await c.json() : [];
        if (view === 'store') renderStore();
    } catch (e) {
        storeCatalogOk = false;
        storeCatalog = [];
        if (view === 'store') document.getElementById('store-content').innerHTML = '<div class="empty">' + t('store.load_failed') + '</div>';
    }
}
async function loadStoreConfig() {
    try {
        const r = await fetch('/api/store/config');
        if (!r.ok) throw new Error('HTTP ' + r.status);
        const j = await r.json();
        const u = document.getElementById('store-custom-url');
        const d = document.getElementById('store-disable-default');
        const h = document.getElementById('store-default-hint');
        if (u) u.value = j.custom_url || '';
        if (d) d.checked = !!j.disable_default;
        if (h) h.textContent = j.default_url ? t('store.default_url', { url: j.default_url }) : '';
    } catch (e) {
        console.warn('store config', e);
    }
}
async function saveStoreConfig() {
    const u = document.getElementById('store-custom-url');
    const d = document.getElementById('store-disable-default');
    const body = {
        custom_url: (u && u.value || '').trim(),
        disable_default: !!(d && d.checked)
    };
    try {
        const r = await fetch('/api/store/config', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify(body)
        });
        if (!r.ok) throw new Error('HTTP ' + r.status);
        await loadStoreConfig();
        toast(t('store.url_saved'));
    } catch (e) {
        toast(t('store.save_failed', { error: (e.message || e) }));
    }
}
async function refreshStoreCatalog() {
    const btn = document.getElementById('store-refresh');
    if (btn) btn.disabled = true;
    toast(t('store.refreshing'));
    try {
        const r = await fetch('/api/store/refresh', {
            method: 'POST'
        });
        if (!r.ok) {
            toast(t('store.refresh_server_failed'));
            return;
        }
        let status = await r.json();
        const deadline = Date.now() + 60000;
        while (status && status.result === 'pending' && Date.now() < deadline) {
            await new Promise(res => setTimeout(res, 500));
            const sr = await fetch('/api/store/refresh');
            if (!sr.ok) throw new Error('HTTP ' + sr.status);
            status = await sr.json();
        }
        if (status && status.result === 'aborted') {
            /* A saved store URL cancels the refresh that was fetching the old
               one; the catalog on flash is still the previous, valid copy. */
            toast(t('store.refresh_aborted'));
            return;
        }
        if (!status || status.result === 'pending' || status.result === 'error') {
            toast(t('store.refresh_server_failed'));
            return;
        }
        const cr = await fetch('/api/store/catalog');
        if (!cr.ok) throw new Error('HTTP ' + cr.status);
        storeCatalog = await cr.json();
        storeCatalogOk = true;
        /* A manual refresh re-syncs with the server: drop the optimistic
         * "just uninstalled" mask so anything the server still offers (now as
         * available, or re-installed) shows again. */
        if (window.uninstalledIds) window.uninstalledIds.clear();
        const [p, m] = await Promise.all([fetch('/api/packs'), fetch('/api/modules')].map(x => x.catch(() => null)));
        packs = p && p.ok ? await p.json() : packs;
        modules = m && m.ok ? await m.json() : modules;
        toast(t('store.catalog_updated', { count: storeCatalog.length }));
        if (view === 'store') renderStore();
    } catch (e) {
        toast(t('store.refresh_failed'));
    } finally {
        if (btn) btn.disabled = false;
    }
}

function renderStore() {
    if (!window.uninstalledIds) window.uninstalledIds = new Set();
    document.querySelectorAll('#store-tabs .chip').forEach(c => {
        c.classList.toggle('active', c.dataset.tab === storeTab);
    });
    const q = storeQ.toLowerCase();
    const filt = x => !q || (x.name && x.name.toLowerCase().includes(q)) || (x.id && x.id.toLowerCase().includes(q)) || (x.description && x.description.toLowerCase().includes(q)) || (x.vendor && x.vendor.toLowerCase().includes(q));
    const pl = packs.filter(filt),
        ml = modules.filter(filt);
    const catById = {};
    storeCatalog.forEach(x => {
        if (x.id) catById[x.id] = x;
    });
    /* "已安装" is built from three sources because no single one is complete:
     *   - storeCatalog items flagged installed: the on-disk truth (the firmware
     *     answers with module_installed()/pack_installed()),
     *   - /api/modules: every REGISTERED module, including builtin and factory
     *     preinstalled ones. Those are not store-installed, so the catalog does
     *     not flag them -- filtering this list by `installed` alone used to hide
     *     them from BOTH tabs, which made the page look empty after an uninstall.
     *   - /api/packs: installed config packs.
     * actBtns() already degrades to a "builtin" label when managed === false, so
     * an item with no store package shows up without install/uninstall actions. */
    const installedById = {};
    const addInstalled = (src, defaults) => {
        (src || []).forEach(it => {
            if (!it || !it.id) return;
            installedById[it.id] = Object.assign({ type: 'module' }, installedById[it.id], defaults, it);
        });
    };
    addInstalled(storeCatalog.filter(x => x.installed && !window.uninstalledIds.has(x.id)), { installed: true });
    addInstalled(pl, { type: 'pack' });
    /* /api/modules lists every slot in the loader's RAM table, and
     * minihub_module_delete() deliberately keeps the slot there until the next
     * hub reload rebuilds the table. Right after an uninstall it therefore still
     * reports the module as managed and enabled -- indistinguishable from an
     * installed one. minihub_module_list_json()'s own `installed` is a native
     * binary check, so it cannot be the tie-breaker on its own (false for a
     * JS-only module). The catalog's flag comes from module_installed(), a disk
     * check, so for anything the catalog lists it wins. Builtins (managed ===
     * false) have no store package that could go missing and stay listed, which
     * is what keeps factory-preinstalled modules from vanishing off the page. */
    const moduleKept = m => m.managed === false ||
        (catById[m.id] ? catById[m.id].installed !== false : m.installed !== false);
    addInstalled(ml.filter(moduleKept).filter(m => !window.uninstalledIds.has(m.id)), { type: 'module' });
    const installed = Object.values(installedById).map(it => {
        const c = catById[it.id];
        const up = !!(c && c.upgrade_available) || !!it.binary_stale;
        return Object.assign({}, it, c ? {
            type: c.type || it.type,
            upgrade_available: up,
            catalog_version: c.version,
            catalog_adapter_version: c.adapter_version,
            config_only: !!c.config_only
        } : {});
    }).sort((a, b) => (a.name || a.id || '').localeCompare(b.name || b.id || ''));
    /* Already on the device (builtin, preinstall, or installed) -> not offered.
     * Keyed on the union, not on the catalog's `installed` flag, which is false
     * for a builtin. */
    const avail = storeCatalog.filter(x => x.id && !installedById[x.id] &&
        (x.type === 'pack' || x.type === 'module')).filter(filt)
        .sort((a, b) => (a.name || a.id || '').localeCompare(b.name || b.id || ''));
    const kindLbl = (k, co) => co ? t('store.kind_config_module') : (k === 'module' ? t('store.kind_module') : t('store.kind_config_module'));
    const actBtns = (kind, id, enabled, managed, upgradeAvailable, configOnly) => {
        if (managed === false) return '<span style="color:var(--dim);font-size:.72rem">' + t('store.builtin') + '</span>';
        const e = esc(id);
        const apiKind = configOnly ? 'pack' : kind;
        const en = enabled ? '<button class="btn-sm" onclick="storeAct(\'' + apiKind + '\',\'' + e + '\',false)">' + t('store.disable') + '</button>' : '<button class="btn-sm on" onclick="storeAct(\'' + apiKind + '\',\'' + e + '\',true)">' + t('store.enable') + '</button>';
        let html = '<div class="store-actions">' + en;
        if (upgradeAvailable) {
            html += '<button class="btn-sm on" onclick="storeUpgrade(\'' + kind + '\',\'' + e + '\')">' + t('store.upgrade') + '</button>';
        }
        html += '<button class="btn-sm bad" onclick="storeUninstall(\'' + apiKind + '\',\'' + e + '\')">' + t('store.uninstall') + '</button>';
        html += '<button class="btn-sm" onclick="storeReinstall(\'' + apiKind + '\',\'' + e + '\')">' + t('store.reinstall') + '</button>';
        return html + '</div>';
    };
    const itemDesc = it => {
        if (it.type === 'pack' || it.config_only || it.inject_adapter) return (it.vendor ? esc(it.vendor) + ' → ' : '') + esc(it.description || (it.vendor ? it.id : '') || '-');
        return esc(it.description || ('adapter: ' + (it.adapter || '-')));
    };
    const verCell = it => {
        let s = esc(it.version || '?');
        if (it.adapter_version) s += ' · ' + esc(it.adapter_version);
        else if (it.adapter_version_expected) s += ' · expect ' + esc(it.adapter_version_expected);
        if (it.binary_stale) s += ' · ' + t('store.needs_upgrade');
        if (it.upgrade_available && it.catalog_version) s += ' → ' + esc(it.catalog_version);
        return s;
    };
    const itemRow = (it, extra) => {
        return '<div class="store-row"><div><div class="store-name">' + esc(it.name || it.id) + '</div>' + '<div class="store-desc">' + esc(it.id || '') + '</div></div>' + '<div class="store-ver">' + verCell(it) + '</div>' + '<div class="store-desc">' + itemDesc(it) + '</div>' + '<div>' + kindLbl(it.type, it.config_only) + '</div>' + '<div>' + esc(it.inject_adapter || it.adapter || '-') + '</div>' + extra + '</div>';
    };
    let html = '';
    if (storeTab === 'avail') {
        html += '<div class="section"><div class="section-h">' + t('store.available') + ' <span>' + avail.length + '</span></div>';
        if (!avail.length) {
            html += '<div class="empty" style="padding:24px">' + t('store.no_install_items') + '</div>';
        } else {
            html += '<div class="store-list"><div class="store-row store-hdr"><div>' + t('store.name') + '</div><div>' + t('store.version') + '</div><div>' + t('store.description') + '</div>        <div>' + t('store.type') + '</div><div>' + t('store.adapter') + '</div><div></div><div>' + t('store.actions') + '</div></div>';
            avail.forEach(it => {
                html += itemRow(it, '<div></div><div class="store-actions">        <button class="btn-sm on" onclick="storeInstall(\'' + esc(it.type) + '\',\'' + esc(it.id) + '\')">' + t('store.install') + '</button>        </div>');
            });
            html += '</div>';
        }
        html += '</div>';
    } else {
        html += '<div class="section"><div class="section-h">' + t('store.installed') + ' <span>' + installed.length + '</span></div>';
        html += '<div class="store-list"><div class="store-row store-hdr"><div>' + t('store.name') + '</div><div>' + t('store.version') + '</div><div>' + t('store.description') + '</div>      <div>' + t('store.type') + '</div><div>' + t('store.adapter') + '</div><div>' + t('store.status') + '</div><div>' + t('store.actions') + '</div></div>';
        installed.forEach(it => {
            html += itemRow(it, '<div class="' + (it.enabled ? 'tag-on' : 'tag-off') + '">' + (it.enabled ? t('store.enabled') : t('store.disabled')) + '</div>' + actBtns(it.type, it.id, it.enabled, it.managed !== false, it.upgrade_available, it.config_only));
        });
        if (!installed.length) html += '<div class="empty" style="padding:24px">' + t('store.no_installed_items') + '</div>';
        html += '</div></div>';
    }
    document.getElementById('store-content').innerHTML = html;
}
