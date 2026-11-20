'use strict';
/*
 * The adapter switches edit the document served by GET/PUT
 * /api/config/adapters. Run the real module in a vm so the enable semantics,
 * capability filtering, radio exclusions, and field preservation are checked
 * without needing a device.
 */
const { test } = require('node:test');
const assert = require('node:assert');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const WEB_JS = process.env.MINIHUB_WEB_JS;
const SOURCE = path.join(WEB_JS, 'adapters_config.js');

function fakeEl() {
    return {
        innerHTML: '', textContent: '', value: '', hidden: false, checked: false,
        dataset: {}, style: {},
        classList: { add() {}, remove() {}, toggle() {}, contains() { return false; } },
        setAttribute() {}, getAttribute() { return null; },
        addEventListener() {}, removeEventListener() {},
        querySelector() { return null; }, querySelectorAll() { return []; },
    };
}

function adaptersSeed() {
    return {
        adapters: {
            wifi: { enabled: true },
            mqtt_broker: { port: 1884, users: [{ name: 'a', pass: 'b' }] },
            thread: { enabled: false, dataset: null },
            my_store_adapter: { enabled: true, devices: [{ point_id: 'x' }] },
        },
    };
}

function adaptersOverview() {
    return {
        wifi: { enabled: true, supported: true },
        mqtt_broker: { enabled: false, supported: true },
        thread: { enabled: false, supported: false },
        zigbee: { enabled: false, supported: true },
        ble_mesh: { enabled: false, supported: true },
        matter: { enabled: false, supported: true },
        modbus: { enabled: false, supported: true },
    };
}

function boot() {
    const els = new Map();
    const byId = (id) => {
        if (!els.has(id)) els.set(id, fakeEl());
        return els.get(id);
    };
    const toasts = [];
    const puts = [];
    let getPayload = adaptersSeed();
    let overviewPayload = adaptersOverview();
    let putResult = { ok: true, status: 200, text: async () => '' };
    let adapterRefreshCalls = 0;
    const sandbox = {
        console,
        document: { getElementById: byId, activeElement: null },
        t: (key) => key,
        toast: (m) => toasts.push(m),
        mhHttpErrorText: (status, raw) => `${status}:${raw}`,
        loadAdapters: async () => { adapterRefreshCalls++; },
        fetch: async (url, opts) => {
            const u = String(url);
            if (!opts || !opts.method) {
                const payload = u.split('?')[0] === '/api/adapters' ? overviewPayload : getPayload;
                return { ok: true, status: 200, json: async () => JSON.parse(JSON.stringify(payload)) };
            }
            if (opts.method === 'PUT') {
                puts.push({ url: u, body: JSON.parse(opts.body) });
                return putResult;
            }
            throw new Error('unexpected request ' + u);
        },
        setTimeout: () => 0, clearTimeout: () => {},
    };
    sandbox.window = sandbox;
    const ctx = vm.createContext(sandbox);
    vm.runInContext(fs.readFileSync(SOURCE, 'utf8'), ctx, { filename: 'adapters_config.js' });
    const run = (code) => vm.runInContext(code, ctx);
    return {
        ctx, els, toasts, puts, run,
        setGet: (p) => { getPayload = p; },
        setOverview: (p) => { overviewPayload = p; },
        setPutResult: (p) => { putResult = p; },
        refreshCalls: () => adapterRefreshCalls,
    };
}

test('shape tolerates a missing or malformed adapters root', () => {
    const b = boot();
    assert.deepEqual(b.run('adaptersCfgShape(null)'), { adapters: {} });
    assert.deepEqual(b.run('adaptersCfgShape({adapters: []})'), { adapters: {} });
});

test('load renders one checkbox row per adapter, with no JSON editor', async () => {
    const b = boot();
    assert.equal(await b.run('adaptersCfgLoad()'), true);
    const rows = b.els.get('sys-adapters-list').innerHTML;
    assert.equal((rows.match(/type="checkbox"/g) || []).length, 4);
    assert.match(rows, /mqtt_broker/);
    assert.match(rows, /my_store_adapter/);
    assert.doesNotMatch(rows, /textarea/);
});

