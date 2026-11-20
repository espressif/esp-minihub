/* Adapter switches for the Config page.
 *
 * Users only decide which adapters are on. The full adapters.json document is
 * fetched from GET /api/config/adapters and sent back unchanged on save, so
 * fields the console does not show (broker users, bound devices, Modbus maps,
 * plugin settings...) are never lost. PUT's `reload` flag applies changes now.
 */
const ADAPTER_CFG_ORDER = ['wifi', 'mqtt_broker', 'thread', 'matter', 'zigbee', 'ble_mesh', 'modbus'];

/* Wi-Fi and BLE Mesh coexist; 802.15.4 (Zigbee or Thread) coexists with
 * neither, and Zigbee/Thread exclude each other. See docs/en/radios.rst. */
const ADAPTER_RADIO_802154 = ['zigbee', 'thread'];
const ADAPTER_RADIO_WIFI_GROUP = ['wifi', 'ble_mesh'];

let adaptersCfg = null;
let adaptersCfgLoadedOnce = false;
let adaptersSupport = {};

function adaptersCfgHtml(s) {
    return String(s == null ? '' : s)
        .replace(/&/g, '&amp;').replace(/</g, '&lt;')
        .replace(/>/g, '&gt;').replace(/"/g, '&quot;');
}

function adaptersCfgJs(s) {
    return String(s == null ? '' : s)
        .replace(/\\/g, '\\\\').replace(/'/g, "\\'")
        .replace(/&/g, '&amp;').replace(/</g, '&lt;')
        .replace(/>/g, '&gt;').replace(/"/g, '&quot;');
}

function adaptersCfgLabel(key) {
    const k = 'sys.adapter.' + key;
    const s = t(k);
    return s === k ? key : s;
}

/* /api/adapters carries a `supported` boolean derived from the firmware's
 * Kconfig guards. Older firmwares lack it: leave those selectable. */
async function adaptersCfgLoadSupport() {
    adaptersSupport = {};
    try {
        const r = await fetch('/api/adapters');
        if (!r.ok) return false;
        const j = await r.json();
        if (!j || typeof j !== 'object') return false;
        Object.keys(j).forEach(function (key) {
            const entry = j[key];
            if (entry && typeof entry === 'object' &&
                Object.prototype.hasOwnProperty.call(entry, 'supported')) {
                adaptersSupport[key] = !!entry.supported;
            }
        });
        return true;
    } catch (e) {
        return false;
    }
}

function adaptersCfgEnabledKeys(cfg) {
    const source = cfg || adaptersCfg;
    const out = [];
    if (!source || !source.adapters || typeof source.adapters !== 'object') return out;
    Object.keys(source.adapters).forEach(function (key) {
        const slot = source.adapters[key];
        /* Mirrors firmware: only an explicit JSON false disables an adapter. */
        if (slot && typeof slot === 'object' && !Array.isArray(slot) && slot.enabled !== false) {
            out.push(key);
        }
    });
    return out;
}

function adaptersCfgToggleConflict(key, cfg) {
    const others = adaptersCfgEnabledKeys(cfg).filter(function (k) { return k !== key; });
    const is154 = ADAPTER_RADIO_802154.indexOf(key) >= 0;
    const isWifiGroup = ADAPTER_RADIO_WIFI_GROUP.indexOf(key) >= 0;
    const other154 = others.some(function (k) { return ADAPTER_RADIO_802154.indexOf(k) >= 0; });
    const otherGroup = others.some(function (k) { return ADAPTER_RADIO_WIFI_GROUP.indexOf(k) >= 0; });
    if (is154 && other154) return t('sys.adapters.conflict_with_154');
    if ((is154 && otherGroup) || (isWifiGroup && other154)) return t('sys.adapters.conflict_with_group');
    return null;
}

/* Defensive check before saving: a config from an older firmware (or another
 * client) could already contain an impossible combination. */
function adaptersCfgValidate(cfg) {
    const enabled = adaptersCfgEnabledKeys(cfg);
    const has154 = enabled.filter(function (k) { return ADAPTER_RADIO_802154.indexOf(k) >= 0; });
    const hasGroup = enabled.filter(function (k) { return ADAPTER_RADIO_WIFI_GROUP.indexOf(k) >= 0; });
    const problems = [];
    if (has154.length > 1) problems.push(t('sys.adapters_conflict_802154'));
    if (has154.length && hasGroup.length) problems.push(t('sys.adapters_conflict_group'));
    const unsupported = enabled.filter(function (k) { return adaptersSupport[k] === false; });
    if (unsupported.length) {
        problems.push(t('sys.adapters_unsupported_enabled', { names: unsupported.join(', ') }));
    }
    return problems;
}

function adaptersCfgShape(cfg) {
    if (!cfg || typeof cfg !== 'object' || Array.isArray(cfg)) {
        return { adapters: {} };
    }
    if (!cfg.adapters || typeof cfg.adapters !== 'object' || Array.isArray(cfg.adapters)) {
        cfg.adapters = {};
    }
    return cfg;
}

function adaptersCfgSlot(key) {
    if (!adaptersCfg) adaptersCfg = adaptersCfgShape(null);
    const slot = adaptersCfg.adapters[key];
    if (!slot || typeof slot !== 'object' || Array.isArray(slot)) {
        adaptersCfg.adapters[key] = {};
    }
    return adaptersCfg.adapters[key];
}

function adaptersCfgRowHtml(key, slot) {
    const jsKey = adaptersCfgJs(key);
    const on = slot && typeof slot === 'object' && !Array.isArray(slot) && slot.enabled !== false;
    const unsupported = adaptersSupport[key] === false;
    const conflict = adaptersCfgToggleConflict(key, adaptersCfg);
    /* An already-on row must stay clickable so it can be switched off, even
     * when the saved document contains a conflict. */
    const blocked = !on && (unsupported || !!conflict);
    const why = unsupported ? t('sys.adapters_unsupported') : conflict;
    return '<label class="adapter-row' + (blocked ? ' blocked' : (unsupported ? ' unsupported' : '')) + '">' +
        '<input type="checkbox" ' + (on ? 'checked' : '') + (blocked ? ' disabled' : '') +
        ' onchange="adaptersCfgSetEnabled(\'' + jsKey + '\',this.checked)" />' +
        '<span class="adapter-name">' + adaptersCfgHtml(adaptersCfgLabel(key)) + '</span>' +
        (why ? '<span class="adapter-badge">' + adaptersCfgHtml(why) + '</span>' : '') +
        '</label>';
}

function adaptersCfgRender() {
    const host = document.getElementById('sys-adapters-list');
    if (!host || !adaptersCfg) return;
    const keys = Object.keys(adaptersCfg.adapters);
    keys.sort(function (a, b) {
        const ia = ADAPTER_CFG_ORDER.indexOf(a);
        const ib = ADAPTER_CFG_ORDER.indexOf(b);
        if (ia < 0 && ib < 0) return a.localeCompare(b);
        if (ia < 0) return 1;
        if (ib < 0) return -1;
        return ia - ib;
    });
    if (!keys.length) {
        host.innerHTML = '<div class="sys-hint">' + adaptersCfgHtml(t('sys.adapters_empty')) + '</div>';
    } else {
        host.innerHTML = keys.map(function (k) {
            return adaptersCfgRowHtml(k, adaptersCfg.adapters[k]);
        }).join('');
    }
    const warn = document.getElementById('sys-adapters-warn');
    if (warn) {
        const problems = adaptersCfgValidate(adaptersCfg);
        warn.textContent = problems.join(' ');
        warn.hidden = problems.length === 0;
    }
}

async function adaptersCfgLoad() {
    try {
        const r = await fetch('/api/config/adapters');
        if (!r.ok) {
            toast(t('sys.adapters_load_failed'));
            return false;
        }
        adaptersCfg = adaptersCfgShape(await r.json());
        await adaptersCfgLoadSupport();
        adaptersCfgLoadedOnce = true;
        adaptersCfgRender();
        return true;
    } catch (e) {
        toast(t('sys.network_error'));
        return false;
    }
}

function adaptersCfgSetEnabled(key, on) {
    /* Blocked rows are disabled in the form; keep this guard for stale events
     * so a click never opens a dialog. Draft state only - nothing is saved. */
    if (on && (adaptersSupport[key] === false || adaptersCfgToggleConflict(key, adaptersCfg))) {
        adaptersCfgRender();
        return;
    }
    /* Only touch `enabled` in the draft; every other field stays untouched. */
    adaptersCfgSlot(key).enabled = !!on;
    adaptersCfgRender();
}

async function adaptersCfgSave() {
    if (!adaptersCfg) {
        toast(t('sys.adapters_load_failed'));
        return false;
    }
    const problems = adaptersCfgValidate(adaptersCfg);
    if (problems.length) {
        toast(t('sys.adapters_conflict_save', { detail: problems.join(' ') }));
        return false;
    }
    const body = JSON.parse(JSON.stringify(adaptersCfg));
    /* One checkbox, one save button: changes always take effect immediately. */
    body.reload = true;
    try {
        const r = await fetch('/api/config/adapters', {
            method: 'PUT',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(body)
        });
        if (!r.ok) {
            toast(mhHttpErrorText(r.status, await r.text()));
            return false;
        }
        toast(t('sys.adapters_saved'));
        if (typeof loadAdapters === 'function') loadAdapters();
        await adaptersCfgLoad();
        return true;
    } catch (e) {
        toast(t('sys.network_error'));
        return false;
    }
}
