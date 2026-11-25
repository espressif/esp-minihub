let scenesCfg = {
    scenes: []
};
let scenesSel = null;
let scenesEditMode = null;     /* null | 'new' | <scene id> */
let _scEdit = null;            /* editor model: { id, name, steps:[{actions,loop,delay_after_ms}], max_duration_ms } */

const SC_MAX_STEPS = 5;
const SC_MAX_ACTIONS = 8;
const SC_MAX_DELAY_SEC = 7200;

function scenesGenId(name) {
    const slug = (name || '').trim().toLowerCase().replace(/[^a-z0-9]+/g, '_').replace(/^_+|_+$/g, '');
    return (slug ? slug + '_' : 'scene_') + Date.now().toString(36);
}

/* Migrate old set_color params.color to params.hue (clamp 0-360). */
function scenesMigrateActionParams(cmd, params) {
    if (!params || cmd !== 'set_color' || params.color == null || params.hue != null) return params;
    const out = {};
    for (const k in params) {
        if (Object.prototype.hasOwnProperty.call(params, k) && k !== 'color') out[k] = params[k];
    }
    let n = parseInt(params.color, 10);
    if (isNaN(n)) n = 0;
    out.hue = Math.max(0, Math.min(360, n));
    return out;
}

/* Normalize any stored scene (new `steps` or legacy `commands`) into the editor
 * model so the editor always works on the same shape. */
function scenesNormalize(sc) {
    if (!sc) {
        return { id: '', name: '', steps: [scenesBlankStep()], max_duration_ms: 0 };
    }
    let steps = [];
    if (Array.isArray(sc.steps)) {
        steps = sc.steps.map(function (s) {
            return {
                actions: (Array.isArray(s.actions) ? s.actions : []).map(function (a) {
                    return { point_id: a.point_id || '', cmd: a.cmd || '', params: scenesMigrateActionParams(a.cmd, a.params) };
                }),
                loop: (s.loop > 0 ? s.loop : 1),
                delay_after_ms: (s.delay_after_ms != null ? s.delay_after_ms : 0)
            };
        });
    } else if (Array.isArray(sc.commands)) {
        /* Legacy flat command list: one step per command, its delay becomes the
         * trailing delay of that step (preserves serial semantics). */
        steps = sc.commands.map(function (c) {
            return {
                actions: [{ point_id: c.point_id || '', cmd: c.cmd || '', params: scenesMigrateActionParams(c.cmd, c.params) }],
                loop: 1,
                delay_after_ms: (c.delay_ms != null ? c.delay_ms : 0)
            };
        });
    }
    if (!steps.length) steps = [scenesBlankStep()];
    return {
        id: sc.id || '',
        name: sc.name || '',
        steps: steps,
        max_duration_ms: (sc.max_duration_ms > 0 ? sc.max_duration_ms : 0)
    };
}

function scenesBlankStep() {
    return { actions: [{ point_id: '', cmd: '', params: null }], loop: 1, delay_after_ms: 0 };
}

async function loadScenes() {
    try {
        const r = await fetch('/api/scenes');
        if (!r.ok) throw new Error('HTTP ' + r.status);
        scenesCfg = await r.json();
        if (!scenesCfg || !Array.isArray(scenesCfg.scenes)) scenesCfg = { scenes: [] };
        if (view === 'scenes') renderScenes();
    } catch (e) {
        toast(t('scenes.load_failed'));
        const el = document.getElementById('scenes-list');
        if (el) el.innerHTML = '<div class="empty">' + t('scenes.load_error') + '</div>';
    }
}

function sceneCmdSummary(cmds) {
    const arr = Array.isArray(cmds) ? cmds : [];
    if (!arr.length) return t('scenes.no_cmds');
    return arr.slice(0, 3).map(function (c) {
        return (c.cmd || '?') + ' → ' + (c.point_id || '?');
    }).join(' · ') + (arr.length > 3 ? ' …' : '');
}

/* Estimated run time (ms) = Σ step(loop × delay_after_ms). Action dispatch is
 * fire-and-forget so it contributes ~0. */
function scenesEstimatedMs(model) {
    let tot = 0;
    (model.steps || []).forEach(function (s) {
        const loop = s.loop > 0 ? s.loop : 1;
        tot += loop * (s.delay_after_ms || 0);
    });
    return tot;
}

