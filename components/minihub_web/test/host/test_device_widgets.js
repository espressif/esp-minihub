'use strict';
/*
 * The console's power control is a catalog question, not a type list: a type
 * whose catalog row offers a writable `power` gets the Overview toggle and the
 * map click, and a type added to model/devices.csv gets them with no JS edit.
 * These tests drive the real cardHtml() / paintMapDevices() in a vm, against the
 * catalog the firmware generator actually produces.
 */
const { test } = require('node:test');
const assert = require('node:assert');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const WEB_JS = process.env.MINIHUB_WEB_JS;
const WEB_ROOT = path.join(WEB_JS, '..');
const CATALOG = JSON.parse(fs.readFileSync(process.env.MINIHUB_TEST_CATALOG, 'utf8'));
/* index.html order, minus boot.js (it wires the page chrome, not the cards). */
const SOURCES = ['config.js', 'i18n.js', 'app.js', 'map.js', 'overview.js', 'device_widgets.js'];
const TOGGLE = '<label class="toggle"><input type="checkbox" data-id="';

function fakeEl() {
    const el = {
        style: { setProperty() {} },
        dataset: {},
        classList: { add() {}, remove() {}, toggle() {}, contains() { return false; } },
        innerHTML: '', textContent: '', value: '', hidden: false, checked: false,
        appendChild(c) { return c; }, removeChild() {}, replaceChild() {}, insertBefore() {},
        setAttribute() {}, getAttribute() { return null; }, removeAttribute() {},
        addEventListener() {}, removeEventListener() {},
        querySelector() { return null; }, querySelectorAll() { return []; },
        closest() { return null; }, contains() { return false; },
        focus() {}, blur() {}, click() {}, remove() {}, scrollTo() {},
        getBoundingClientRect() { return { top: 0, left: 0, width: 0, height: 0 }; },
    };
    return el;
}

function bootConsole(catalog) {
    const els = new Map();
    const posts = [];
    const byId = (id) => {
        if (!els.has(id)) els.set(id, fakeEl());
        return els.get(id);
    };
    const document = {
        getElementById: byId,
        createElement: () => fakeEl(),
        querySelector: () => null,
        querySelectorAll: () => [],
        addEventListener() {}, removeEventListener() {},
        body: fakeEl(), head: fakeEl(), documentElement: fakeEl(),
        activeElement: null, hidden: false,
    };
    const store = new Map();
    const ok = (payload) => ({ ok: true, status: 200, json: async () => payload, text: async () => JSON.stringify(payload) });
    const fetchStub = async (url, opts) => {
        const u = String(url);
        if (opts && opts.method === 'POST') {
            posts.push({ url: u.split('?')[0], body: JSON.parse(opts.body) });
            return ok({ ok: true });
        }
        const p = u.split('?')[0];
        if (p === '/api/device_widgets') return ok(catalog);
        if (p.startsWith('/web/i18n/')) {
            const f = path.join(WEB_ROOT, 'i18n', path.basename(p));
            if (fs.existsSync(f)) return ok(JSON.parse(fs.readFileSync(f, 'utf8')));
        }
        if (p === '/api/points') return ok({ points: [] });
        return { ok: false, status: 404, json: async () => ({}), text: async () => '' };
    };
    const sandbox = {
        console, document, fetch: fetchStub,
        /* Timers are recorded, never scheduled: a pending refresh must not keep
         * the test process alive or fire after the assertions have run. */
        setTimeout: () => 0, clearTimeout: () => {}, setInterval: () => 0, clearInterval: () => {},
        requestAnimationFrame: () => 0, cancelAnimationFrame: () => {},
        localStorage: {
            getItem: (k) => (store.has(k) ? store.get(k) : null),
            setItem: (k, v) => store.set(k, String(v)),
            removeItem: (k) => store.delete(k),
        },
        navigator: { language: 'en', languages: ['en'], userAgent: 'node' },
        location: { href: 'http://hub.local/', hash: '', reload() {} },
    };
    sandbox.window = sandbox;
    const ctx = vm.createContext(sandbox);
    for (const name of SOURCES) {
        vm.runInContext(fs.readFileSync(path.join(WEB_JS, name), 'utf8'), ctx, { filename: name });
    }
    /* Parse inside the realm: a host array would fail Array.isArray in the console. */
    vm.runInContext(`dwCatalog = ${JSON.stringify(catalog)};`, ctx);
    return { ctx, posts, run: (code) => vm.runInContext(code, ctx) };
}

function point(over) {
    return Object.assign({
        point_id: 'ble_mesh.bm_0001', type: 'light', adapter: 'ble_mesh',
        online: true, value: 'on', props: { params: { power: { value: 'on' } } },
    }, over);
}

function cardHasToggle(c, p) {
    return c.run(`cardHtml(${JSON.stringify(p)})`).includes(TOGGLE);
}

/* A type nobody has written JS for: the catalog alone must be enough. */
function withNewType(base, device, paramName) {
    const cat = JSON.parse(JSON.stringify(base));
    const power = JSON.parse(JSON.stringify(cat.parameters.power));
    power.parameter = paramName;
    cat.devices.push({ device, image: '', params: [power] });
    return cat;
}

test('a light card keeps its power toggle', () => {
    const c = bootConsole(CATALOG);
    assert.strictEqual(cardHasToggle(c, point()), true);
});

