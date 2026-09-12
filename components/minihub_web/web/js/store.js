/* MiniHub web — store page */
async function loadStoreData() {
    try {
        const [p, m] = await Promise.all([fetch('/api/packs'), fetch('/api/modules')].map(r => r.catch(() => null)));
        packs = p && p.ok ? await p.json() : [];
        modules = m && m.ok ? await m.json() : [];
        if (view === 'store') renderStore();
        if (view === 'overview') renderOverview(); /* Catalog separate — must not block installed packs/modules if remote store is down. */
        const c = await fetch('/api/store/catalog').catch(() => null);
        storeCatalogOk = !!(c && c.ok);
        storeCatalog = storeCatalogOk ? await c.json() : [];
        if (view === 'store') renderStore();
    } catch (e) {
        storeCatalogOk = false;
        storeCatalog = [];
        if (view === 'store') document.getElementById('store-content').innerHTML = '<div class="empty">加载失败</div>';
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
        if (h) h.textContent = j.default_url ? ('默认: ' + j.default_url) : '';
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
        toast('Store URL 已保存');
    } catch (e) {
        toast('保存失败: ' + (e.message || e));
    }
}
async function refreshStoreCatalog() {
    const btn = document.getElementById('store-refresh');
    if (btn) btn.disabled = true;
    toast('正在从商店服务器刷新目录...');
    try {
        const r = await fetch('/api/store/refresh', {
            method: 'POST'
        });
        if (!r.ok) {
            toast('无法从商店服务器刷新目录');
            return;
        }
        storeCatalog = await r.json();
        storeCatalogOk = true;
        const [p, m] = await Promise.all([fetch('/api/packs'), fetch('/api/modules')].map(x => x.catch(() => null)));
        packs = p && p.ok ? await p.json() : packs;
        modules = m && m.ok ? await m.json() : modules;
        toast('目录已更新（' + storeCatalog.length + ' 项）');
        if (view === 'store') renderStore();
    } catch (e) {
        toast('刷新失败');
    } finally {
        if (btn) btn.disabled = false;
    }
}

function renderStore() {
    document.querySelectorAll('#store-tabs .chip').forEach(c => {
        c.classList.toggle('active', c.dataset.tab === storeTab);
    });
    const q = storeQ.toLowerCase();
    const filt = x => !q || (x.name && x.name.toLowerCase().includes(q)) || (x.id && x.id.toLowerCase().includes(q)) || (x.description && x.description.toLowerCase().includes(q)) || (x.vendor && x.vendor.toLowerCase().includes(q));
    const pl = packs.filter(filt),
        ml = modules.filter(m => filt(m) && m.installed !== false);
    const catById = {};
    storeCatalog.forEach(x => {
        if (x.id) catById[x.id] = x;
    });
    const avail = storeCatalog.filter(x => !x.installed && (x.kind === 'pack' || x.kind === 'module')).filter(filt).sort((a, b) => (a.name || a.id || '').localeCompare(b.name || b.id || ''));
    const installed = [...pl.map(p => {
        const c = catById[p.id];
        return {
            ...p,
            kind: (c && c.kind) || 'pack',
            config_only: !!(c && c.config_only),
            name: p.name || p.id
        };
    }), ...ml.map(m => ({
        ...m,
        kind: 'module',
        name: m.name || m.id
    }))].map(it => {
        const c = catById[it.id];
        const up = !!(c && c.upgrade_available) || !!it.binary_stale;
        return c ? {
            ...it,
            upgrade_available: up,
            catalog_version: c.version,
            catalog_adapter_version: c.adapter_version,
            config_only: !!c.config_only
        } : it;
    }).sort((a, b) => (a.name || a.id || '').localeCompare(b.name || b.id || ''));
    const kindLbl = (k, co) => co ? '配置模块' : (k === 'module' ? '模块' : '配置模块');
    const actBtns = (kind, id, enabled, managed, upgradeAvailable, configOnly) => {
        if (managed === false) return '<span style="color:var(--dim);font-size:.72rem">内置</span>';
        const e = esc(id);
        const apiKind = configOnly ? 'pack' : kind;
        const en = enabled ? '<button class="btn-sm" onclick="storeAct(\'' + apiKind + '\',\'' + e + '\',false)">禁用</button>' : '<button class="btn-sm on" onclick="storeAct(\'' + apiKind + '\',\'' + e + '\',true)">启用</button>';
        let html = '<div class="store-actions">' + en;
        if (upgradeAvailable) {
            html += '<button class="btn-sm on" onclick="storeUpgrade(\'' + kind + '\',\'' + e + '\')">升级</button>';
        }
        html += '<button class="btn-sm bad" onclick="storeDel(\'' + apiKind + '\',\'' + e + '\')">删除</button>';
        return html + '</div>';
    };
    const itemDesc = it => {
        if (it.kind === 'pack' || it.config_only || it.inject_adapter) return (it.vendor ? esc(it.vendor) + ' → ' : '') + esc(it.description || (it.vendor ? it.id : '') || '-');
        return esc(it.description || ('adapter: ' + (it.adapter || '-')));
    };
    const verCell = it => {
        let s = esc(it.version || '?');
        if (it.adapter_version) s += ' · ' + esc(it.adapter_version);
        else if (it.adapter_version_expected) s += ' · expect ' + esc(it.adapter_version_expected);
        if (it.binary_stale) s += ' · 需升级';
        if (it.upgrade_available && it.catalog_version) s += ' → ' + esc(it.catalog_version);
        return s;
    };
    const itemRow = (it, extra) => {
        return '<div class="store-row"><div><div class="store-name">' + esc(it.name || it.id) + '</div>' + '<div class="store-desc">' + esc(it.id || '') + '</div></div>' + '<div class="store-ver">' + verCell(it) + '</div>' + '<div class="store-desc">' + itemDesc(it) + '</div>' + '<div>' + kindLbl(it.kind, it.config_only) + '</div>' + '<div>' + esc(it.inject_adapter || it.adapter || '-') + '</div>' + extra + '</div>';
    };
    let html = '';
    if (storeTab === 'avail') {
        html += '<div class="section"><div class="section-h">可用 <span>' + avail.length + '</span></div>';
        if (!avail.length) {
            html += '<div class="empty" style="padding:24px">暂无可安装项（均已安装或未预置）</div>';
        } else {
            html += '<div class="store-list"><div class="store-row store-hdr"><div>名称</div><div>版本</div><div>描述</div>        <div>类型</div><div>适配器</div><div></div><div>操作</div></div>';
            avail.forEach(it => {
                html += itemRow(it, '<div></div><div class="store-actions">        <button class="btn-sm on" onclick="storeInstall(\'' + esc(it.kind) + '\',\'' + esc(it.id) + '\')">安装</button>        </div>');
            });
            html += '</div>';
        }
        html += '</div>';
    } else {
        html += '<div class="section"><div class="section-h">已安装 <span>' + installed.length + '</span></div>';
        html += '<div class="store-list"><div class="store-row store-hdr"><div>名称</div><div>版本</div><div>描述</div>      <div>类型</div><div>适配器</div><div>状态</div><div>操作</div></div>';
        installed.forEach(it => {
            html += itemRow(it, '<div class="' + (it.enabled ? 'tag-on' : 'tag-off') + '">' + (it.enabled ? '已启用' : '未启用') + '</div>' + actBtns(it.kind, it.id, it.enabled, it.managed !== false, it.upgrade_available, it.config_only));
        });
        if (!installed.length) html += '<div class="empty" style="padding:24px">暂无已安装项</div>';
        html += '</div></div>';
    }
    document.getElementById('store-content').innerHTML = html;
}