function sceneStepSummary(step, si) {
    const acts = (step.actions || []).map(function (a) {
        return (a.cmd || '?') + ' → ' + (a.point_id || '?');
    }).join(' · ');
    const loop = step.loop > 0 ? step.loop : 1;
    const delayS = (step.delay_after_ms || 0) / 1000;
    let tail = '';
    if (loop > 1) tail += ' · ' + t('scenes.loop') + ' ×' + loop;
    if (delayS > 0) tail += ' · ' + t('scenes.delay_after') + ' ' + delayS + 's';
    return '<div class="rule-step"><div class="rule-step-label">' + t('scenes.step', { n: si + 1 }) +
        '</div><div class="rule-step-title">' + esc(acts || t('scenes.no_cmds')) +
        '</div><div class="rule-step-body">' + esc(tail) + '</div></div>';
}

function renderScenes() {
    const list = document.getElementById('scenes-list');
    const detail = document.getElementById('scenes-detail');
    const edit = document.getElementById('scenes-edit');
    if (!list || !detail) return;
    if (edit) {
        if (scenesEditMode) {
            list.hidden = true;
            detail.hidden = true;
            edit.hidden = false;
            return;
        }
        edit.hidden = true;
        edit.innerHTML = '';
    }
    if (scenesSel) {
        const sc = (scenesCfg.scenes || []).find(function (x) {
            return x.id === scenesSel;
        });
        list.hidden = true;
        detail.hidden = false;
        if (!sc) {
            scenesSel = null;
            renderScenes();
            return;
        }
        const norm = scenesNormalize(sc);
        const steps = norm.steps.map(function (s, i) {
            return sceneStepSummary(s, i);
        }).join('<div class="rule-arrow">↓</div>');
        const estSec = Math.round(scenesEstimatedMs(norm) / 1000);
        detail.innerHTML = '<button class="btn-sm" type="button" onclick="scenesBack()">' + t('scenes.back') + '</button>' +
            '<button class="btn-sm on" type="button" style="margin-left:8px" onclick="scenesEditOpen(\'' + escAttr(sc.id) + '\')">' + t('scenes.edit') + '</button>' +
            '<button class="btn-sm on" type="button" style="margin-left:8px" onclick="scenesInvoke(\'' + escAttr(sc.id) + '\')">' + t('scenes.run') + '</button>' +
            '<div class="dcard" style="margin-top:12px"><div class="card-top"><div class="card-name">' + esc(sc.name || sc.id || t('scenes.scene')) + '</div><span class="badge sensor">' + t('scenes.step_count', { n: (sc.steps || []).length }) + '</span></div>' +
            '<div class="card-meta"><span>' + esc(t('scenes.est_total', { n: estSec + 's' })) + '</span></div>' +
            '<div class="rule-flow">' + (steps || '<div class="empty">' + t('scenes.no_cmds') + '</div>') + '</div></div>';
        return;
    }
    detail.hidden = true;
    list.hidden = false;
    const arr = scenesCfg.scenes || [];
    if (!arr.length) {
        list.className = '';
        list.innerHTML = '<div class="empty">' + t('scenes.empty') + '</div>';
        return;
    }
    list.className = 'grid';
    list.innerHTML = arr.map(function (s) {
        const norm = scenesNormalize(s);
        const n = norm.steps.length;
        const estSec = Math.round(scenesEstimatedMs(norm) / 1000);
        return '<div class="dcard" style="cursor:pointer" data-scene-id="' + escAttr(s.id || '') + '">' +
            '<div class="card-top"><div class="card-name">' + esc(s.name || s.id || '(unnamed)') + '</div><span class="badge sensor">' + t('scenes.step_count', { n: n }) + '</span></div>' +
            '<div class="card-meta"><span>' + esc(t('scenes.est_total', { n: estSec + 's' })) + '</span></div>' +
            '<div class="card-actions" style="margin-top:10px"><button class="btn-sm on" type="button" data-invoke="' + escAttr(s.id || '') + '">' + t('scenes.run') + '</button></div></div>';
    }).join('');
    list.onclick = function (ev) {
        const inv = ev.target.closest('[data-invoke]');
        if (inv) {
            ev.stopPropagation();
            scenesInvoke(inv.getAttribute('data-invoke'));
            return;
        }
        const card = ev.target.closest('[data-scene-id]');
        if (!card) return;
        scenesSel = card.getAttribute('data-scene-id');
        renderScenes();
    };
}