test('a speaker card offers the power toggle its catalog row declares', () => {
    const c = bootConsole(CATALOG);
    const p = point({
        point_id: 'sim.js_speaker', type: 'speaker', adapter: 'sim',
        props: { params: { power: { value: 'on' }, volume: { value: 40 } } },
    });
    assert.strictEqual(cardHasToggle(c, p), true);
});

test('a type added to the catalog gets a toggle with no JS edit', () => {
    const c = bootConsole(withNewType(CATALOG, 'outlet', 'power'));
    const p = point({ point_id: 'sim.js_outlet', type: 'outlet', adapter: 'sim' });
    assert.strictEqual(cardHasToggle(c, p), true);
});

test('a sensor type without a power row offers no toggle', () => {
    const c = bootConsole(CATALOG);
    const p = point({
        point_id: 'ble_mesh.bm_0002', type: 'sensor_temp_humidity', value: '23.5',
        props: { params: { temperature: { value: 23.5, access: 'R' }, humidity: { value: 50, access: 'R' } } },
    });
    assert.strictEqual(cardHasToggle(c, p), false);
});

test('a hidden power param offers no toggle', () => {
    const c = bootConsole(CATALOG);
    const p = point({ props: { params: { power: { value: 'on', visible: false } } } });
    assert.strictEqual(cardHasToggle(c, p), false);
});

test('a read-only power param offers no toggle', () => {
    const c = bootConsole(CATALOG);
    const p = point({ props: { params: { power: { value: 'on', access: 'R' } } } });
    assert.strictEqual(cardHasToggle(c, p), false);
});

test('a camera card keeps its toggle from the boolean summary', () => {
    const c = bootConsole(CATALOG);
    /* rtsp_record.c registers camera/cam0 with a flat on/off value and no params. */
    const p = point({ point_id: 'camera/cam0', type: 'camera', adapter: 'rtsp_cam', value: 'off', props: { recording: false } });
    assert.strictEqual(cardHasToggle(c, p), true);
});

test('a cover card offers no power toggle', () => {
    const c = bootConsole(CATALOG);
    const p = point({ point_id: 'matter.cover_1', type: 'cover', adapter: 'matter', value: '50', props: {} });
    assert.strictEqual(cardHasToggle(c, p), false);
});

test('an unknown type reporting a state, not a power summary, offers no toggle', () => {
    const c = bootConsole(CATALOG);
    const p = point({ point_id: 'matter.door_1', type: 'binary_sensor', adapter: 'matter', value: 'open', props: {} });
    assert.strictEqual(cardHasToggle(c, p), false);
});

function bootMap(points) {
    const c = bootConsole(CATALOG);
    c.ctx.L = {
        circleMarker(latlng, opts) {
            const m = {
                latlng, opts, handlers: {},
                bindPopup() { return m; },
                on(ev, fn) { m.handlers[ev] = fn; return m; },
            };
            c.ctx.__markers.push(m);
            return m;
        },
    };
    c.ctx.__markers = [];
    c.run(`enabledAdapters = { ble_mesh: true, sim: true, rtsp_cam: true, matter: true };
           pts = ${JSON.stringify(points)};
           deviceLayer = { clearLayers: function () {}, addLayer: function () {} };
           paintMapDevices([22.5, 113.9]);`);
    return c;
}

test('clicking a speaker marker sends the power command', async () => {
    const c = bootMap([point({ point_id: 'sim.js_speaker', type: 'speaker', adapter: 'sim' })]);
    const marker = c.ctx.__markers[0];
    assert.ok(marker.handlers.click, 'speaker marker has no click handler');
    await marker.handlers.click();
    assert.deepStrictEqual(c.posts.map((p) => [p.url, p.body.cmd]), [['/api/command', 'turn_off']]);
});

test('clicking a camera marker sends the power command', async () => {
    const c = bootMap([point({ point_id: 'camera/cam0', type: 'camera', adapter: 'rtsp_cam', value: 'off', props: { recording: false } })]);
    const marker = c.ctx.__markers[0];
    assert.ok(marker.handlers.click, 'camera marker has no click handler');
    await marker.handlers.click();
    assert.deepStrictEqual(c.posts.map((p) => [p.url, p.body.cmd]), [['/api/command', 'turn_on']]);
});

test('clicking a sensor marker sends nothing', async () => {
    const c = bootMap([point({
        point_id: 'ble_mesh.bm_0002', type: 'sensor_temp_humidity', value: '23.5',
        props: { params: { temperature: { value: 23.5, access: 'R' } } },
    })]);
    assert.strictEqual(c.ctx.__markers[0].handlers.click, undefined);
    assert.deepStrictEqual(c.posts, []);
});

test('the console asks the catalog instead of listing controllable types', () => {
    const overview = fs.readFileSync(path.join(WEB_JS, 'overview.js'), 'utf8');
    const map = fs.readFileSync(path.join(WEB_JS, 'map.js'), 'utf8');
    assert.strictEqual(overview.includes('ctrlTypes'), false,
        'overview.js still gates the toggle on a hardcoded type list');
    assert.strictEqual(/p\.type === 'switch' \|\| p\.type === 'light'/.test(map), false,
        'map.js still gates the click on a hardcoded type list');
});