test('only an explicit false disables an adapter', async () => {
    const b = boot();
    await b.run('adaptersCfgLoad()');
    const rows = b.els.get('sys-adapters-list').innerHTML;
    assert.match(rows, /checked[^>]*onchange="adaptersCfgSetEnabled\('mqtt_broker'/);
});

test('unsupported adapters are badged, disabled and cannot be enabled', async () => {
    const b = boot();
    await b.run('adaptersCfgLoad()');
    const rows = b.els.get('sys-adapters-list').innerHTML;
    assert.match(rows, /adapter-badge/);
    assert.match(rows, /sys.adapters_unsupported/);
    assert.match(rows, /disabled/);
    b.run("adaptersCfgSetEnabled('thread', true)");
    assert.notEqual(b.run("adaptersCfg.adapters.thread.enabled"), true);
    assert.equal(b.toasts.length, 0);
});

test('radio groups exclude each other and Zigbee/Thread are exclusive', async () => {
    const b = boot();
    const ov = adaptersOverview();
    ov.thread.supported = true;
    b.setOverview(ov);
    await b.run('adaptersCfgLoad()');

    const rows = b.els.get('sys-adapters-list').innerHTML;
    assert.match(rows, /adapter-row blocked/);
    assert.match(rows, /sys.adapters.conflict_with_group/);
    b.run("adaptersCfgSetEnabled('zigbee', true)");
    assert.notEqual(b.run("adaptersCfg.adapters.zigbee && adaptersCfg.adapters.zigbee.enabled"), true);
    assert.equal(b.toasts.length, 0);

    b.run("adaptersCfgSetEnabled('wifi', false)");
    b.run("adaptersCfgSetEnabled('zigbee', true)");
    assert.equal(b.run("adaptersCfg.adapters.zigbee.enabled"), true);
    b.run("adaptersCfgSetEnabled('thread', true)");
    assert.notEqual(b.run("adaptersCfg.adapters.thread.enabled"), true);
    assert.equal(b.toasts.length, 0);

    b.run("adaptersCfgSetEnabled('zigbee', false)");
    b.run("adaptersCfgSetEnabled('wifi', true)");
    b.run("adaptersCfgSetEnabled('ble_mesh', true)");
    assert.equal(b.run("adaptersCfg.adapters.ble_mesh.enabled"), true);
});

test('save keeps every non-enabled field and always reloads', async () => {
    const b = boot();
    await b.run('adaptersCfgLoad()');
    b.run("adaptersCfgSetEnabled('mqtt_broker', true)");
    assert.equal(await b.run('adaptersCfgSave()'), true);
    assert.equal(b.puts.length, 1);
    assert.equal(b.puts[0].url, '/api/config/adapters');
    assert.equal(b.puts[0].body.reload, true);
    assert.equal(b.puts[0].body.adapters.mqtt_broker.enabled, true);
    assert.equal(b.puts[0].body.adapters.mqtt_broker.port, 1884);
    assert.deepEqual(b.puts[0].body.adapters.mqtt_broker.users, [{ name: 'a', pass: 'b' }]);
    assert.deepEqual(b.puts[0].body.adapters.my_store_adapter.devices, [{ point_id: 'x' }]);
    assert.equal(b.refreshCalls(), 1);
});

test('save refuses an impossible config loaded from the device', async () => {
    const b = boot();
    const cfg = adaptersSeed();
    cfg.adapters.zigbee = { enabled: true };
    b.setGet(cfg);
    await b.run('adaptersCfgLoad()');
    assert.equal(await b.run('adaptersCfgSave()'), false);
    assert.equal(b.puts.length, 0);
    assert.match(b.els.get('sys-adapters-warn').textContent, /sys.adapters_conflict_group/);
});

test('missing capability flags keep every option selectable on older firmware', async () => {
    const b = boot();
    b.setOverview({ wifi: { enabled: true } });
    assert.equal(await b.run('adaptersCfgLoad()'), true);
    assert.doesNotMatch(b.els.get('sys-adapters-list').innerHTML, /sys.adapters_unsupported/);
    assert.equal(b.run("adaptersCfgValidate({adapters:{zigbee:{enabled:true}}})").length, 0);
});

test('save refusal surfaces the device error and does not claim success', async () => {
    const b = boot();
    await b.run('adaptersCfgLoad()');
    b.setPutResult({ ok: false, status: 400, text: async () => 'missing adapters object' });
    assert.equal(await b.run('adaptersCfgSave()'), false);
    assert.match(b.toasts.join('|'), /400:missing adapters object/);
});