function scenesBack() {
    scenesSel = null;
    scenesEditMode = null;
    renderScenes();
}

async function scenesInvoke(id) {
    if (!id) return;
    toast(t('scenes.running'));
    try {
        const r = await fetch('/api/scenes/invoke', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ scene_id: id })
        });
        if (r.ok) toast(t('scenes.invoked'));
        else {
            let m = t('scenes.failed');
            try {
                const j = await r.json();
                m = j.error || j.message || m;
            } catch (e) {}
            toast(String(m).slice(0, 80));
        }
    } catch (e) {
        toast(t('scenes.net_error'));
    }
}

async function scenesEditNew() {
    if (!pts || !pts.length) {
        try { await loadPoints(); } catch (e) {}
    }
    scenesSel = null;
    scenesEditMode = 'new';
    _scEdit = scenesNormalize(null);
    scenesEditRender();
}

async function scenesEditOpen(id) {
    if (!pts || !pts.length) {
        try { await loadPoints(); } catch (e) {}
    }
    const sc = (scenesCfg.scenes || []).find(function (x) {
        return x.id === id;
    });
    if (!sc) {
        toast(t('scenes.not_found'));
        return;
    }
    scenesEditMode = id;
    _scEdit = scenesNormalize(sc);
    scenesEditRender();
}

function scenesEditCancel() {
    scenesEditMode = null;
    _scEdit = null;
    renderScenes();
}

/* ---- editor: action row (device + command + optional value, like rules) ---- */
function scenesActRow(act, si, aj) {
    const pid = act.point_id || '';
    const type = (rulesFindPoint(pid) || {}).type || '';
    let cmd = act.cmd || '';
    if (!pid) {
        cmd = '';
    } else if (cmd && rulesCmdDefFor(cmd, type) == null) {
        /* Drop stale defaults (e.g. turn_on on a speaker) instead of painting
         * an orphan option. */
        cmd = '';
    }
    let h = '<div class="rule-then-row" data-step="' + si + '" data-act="' + aj + '"><div class="rule-then-fields">';
    h += '<div><label>' + t('scenes.field_point') + '</label>' +
        rulesSelectHtml('sc-step-' + si + '-act-' + aj + '-pid', rulesDeviceOptionsHtml(pid), 'scenesEditOnDeviceChange(' + si + ',' + aj + ')') + '</div>';
    h += '<div><label>' + t('scenes.field_cmd') + '</label>' +
        '<select id="sc-step-' + si + '-act-' + aj + '-cmd" onchange="scenesEditOnCmdChange(' + si + ',' + aj + ')">' +
        rulesCmdOptionsHtml(pid, type, cmd) + '</select></div>';
    const def = cmd ? rulesCmdDefFor(cmd, type) : null;
    if (def && def.value) {
        const curVal = (def.param && act.params && act.params[def.param] != null) ? act.params[def.param] : '';
        const rng = rulesActionValueRange(pid, def.param, def);
        const unit = rng.unit ? ' (' + rng.unit + ')' : '';
        const min = (rng.min != null) ? ' min="' + rng.min + '"' : '';
        const max = (rng.max != null) ? ' max="' + rng.max + '"' : '';
        h += '<div><label>' + t('scenes.field_value') + esc(unit) + '</label>' +
            '<div class="rule-ed-val"><input type="number" id="sc-step-' + si + '-act-' + aj + '-value"' + min + max +
            ' value="' + escAttr(String(curVal)) + '" placeholder="' + escAttr(def.param || '') +
            '" oninput="rulesClampInput(this,\'max\');scenesEditOnValueInput(' + si + ',' + aj + ')" onchange="rulesClampInput(this);scenesEditOnValueInput(' + si + ',' + aj + ')" />' +
            '<span class="rule-ed-unit">' + esc(rng.unit || '') + '</span></div></div>';
    }
    h += '</div><div style="text-align:right;margin-top:2px"><button class="btn-sm bad scenes-rm" type="button" onclick="scenesEditRmAction(' + si + ',' + aj + ')" title="' + t('scenes.delete') + '">×</button></div>';
    h += '</div>';
    return h;
}

function scenesEditPaintSteps() {
    const box = document.getElementById('sc-steps');
    if (!box) return;
    const m = _scEdit;
    let h = '';
    m.steps.forEach(function (step, si) {
        h += '<div class="rule-step-card" data-step="' + si + '">';
        h += '<div class="rule-step-head"><span class="rule-step-num">' + (si + 1) + '</span>';
        if (m.steps.length > 1) {
            h += '<button class="btn-sm bad" type="button" onclick="scenesEditRmStep(' + si + ')">' + t('scenes.delete') + '</button>';
        }
        h += '</div>';
        if (step.actions.length === 0) {
            h += '<div class="rule-step-empty">' + t('scenes.step_no_action') + '</div>';
        }
        step.actions.forEach(function (act, aj) {
            if (aj > 0) h += '<div class="rule-then-sep"></div>';
            h += scenesActRow(act, si, aj);
        });
        h += '<div class="rule-ed-actions" style="margin-top:8px"><button class="btn-sm" type="button" onclick="scenesEditAddAction(' + si + ')">' + t('scenes.add_action') + '</button></div>';
        h += '<div class="row2 scenes-step-params">';
        h += '<div><label>' + t('scenes.loop') + '</label><input type="number" min="1" id="sc-step-' + si + '-loop" value="' + escAttr(String(step.loop || 1)) + '" oninput="scenesEditOnParamInput(' + si + ')" /></div>';
        h += '<div><label>' + t('scenes.delay_after') + '</label><input type="number" min="0" max="' + SC_MAX_DELAY_SEC + '" step="0.1" id="sc-step-' + si + '-delay" value="' + escAttr(String((step.delay_after_ms || 0) / 1000)) + '" oninput="scenesEditOnParamInput(' + si + ')" /></div>';
        h += '</div></div>';
    });
    if (m.steps.length < SC_MAX_STEPS) {
        h += '<button class="btn-sm" type="button" style="margin-top:10px" onclick="scenesEditAddStep()">' + t('scenes.add_step') + '</button>';
    }
    box.innerHTML = h;
}

function scenesEditRender() {
    const edit = document.getElementById('scenes-edit');
    const list = document.getElementById('scenes-list');
    const detail = document.getElementById('scenes-detail');
    if (!edit) return;
    if (list) list.hidden = true;
    if (detail) detail.hidden = true;
    edit.hidden = false;

    const isNew = scenesEditMode === 'new';
    const estSec = Math.round(scenesEstimatedMs(_scEdit) / 1000);

    edit.innerHTML =
        '<div class="rule-ed">' +
        '<div class="rule-ed-actions" style="margin-top:0;margin-bottom:12px"><button class="btn-sm" type="button" onclick="scenesEditCancel()">' + t('scenes.cancel_back') + '</button></div>' +
        '<h3>' + (isNew ? t('scenes.new') : t('scenes.edit_title')) + '</h3>' +
        '<div class="scenes-name-row">' +
        '<label>' + t('scenes.field_name') + '</label>' +
        '<input id="sc-name" value="' + escAttr(_scEdit.name || '') + '" placeholder="' + t('scenes.name_ph') + '" />' +
        '</div>' +
        '<div class="rule-ed-sec">' +
        '<h4>' + t('scenes.steps_title') + '</h4>' +
        '<div id="sc-steps"></div>' +
        '</div>' +
        '<div class="rule-ed-sec">' +
        '<h4>' + t('scenes.duration_title') + '</h4>' +
        '<div class="scenes-dur-row">' + t('scenes.est_total', { n: '<span id="sc-est-total">' + estSec + '</span>' }) + '</div>' +
        '<div class="row2"><div><label>' + t('scenes.max_duration') + '</label>' +
        '<input type="number" min="0" step="1" id="sc-max-dur" placeholder="' + estSec + '" value="' + (Math.round((_scEdit.max_duration_ms || 0) / 1000) || '') + '" oninput="scenesEditUpdateTotal()" /></div></div>' +
        '<div class="rule-ed-hint" id="sc-max-err" hidden>' + t('scenes.max_duration_err') + '</div>' +
        '</div>' +
        '<div class="rule-ed-actions">' +
        '<button class="btn-sm on" type="button" onclick="scenesEditSave()">' + t('scenes.save') + '</button>' +
        (isNew ? '' : '<button class="btn-sm bad" type="button" onclick="scenesEditDelete()">' + t('scenes.delete') + '</button>') +
        '</div>' +
        '</div>';
    scenesEditPaintSteps();
    scenesEditUpdateTotal();
}

/* ---- editor interactions ---- */
function scenesEditOnDeviceChange(si, aj) {
    const el = document.getElementById('sc-step-' + si + '-act-' + aj + '-pid');
    if (el) {
        const a = _scEdit.steps[si].actions[aj];
        a.point_id = el.value;
        /* Clear command so the next paint shows only this product's real cmds. */
        a.cmd = '';
        a.params = null;
    }
    scenesEditPaintSteps();
}
function scenesEditOnCmdChange(si, aj) {
    const el = document.getElementById('sc-step-' + si + '-act-' + aj + '-cmd');
    if (el) {
        const a = _scEdit.steps[si].actions[aj];
        a.cmd = el.value;
        a.params = null;
    }
    scenesEditPaintSteps();
}
function scenesEditOnValueInput(si, aj) {
    const el = document.getElementById('sc-step-' + si + '-act-' + aj + '-value');
    if (!el) return;
    const a = _scEdit.steps[si].actions[aj];
    const pid = a.point_id;
    const type = (rulesFindPoint(pid) || {}).type || '';
    const def = rulesCmdDefFor(a.cmd, type);
    if (!def) return;
    const rng = rulesActionValueRange(pid, def.param, def);
    const v = rulesClampValue(rng, el.value);
    if (v === '' || v == null) {
        if (a.params) delete a.params[def.param];
    } else {
        const val = (def.min != null || rng.min != null) ? Number(v) : v;
        a.params = a.params || {};
        a.params[def.param] = val;
    }
}
function scenesEditOnParamInput(si) {
    const loopEl = document.getElementById('sc-step-' + si + '-loop');
    const delayEl = document.getElementById('sc-step-' + si + '-delay');
    const s = _scEdit.steps[si];
    if (loopEl) { let l = parseInt(loopEl.value, 10); if (isNaN(l) || l < 1) l = 1; s.loop = l; }
    if (delayEl) { let d = parseFloat(delayEl.value); if (isNaN(d) || d < 0) d = 0; if (d > SC_MAX_DELAY_SEC) d = SC_MAX_DELAY_SEC; if (delayEl.value !== '' && Number(delayEl.value) !== d) delayEl.value = String(d); s.delay_after_ms = Math.round(d * 1000); }
    scenesEditUpdateTotal();
}
function scenesEditAddAction(si) {
    if (_scEdit.steps[si].actions.length >= SC_MAX_ACTIONS) {
        toast(t('scenes.err_action_max', { n: SC_MAX_ACTIONS }));
        return;
    }
    _scEdit.steps[si].actions.push({ point_id: '', cmd: '', params: null });
    scenesEditPaintSteps();
}
function scenesEditRmAction(si, aj) {
    _scEdit.steps[si].actions.splice(aj, 1);
    scenesEditPaintSteps();
}
function scenesEditAddStep() {
    if (_scEdit.steps.length >= SC_MAX_STEPS) {
        toast(t('scenes.err_step_max', { n: SC_MAX_STEPS }));
        return;
    }
    _scEdit.steps.push(scenesBlankStep());
    scenesEditPaintSteps();
    scenesEditUpdateTotal();
}
function scenesEditRmStep(si) {
    _scEdit.steps.splice(si, 1);
    if (!_scEdit.steps.length) _scEdit.steps.push(scenesBlankStep());
    scenesEditPaintSteps();
    scenesEditUpdateTotal();
}
function scenesEditUpdateTotal() {
    const estSec = Math.round(scenesEstimatedMs(_scEdit) / 1000);
    const estEl = document.getElementById('sc-est-total');
    if (estEl) estEl.textContent = estSec;
    const maxEl = document.getElementById('sc-max-dur');
    const errEl = document.getElementById('sc-max-err');
    let maxSec = maxEl ? parseInt(maxEl.value, 10) : 0;
    if (isNaN(maxSec)) maxSec = 0;
    if (maxSec > 0 && maxSec > estSec) {
        if (errEl) errEl.hidden = false;
        _scEdit._maxErr = true;
    } else {
        if (errEl) errEl.hidden = true;
        _scEdit._maxErr = false;
    }
}

function scenesEditSyncModel() {
    if (!_scEdit) return;
    const nameEl = document.getElementById('sc-name');
    if (nameEl) _scEdit.name = nameEl.value.trim();
    _scEdit.steps.forEach(function (s, si) {
        const loopEl = document.getElementById('sc-step-' + si + '-loop');
        const delayEl = document.getElementById('sc-step-' + si + '-delay');
        if (loopEl) { let l = parseInt(loopEl.value, 10); if (isNaN(l) || l < 1) l = 1; s.loop = l; }
        if (delayEl) { let d = parseFloat(delayEl.value); if (isNaN(d) || d < 0) d = 0; if (d > SC_MAX_DELAY_SEC) d = SC_MAX_DELAY_SEC; s.delay_after_ms = Math.round(d * 1000); }
        s.actions.forEach(function (a, aj) {
            const pidEl = document.getElementById('sc-step-' + si + '-act-' + aj + '-pid');
            const cmdEl = document.getElementById('sc-step-' + si + '-act-' + aj + '-cmd');
            const valEl = document.getElementById('sc-step-' + si + '-act-' + aj + '-value');
            if (pidEl) a.point_id = pidEl.value;
            if (cmdEl) a.cmd = cmdEl.value;
            if (valEl) {
                const type = (rulesFindPoint(a.point_id) || {}).type || '';
                const def = rulesCmdDefFor(a.cmd, type);
                if (def && def.param) {
                    const rng = rulesActionValueRange(a.point_id, def.param, def);
                    const v = rulesClampValue(rng, valEl.value);
                    if (v === '' || v == null) {
                        if (a.params) delete a.params[def.param];
                    } else {
                        const val = (def.min != null || rng.min != null) ? Number(v) : v;
                        a.params = a.params || {};
                        a.params[def.param] = val;
                    }
                }
            }
        });
    });
    scenesEditUpdateTotal();
}

async function scenesEditSave() {
    scenesEditSyncModel();
    const name = _scEdit.name;
    if (!name) { toast(t('scenes.err_name')); return; }
    if (!_scEdit.steps.length) { toast(t('scenes.err_no_steps')); return; }
    for (let i = 0; i < _scEdit.steps.length; i++) {
        const s = _scEdit.steps[i];
        if (!s.actions.length) { toast(t('scenes.err_step_no_action', { n: i + 1 })); return; }
        for (let j = 0; j < s.actions.length; j++) {
            const a = s.actions[j];
            if (!a.point_id || !a.cmd) { toast(t('scenes.err_action_fields')); return; }
        }
    }
    if (_scEdit._maxErr) { toast(t('scenes.err_max_duration')); return; }

    const id = (scenesEditMode === 'new') ? scenesGenId(name) : (_scEdit.id || scenesGenId(name));
    const maxEl = document.getElementById('sc-max-dur');
    let maxSec = maxEl ? parseInt(maxEl.value, 10) : 0;
    if (isNaN(maxSec) || maxSec < 0) maxSec = 0;

    const steps = _scEdit.steps.map(function (s) {
        return {
            actions: s.actions
                .filter(function (a) { return a.point_id && a.cmd; })
                .map(function (a) {
                    const o = { point_id: a.point_id, cmd: a.cmd };
                    if (a.params && Object.keys(a.params).length) o.params = a.params;
                    return o;
                }),
            loop: (s.loop > 0 ? s.loop : 1),
            delay_after_ms: (s.delay_after_ms || 0)
        };
    });
    const item = { id: id, name: name, steps: steps };
    if (maxSec > 0) item.max_duration_ms = maxSec * 1000;

    const next = (scenesCfg.scenes || []).slice();
    const ix = next.findIndex(function (x) { return x.id === id; });
    if (ix < 0) next.push(item);
    else next[ix] = item;

    try {
        const r = await fetch('/api/scenes/import', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ scenes: next })
        });
        if (!r.ok) { toast(t('scenes.save_failed')); return; }
        scenesEditMode = null;
        scenesSel = id;
        _scEdit = null;
        await loadScenes();
        toast(t('scenes.saved'));
    } catch (e) {
        toast(t('scenes.net_error'));
    }
}

async function scenesEditDelete() {
    const id = _scEdit.id;
    if (!id || !confirm(t('scenes.confirm_delete', { id: id }))) return;
    const next = (scenesCfg.scenes || []).filter(function (x) { return x.id !== id; });
    try {
        const r = await fetch('/api/scenes/import', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ scenes: next })
        });
        if (!r.ok) { toast(t('scenes.delete_failed')); return; }
        scenesEditMode = null;
        scenesSel = null;
        _scEdit = null;
        await loadScenes();
        toast(t('scenes.deleted'));
    } catch (e) {
        toast(t('scenes.net_error'));
    }
}
