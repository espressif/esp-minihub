let rulesCfg = {
    rules: []
};
let rulesSel = null;
let rulesEditMode = null;

const RULES_AI_LS = 'minihub_rules_ai_v1';
let rulesAiDraft = null;

function rulesAiLoadCfg() {
    try {
        const raw = localStorage.getItem(RULES_AI_LS);
        if (!raw) return;
        const j = JSON.parse(raw);
        const b = document.getElementById('rules-ai-base');
        const m = document.getElementById('rules-ai-model');
        const k = document.getElementById('rules-ai-key');
        if (b && j.base) b.value = j.base;
        if (m && j.model) m.value = j.model;
        if (k && j.key) k.value = j.key;
    } catch (e) {}
}

function rulesAiSaveCfg() {
    const b = (document.getElementById('rules-ai-base').value || '').trim();
    const m = (document.getElementById('rules-ai-model').value || '').trim();
    const k = (document.getElementById('rules-ai-key').value || '').trim();
    try {
        localStorage.setItem(RULES_AI_LS, JSON.stringify({
            base: b,
            model: m,
            key: k
        }));
    } catch (e) {}
}

function rulesAiToggleCfg() {
    const el = document.getElementById('rules-ai-cfg');
    if (!el) return;
    el.hidden = !el.hidden;
    if (!el.hidden) rulesAiLoadCfg();
}

function rulesAiSystemPrompt(pointIds) {
    return 'You generate MiniHub rules.json. Output ONLY valid JSON object with key "rules" (array). No markdown.\n' +
        'Schema per rule: id(string), name(string), enabled(bool), when, if(array), then.\n' +
        'when point: {"type":"point","point_id":"<device point>","parameter":"<param>","op":"eq|lt|le|gt|ge","value":"..."}\n' +
        '  Legacy also ok: {"type":"point","point_id":"...","equals":"..."}.\n' +
        'when time: {"type":"time","at":"HH:MM"}.\n' +
        'if item: same shape as when point (point_id + parameter + op + value). Empty if ok.\n' +
        'then: one action {"type":"command","point_id":"...","cmd":"..."} or an ordered array of them ' +
        '(e.g. [{"type":"delay","seconds":5},{"type":"command","point_id":"...","cmd":"turn_on"}]; max 4).\n' +
        'Prefer these device point_id values when relevant: ' + JSON.stringify(pointIds.slice(0, 80)) + '\n' +
        'Common cmds: turn_on, turn_off, toggle. Chinese user input is ok.';
}

function rulesAiValidate(file) {
    if (!file || typeof file !== 'object' || !Array.isArray(file.rules) || !file.rules.length) return {
        ok: false,
        error: t('rules.err_missing_rules_array')
    };
    for (let i = 0; i < file.rules.length; i++) {
        const r = file.rules[i];
        if (!r || typeof r !== 'object') return {
            ok: false,
            error: t('rules.err_rule_invalid', { n: i + 1 })
        };
        if (!r.id || !r.name || !r.when || !r.then) return {
            ok: false,
            error: t('rules.err_rule_missing_fields', { n: i + 1 })
        };
        const w = r.when;
        if (w.type === 'point') {
            const hasVal = (w.value != null && w.value !== '') || (w.equals != null && w.equals !== '');
            if (!w.point_id || !hasVal) return {
                ok: false,
                error: t('rules.err_rule_when_incomplete', { n: i + 1 })
            };
        } else if (w.type === 'time') {
            if (!/^\d{2}:\d{2}$/.test(w.at || '')) return {
                ok: false,
                error: t('rules.err_rule_when_time', { n: i + 1 })
            };
        } else return {
            ok: false,
            error: t('rules.err_rule_when_type', { n: i + 1 })
        };
        const ths = rulesThenList(r.then);
        for (let k = 0; k < ths.length; k++) {
            const th = ths[k];
            if (th.type === 'command') {
                if (!th.point_id || !th.cmd) return {
                    ok: false,
                    error: t('rules.err_rule_then_incomplete', { n: i + 1 })
                };
            } else if (th.type === 'delay') {
                if (!(th.seconds > 0)) return {
                    ok: false,
                    error: t('rules.err_rule_then_delay', { n: i + 1 })
                };
            } else return {
                ok: false,
                error: t('rules.err_rule_then_type', { n: i + 1 })
            };
        }
        if (r.if != null && !Array.isArray(r.if)) return {
            ok: false,
            error: t('rules.err_rule_if_array', { n: i + 1 })
        };
    }
    return {
        ok: true
    };
}

function rulesAiExtractJson(text) {
    if (!text) return null;
    let s = String(text).trim();
    const fence = s.match(/```(?:json)?\s*([\s\S]*?)```/i);
    if (fence) s = fence[1].trim();
    const a = s.indexOf('{');
    const b = s.lastIndexOf('}');
    if (a >= 0 && b > a) s = s.slice(a, b + 1);
    try {
        return JSON.parse(s);
    } catch (e) {
        return null;
    }
}

function rulesAiRenderDraft(file) {
    const body = document.getElementById('rules-ai-draft-body');
    const box = document.getElementById('rules-ai-draft');
    if (!body || !box) return;
    const rules = file.rules || [];
    let html = '';
    rules.forEach(function(rule, idx) {
        const iff = Array.isArray(rule.if) ? rule.if : [];
        const w = whenPlain(rule.when);
        const i = ifPlain(iff);
        const th = thenPlain(rule.then);
        html += '<div class="dcard" style="margin-bottom:10px">' +
            '<div class="card-top"><div class="card-name">' + esc(rule.name || rule.id || t('rules.draft', { n: idx + 1 })) + '</div>' +
            (rule.enabled === false ? '<span class="badge">' + t('rules.disabled') + '</span>' : '<span class="badge sensor">' + t('rules.enabled') + '</span>') + '</div>' +
            '<div class="card-meta"><span>' + esc(rule.id || '') + '</span></div>' +
            '<div class="rule-flow">' + ruleStepHtml(t('rules.trigger_when'), w) + '<div class="rule-arrow">↓</div>' + ruleStepHtml(t('rules.condition_if'), i) + '<div class="rule-arrow">↓</div>' + ruleStepHtml(t('rules.action_then'), th) + '</div></div>';
    });
    html += '<details class="rule-tech"><summary>' + t('rules.tech_details') + '</summary><pre class="rule-pre">' + esc(JSON.stringify(file, null, 2)) + '</pre></details>';
    body.innerHTML = html;
    box.hidden = false;
}

function rulesAiCancel() {
    rulesAiDraft = null;
    const box = document.getElementById('rules-ai-draft');
    if (box) box.hidden = true;
    const body = document.getElementById('rules-ai-draft-body');
    if (body) body.innerHTML = '';
}

function rulesAiNextIds(n) {
    const used = {};
    (rulesCfg.rules || []).forEach(function(r) {
        if (r && r.id) used[r.id] = 1;
    });
    let i = 1;
    const out = [];
    while (out.length < n) {
        const id = 'rule_' + String(i).padStart(3, '0');
        if (!used[id]) {
            out.push(id);
            used[id] = 1;
        }
        i++;
    }
    return out;
}
async function rulesAiGenerate() {
    const prompt = (document.getElementById('rules-ai-prompt').value || '').trim();
    if (!prompt) {
        toast(t('rules.ai_need_prompt'));
        return;
    }
    rulesAiLoadCfg();
    const base = ((document.getElementById('rules-ai-base').value || '').trim() || 'https://api.openai.com/v1').replace(/\/$/, '');
    const model = (document.getElementById('rules-ai-model').value || '').trim() || 'gpt-4o-mini';
    const key = (document.getElementById('rules-ai-key').value || '').trim();
    if (!key) {
        toast(t('rules.ai_need_key'));
        const cfg = document.getElementById('rules-ai-cfg');
        if (cfg) cfg.hidden = false;
        return;
    }
    rulesAiSaveCfg();
    if (!pts || !pts.length) {
        try {
            await loadPoints();
        } catch (e) {}
    }
    const pointIds = (pts || []).map(function(p) {
        return p.point_id;
    }).filter(Boolean);
    const btn = document.getElementById('rules-ai-gen');
    if (btn) btn.disabled = true;
    toast(t('rules.ai_generating'));
    try {
        const r = await fetch('/api/rules/ai/chat', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify({
                base_url: base,
                model: model,
                api_key: key,
                messages: [{
                        role: 'system',
                        content: rulesAiSystemPrompt(pointIds)
                    },
                    {
                        role: 'user',
                        content: prompt
                    }
                ]
            })
        });
        const j = await r.json().catch(function() {
            return {};
        });
        if (!r.ok) {
            const msg = (j.error && j.error.message) || j.message || (j.upstream_body ? String(j.upstream_body).slice(0, 120) : '') || ('HTTP ' + r.status);
            toast(t('rules.ai_cloud_failed', { msg: msg }));
            return;
        }
        const content = (j.choices && j.choices[0] && j.choices[0].message && j.choices[0].message.content) || '';
        const parsed = rulesAiExtractJson(content);
        if (!parsed) {
            toast(t('rules.ai_bad_json'));
            return;
        }
        const v = rulesAiValidate(parsed);
        if (!v.ok) {
            toast(t('rules.ai_validation_failed', { err: v.error }));
            return;
        }
        parsed.rules.forEach(function(rule) {
            if (typeof rule.enabled !== 'boolean') rule.enabled = true;
            if (!Array.isArray(rule.if)) rule.if = [];
        });
        rulesAiDraft = parsed;
        rulesAiRenderDraft(parsed);
        toast(t('rules.ai_generated', { n: parsed.rules.length }));
    } catch (e) {
        toast(t('rules.ai_gen_failed'));
    } finally {
        if (btn) btn.disabled = false;
    }
}
async function rulesAiApply() {
    if (!rulesAiDraft || !Array.isArray(rulesAiDraft.rules) || !rulesAiDraft.rules.length) {
        toast(t('rules.ai_no_draft'));
        return;
    }
    const replace = !!(document.getElementById('rules-ai-replace') && document.getElementById('rules-ai-replace').checked);
    let nextRules;
    if (replace) {
        nextRules = rulesAiDraft.rules.slice();
    } else {
        const ids = rulesAiNextIds(rulesAiDraft.rules.length);
        const existing = (rulesCfg.rules || []).slice();
        const added = rulesAiDraft.rules.map(function(r, i) {
            const x = Object.assign({}, r);
            x.id = ids[i];
            return x;
        });
        nextRules = existing.concat(added);
    }
    const body = {
        rules: nextRules
    };
    const btn = document.getElementById('rules-ai-apply');
    if (btn) btn.disabled = true;
    try {
        const r = await fetch('/api/rules/import', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify(body)
        });
        const j = await r.json().catch(function() {
            return {};
        });
        if (!r.ok) {
            toast(t('rules.ai_apply_failed', { err: (j.message || r.status) }));
            return;
        }
        toast(t('rules.ai_applied', { n: (j.count != null ? j.count : nextRules.length) }));
        rulesAiCancel();
        rulesSel = null;
        await loadRules();
    } catch (e) {
        toast(t('rules.apply_failed'));
    } finally {
        if (btn) btn.disabled = false;
    }
}

function rulesOpLabel(op) {
    const m = {
        lt: '<',
        le: '≦',
        eq: '=',
        gt: '>',
        ge: '≧',
        ne: '≠'
    };
    return m[op] || op || '=';
}

function rulesCondValue(c) {
    if (!c || typeof c !== 'object') return '';
    if (c.value != null && c.value !== '') return String(c.value);
    if (c.equals != null && c.equals !== '') return String(c.equals);
    return '';
}

function rulesCondOp(c) {
    const raw = (c && c.op) ? String(c.op) : 'eq';
    if (raw === '<' || raw === 'lt') return 'lt';
    if (raw === '<=' || raw === '≦' || raw === 'le') return 'le';
    if (raw === '>' || raw === 'gt') return 'gt';
    if (raw === '>=' || raw === '≧' || raw === 'ge') return 'ge';
    if (raw === '!=' || raw === 'ne' || raw === '<>') return 'ne';
    return 'eq';
}

function rulesDeviceLabel(pointId) {
    if (!pointId) return '?';
    const segs = String(pointId).split('.');
    if (segs.length >= 2) return segs[segs.length - 1];
    return pointId;
}

function whenSummary(w) {
    if (!w || typeof w !== 'object') return '-';
    if (w.type === 'time') return t('rules.summary_time', { at: (w.at || '') });
    if (w.type === 'point') {
        const dev = rulesDeviceLabel(w.point_id);
        const param = w.parameter ? String(w.parameter) : '';
        const op = rulesOpLabel(rulesCondOp(w));
        const val = rulesCondValue(w);
        if (param) return dev + '.' + param + ' ' + op + ' ' + val;
        return dev + ' ' + op + ' ' + val;
    }
    return w.type || '-';
}

/* Short, one-line description of a single action (used by the list/detail views,
 * which now render the steps form: each step is a group of actions + delay_after). */
function thenActionSummary(a) {
    if (!a || typeof a !== 'object') return '';
    if (a.type === 'delay') {
        const n = a.delay_sec != null ? a.delay_sec :
            (a.seconds != null ? a.seconds : (a.ms != null ? Math.round(a.ms / 1000) : '?'));
        return t('rules.summary_delay', { n: n });
    }
    if (a.type === 'command') return (a.cmd || '?') + ' → ' + rulesDeviceLabel(a.point_id || '?');
    if (a.type === 'command_by_type') return (a.cmd || '?') + ' → ' + (a.types || '?');
    if (a.type === 'notify') return t('rules.opt_notify') + (a.message ? ': ' + a.message : '');
    return a.type || '-';
}

function thenSummary(th) {
    const steps = rulesThenList(th);
    return steps.map(function(st, i) {
        const acts = (st.actions || []).map(thenActionSummary);
        let s = acts.join(' › ');
        if (st.delay_after > 0) s += ' › ' + t('rules.summary_delay', { n: st.delay_after });
        return (steps.length > 1 ? ((i + 1) + '. ') : '') + s;
    }).join('<br>');
}

function whenPlain(w) {
    if (!w || typeof w !== 'object') return {
        title: t('rules.when_unknown'),
        body: t('rules.when_unconfigured')
    };
    if (w.type === 'time') return {
        title: t('rules.when_time'),
        body: t('rules.when_time_body', { at: esc(w.at || '??:??') })
    };
    if (w.type === 'delay') return {
        title: t('rules.when_delay_type'),
        body: t('rules.when_delay_body', { n: (w.delay_sec > 0 ? w.delay_sec : '?') })
    };
    if (w.type === 'point') {
        const dev = rulesDeviceLabel(w.point_id);
        const param = w.parameter ? String(w.parameter) : '';
        const op = rulesOpLabel(rulesCondOp(w));
        const val = rulesCondValue(w);
        const target = param ? (esc(dev) + ' · <b>' + esc(param) + '</b>') : ('<b>' + esc(dev) + '</b>');
        return {
            title: t('rules.device_state_change'),
            body: t('rules.when_device_body', { target: target, op: esc(op), val: esc(val || '?') })
        };
    }
    return {
        title: esc(w.type || t('rules.trigger')),
        body: esc(JSON.stringify(w))
    };
}

function ifPlain(iff) {
    const arr = Array.isArray(iff) ? iff : [];
    if (!arr.length) return {
        title: t('rules.if_none'),
        body: t('rules.if_none_body')
    };
    const lines = arr.map(function(c, i) {
        if (!c || typeof c !== 'object') return (i + 1) + '. ' + t('rules.if_invalid');
        const dev = rulesDeviceLabel(c.point_id);
        const param = c.parameter ? String(c.parameter) : '';
        const op = rulesOpLabel(rulesCondOp(c));
        const val = rulesCondValue(c);
        const target = param ? (esc(dev) + ' · <b>' + esc(param) + '</b>') : ('<b>' + esc(dev) + '</b>');
        return (i + 1) + '. ' + target + ' ' + esc(op) + ' <b>' + esc(val || '?') + '</b>';
    }).join('<br>');
    return {
        title: t('rules.if_also'),
        body: lines
    };
}

function thenPlain(th) {
    if (!th || typeof th !== 'object') return {
        title: t('rules.then_unknown'),
        body: t('rules.then_unconfigured')
    };
    const steps = rulesThenList(th);
    const stepOne = function(st, i) {
        const parts = (st.actions || []).map(thenActionSummary);
        let body = parts.join(' › ');
        if (st.delay_after > 0) body += ' › ' + t('rules.summary_delay', { n: st.delay_after });
        return (steps.length > 1 ? ('<b>' + (i + 1) + '.</b> ') : '') + body;
    };
    const body = steps.map(stepOne).join('<br>');
    return {
        title: t('rules.then_steps'),
        body: body
    };
}

function ruleStepHtml(stepLabel, plain) {
    return '<div class="rule-step"><div class="rule-step-label">' + stepLabel + '</div><div class="rule-step-title">' + plain.title + '</div><div class="rule-step-body">' + plain.body + '</div></div>';
}

async function loadRules() {
    dwRuleLoadCatalog();
    try {
        const r = await fetch('/api/rules');
        if (!r.ok) throw new Error('HTTP ' + r.status);
        rulesCfg = await r.json();
        if (!rulesCfg || !Array.isArray(rulesCfg.rules)) rulesCfg = {
            rules: []
        };
        if (view === 'rules') renderRules();
    } catch (e) {
        toast(t('rules.load_failed'));
        const el = document.getElementById('rules-list');
        if (el) el.innerHTML = '<div class="empty">' + t('rules.load_error') + '</div>';
    }
}

const RULE_OPS = [
    { op: 'lt', label: '<' },
    { op: 'le', label: '≦' },
    { op: 'eq', label: '=' },
    { op: 'gt', label: '>' },
    { op: 'ge', label: '≧' }
];

/* Bool parameters (toggle/power/...) only make sense with equality, so the editor
 * hides the ordering operators for them. */
const RULE_OPS_BOOL = [
    { op: 'eq', label: '=' },
    { op: 'ne', label: '!=' }
];

/* Clamp a numeric value to the parameter's [min, max] range. Non-numeric or
 * range-less values pass through untouched. */
function rulesClampValue(meta, raw) {
    if (!meta || meta.min == null || meta.max == null) return raw;
    const v = (raw == null ? '' : String(raw)).trim();
    if (v === '') return raw;
    const n = Number(v);
    if (isNaN(n)) return raw;
    let c = n;
    if (c < meta.min) c = meta.min;
    if (c > meta.max) c = meta.max;
    return String(c);
}

/* Snap a numeric <input> into its own [min,max] range in place.
 * `which` picks the bound to enforce: 'max' (upper only — safe to run on every
 * keystroke, since typing can only overshoot above), 'min', or undefined for
 * both (run on change/blur). This lets an over-range entry settle at the bound
 * immediately (brightness 100000 -> 100) while a below-range entry waits until
 * the field is left, so mid-type digits are never clamped (temperature 500 ->
 * 2700 only on blur). Reads the bounds straight from the element's attributes. */
function rulesClampInput(el, which) {
    if (!el || el.type !== 'number') return;
    const raw = String(el.value == null ? '' : el.value).trim();
    if (raw === '') return;
    const n = Number(raw);
    if (isNaN(n)) return;
    const min = (el.min !== '') ? Number(el.min) : null;
    const max = (el.max !== '') ? Number(el.max) : null;
    let c = n;
    if (which !== 'max' && min != null && !isNaN(min) && c < min) c = min;
    if (which !== 'min' && max != null && !isNaN(max) && c > max) c = max;
    if (String(c) !== raw) el.value = String(c);
}

// Per device-type action commands. `value:true` means the command needs a
// parameter value; `param` is the key the adapter expects inside `params`
// (e.g. set_brightness -> params.brightness). `unit`/`min`/`max` shape the input.
const RULE_TYPE_CMDS = {
    light: [
        { cmd: 'turn_on', labelKey: 'rules.opt_on' },
        { cmd: 'turn_off', labelKey: 'rules.opt_off' },
        { cmd: 'toggle', labelKey: 'rules.opt_toggle' },
        { cmd: 'set_brightness', labelKey: 'rules.set_brightness', value: true, param: 'brightness', unit: '%', min: 1, max: 100 },
        { cmd: 'set_color_temp', labelKey: 'rules.set_color_temp', value: true, param: 'color_temp', unit: 'K', min: 2700, max: 6500 },
        { cmd: 'set_color', labelKey: 'rules.set_color', value: true, param: 'color' }
    ],
    switch: [
        { cmd: 'turn_on', labelKey: 'rules.opt_on' },
        { cmd: 'turn_off', labelKey: 'rules.opt_off' },
        { cmd: 'toggle', labelKey: 'rules.opt_toggle' }
    ],
    plug: [
        { cmd: 'turn_on', labelKey: 'rules.opt_on' },
        { cmd: 'turn_off', labelKey: 'rules.opt_off' },
        { cmd: 'toggle', labelKey: 'rules.opt_toggle' }
    ],
    cover: [
        { cmd: 'open', labelKey: 'rules.opt_open' },
        { cmd: 'close', labelKey: 'rules.opt_close' },
        { cmd: 'stop', labelKey: 'rules.opt_stop' },
        { cmd: 'set_position', labelKey: 'rules.set_position', value: true, param: 'position', unit: '%', min: 0, max: 100 }
    ],
    curtain: [
        { cmd: 'open', labelKey: 'rules.opt_open' },
        { cmd: 'close', labelKey: 'rules.opt_close' },
        { cmd: 'stop', labelKey: 'rules.opt_stop' },
        { cmd: 'set_position', labelKey: 'rules.set_position', value: true, param: 'position', unit: '%', min: 0, max: 100 }
    ],
    airer: [
        { cmd: 'open', labelKey: 'rules.opt_open' },
        { cmd: 'close', labelKey: 'rules.opt_close' },
        { cmd: 'stop', labelKey: 'rules.opt_stop' },
        { cmd: 'set_position', labelKey: 'rules.set_position', value: true, param: 'position', unit: '%', min: 0, max: 100 }
    ],
    air_conditioner: [
        { cmd: 'turn_on', labelKey: 'rules.opt_on' },
        { cmd: 'turn_off', labelKey: 'rules.opt_off' },
        { cmd: 'set_temperature', labelKey: 'rules.set_temperature', value: true, param: 'temperature', unit: '°C', min: 16, max: 30 },
        { cmd: 'set_mode', labelKey: 'rules.set_mode', value: true, param: 'mode' },
        { cmd: 'set_fan_level', labelKey: 'rules.set_fan_level', value: true, param: 'fan_level' }
    ],
    fan: [
        { cmd: 'turn_on', labelKey: 'rules.opt_on' },
        { cmd: 'turn_off', labelKey: 'rules.opt_off' },
        { cmd: 'toggle', labelKey: 'rules.opt_toggle' },
        { cmd: 'set_fan_level', labelKey: 'rules.set_fan_level', value: true, param: 'fan_level' }
    ],
    speaker: [
        { cmd: 'turn_on', labelKey: 'rules.opt_on' },
        { cmd: 'turn_off', labelKey: 'rules.opt_off' },
        { cmd: 'set_volume', labelKey: 'rules.set_volume', value: true, param: 'volume', unit: '%', min: 0, max: 100 }
    ],
    television: [
        { cmd: 'turn_on', labelKey: 'rules.opt_on' },
        { cmd: 'turn_off', labelKey: 'rules.opt_off' },
        { cmd: 'set_volume', labelKey: 'rules.set_volume', value: true, param: 'volume', unit: '%', min: 0, max: 100 }
    ]
};

/* Labels must be resolved at render time (not baked into the table above): the
 * i18n dictionaries load asynchronously, so a module-scope t() call runs before
 * `strings`/`base` are populated and would freeze every label as a raw key. */
function rulesCmdLabel(d) {
    if (!d) return '';
    return d.labelKey ? t(d.labelKey) : (d.label || d.cmd || '');
}

function rulesCmdDefsForType(type) {
    if (type && RULE_TYPE_CMDS[type]) return RULE_TYPE_CMDS[type];
    return [
        { cmd: 'turn_on', labelKey: 'rules.opt_on' },
        { cmd: 'turn_off', labelKey: 'rules.opt_off' },
        { cmd: 'toggle', labelKey: 'rules.opt_toggle' }
    ];
}

function rulesCmdDefFor(cmd, type) {
    return rulesCmdDefsForType(type).find(function(d) {
        return d.cmd === cmd;
    }) || null;
}

/* Range/unit for a command's value input. Prefer what the SELECTED DEVICE
 * actually reports (props.params.<param>.min/max — a real light publishes its
 * own range, e.g. 1-100 or 1-80); otherwise fall back to the per-type defaults
 * in RULE_TYPE_CMDS. Without this the action row showed a hardcoded 1-100 while
 * the WHEN condition row used the device's own range, so the same brightness
 * looked like two different limits on the same rule. */
function rulesActionValueRange(pid, param, def) {
    const meta = (pid && param) ? rulesParamMeta(pid, param) : null;
    return {
        min: (meta && meta.min != null) ? meta.min : ((def && def.min != null) ? def.min : null),
        max: (meta && meta.max != null) ? meta.max : ((def && def.max != null) ? def.max : null),
        unit: (meta && meta.unit) ? meta.unit : ((def && def.unit) ? def.unit : '')
    };
}

let dwRuleCatalog = null;

async function dwRuleLoadCatalog() {
    try {
        const r = await fetch(window.mhCatalogUrl('/api/device_widgets'));
        if (r.ok) dwRuleCatalog = await r.json();
    } catch (e) {  }
}

function rulesParseStyle(raw) {
    if (!raw) return {};
    if (typeof raw === 'object') return raw;
    try {
        return JSON.parse(raw) || {};
    } catch (e) {
        return {};
    }
}

function rulesCatalogForType(type) {
    if (!dwRuleCatalog || !Array.isArray(dwRuleCatalog.devices) || !type) return null;
    for (let i = 0; i < dwRuleCatalog.devices.length; i++) {
        const d = dwRuleCatalog.devices[i];
        if (d && d.device === type) return d;
    }
    return null;
}

function rulesCatalogParam(type, parameter) {
    const d = rulesCatalogForType(type);
    if (!d || !Array.isArray(d.params)) return null;
    for (let i = 0; i < d.params.length; i++) {
        const p = d.params[i];
        if (p && p.parameter === parameter) return p;
    }
    return null;
}

function rulesDeviceInstances() {
    if (typeof dwBuildInstances === 'function') {
        return dwBuildInstances(pts || []);
    }
    const out = [];
    (pts || []).forEach(function(pt) {
        if (!pt || !pt.point_id) return;
        const segs = String(pt.point_id).split('.');
        const pr = (pt.props && typeof pt.props === 'object') ? pt.props : null;
        const params = pr && pr.params;
        const hasParams = !!(params && typeof params === 'object' &&
            (Array.isArray(params) ? params.length : Object.keys(params).length));
        if (hasParams || segs.length === 2 || segs.length < 2) {
            const tail = String(pt.point_id).split('/').filter(Boolean);
            out.push({
                point_id: pt.point_id,
                type: pt.type || '',
                endpoint: segs.length >= 2 ? segs[segs.length - 1] : (tail.length ? tail[tail.length - 1] : pt.point_id),
                params: Object.create(null)
            });
        }
    });
    return out;
}

function rulesFindPoint(pointId) {
    return (pts || []).find(function(p) {
        return p && p.point_id === pointId;
    }) || null;
}

function rulesLiveParams(pointId) {
    if (typeof dwBuildInstances === 'function') {
        const insts = dwBuildInstances(pts || []);
        const inst = insts.find(function(x) {
            return x.point_id === pointId || x.key === pointId;
        });
        if (inst && inst.params) return inst.params;
    }
    const pt = rulesFindPoint(pointId);
    const blob = pt && pt.props && pt.props.params;
    const out = Object.create(null);
    if (!blob || typeof blob !== 'object') {
        if (pt && pt.value != null && String(pt.value) !== '') {
            out.power = {
                name: 'power',
                value: String(pt.value),
                visible: true
            };
        }
        return out;
    }
    if (Array.isArray(blob)) {
        blob.forEach(function(ent) {
            if (!ent || !ent.name) return;
            out[ent.name] = {
                name: ent.name,
                value: ent.value != null ? String(ent.value) : '',
                visible: !(ent.visible === false),
                min: ent.min,
                max: ent.max,
                step: ent.step,
                unit: ent.unit
            };
        });
    } else {
        Object.keys(blob).forEach(function(name) {
            const ent = blob[name] || {};
            out[name] = {
                name: name,
                value: ent.value != null ? String(ent.value) : '',
                visible: !(ent.visible === false),
                min: ent.min,
                max: ent.max,
                step: ent.step,
                unit: ent.unit
            };
        });
    }
    return out;
}

function rulesVisibleParams(pointId) {
    const pt = rulesFindPoint(pointId);
    const type = (pt && pt.type) || '';
    const live = rulesLiveParams(pointId);
    const cat = rulesCatalogForType(type);
    const names = [];
    const seen = Object.create(null);
    const order = cat && Array.isArray(cat.params) ?
        cat.params.map(function(p) {
            return p.parameter;
        }) : Object.keys(live);
    order.forEach(function(name) {
        if (!name || seen[name]) return;
        const lv = live[name];
        if (!lv || lv.visible === false) return;
        seen[name] = true;
        names.push(name);
    });
    Object.keys(live).forEach(function(name) {
        if (seen[name]) return;
        if (live[name].visible === false) return;
        names.push(name);
    });
    return names;
}

function rulesParamMeta(pointId, parameter) {
    const pt = rulesFindPoint(pointId);
    const type = (pt && pt.type) || '';
    const cat = rulesCatalogParam(type, parameter) || {
        parameter: parameter,
        label: parameter,
        widget: '',
        style: '{}'
    };
    const style = rulesParseStyle(cat.style);
    const live = rulesLiveParams(pointId)[parameter] || {};
    const widget = (cat.widget || style.widget || style.ui_element || '').replace(/^ui\./, '').replace(/-/g, '_');
    return {
        parameter: parameter,
        label: cat.label || style.label || parameter,
        widget: widget,
        min: live.min != null ? live.min : (style.min != null ? style.min : null),
        max: live.max != null ? live.max : (style.max != null ? style.max : null),
        step: live.step != null ? live.step : (style.step != null ? style.step : null),
        unit: live.unit != null ? live.unit : (style.unit || ''),
        options: Array.isArray(style.options) ? style.options : []
    };
}

function rulesDeviceOptionsHtml(selected) {
    const insts = rulesDeviceInstances();
    let ids = insts.map(function(x) {
        return x.point_id;
    }).filter(Boolean);
    if (!ids.length) {
        (pts || []).forEach(function(p) {
            if (!p || !p.point_id) return;
            /* Hide devices belonging to an auth adapter (e.g. miot) that is logged
             * out, so devices from a previous link / removed account no longer linger
             * in the rule device picker (issue #9). The core already unregisters
             * these points on sync/logout; this is a UI-side safeguard for the
             * transient window before that completes. */
            if (p.adapter && typeof authStatus === 'function') {
                const st = authStatus(p.adapter);
                if (st && st.loaded === true && st.logged_in === false) return;
            }
            ids.push(p.point_id);
        });
    }
    if (selected && ids.indexOf(selected) < 0) ids.unshift(selected);
    if (!ids.length) ids = [''];
    return ids.map(function(id) {
        const pt = rulesFindPoint(id);
        const label = rulesDeviceLabel(id) + (pt && pt.type ? ' (' + pt.type + ')' : '');
        return '<option value="' + escAttr(id) + '"' + (id === selected ? ' selected' : '') + '>' +
            esc(label || t('rules.no_devices')) + '</option>';
    }).join('');
}

function rulesParamOptionsHtml(pointId, selected) {
    const names = rulesVisibleParams(pointId);
    if (selected && names.indexOf(selected) < 0) names.unshift(selected);
    if (!names.length) {
        return '<option value="">' + t('rules.no_params') + '</option>';
    }
    return names.map(function(name) {
        const meta = rulesParamMeta(pointId, name);
        return '<option value="' + escAttr(name) + '"' + (name === selected ? ' selected' : '') + '>' +
            esc(meta.label || name) + '</option>';
    }).join('');
}

function rulesOpOptionsHtml(selected, boolOnly) {
    const list = boolOnly ? RULE_OPS_BOOL : RULE_OPS;
    const sel = selected || (boolOnly ? 'eq' : 'eq');
    return list.map(function(o) {
        return '<option value="' + o.op + '"' + (o.op === sel ? ' selected' : '') + '>' +
            esc(o.label) + '</option>';
    }).join('');
}

function rulesValueInputHtml(id, pointId, parameter, value) {
    const v = value != null ? String(value) : '';
    if (!parameter) {
        return '<input type="text" id="' + id + '" value="' + escAttr(v) + '" placeholder="' + t('rules.field_value') + '" />';
    }
    const meta = rulesParamMeta(pointId, parameter);
    const w = meta.widget || '';
    if (w === 'toggle' || w === 'bool' || parameter === 'power') {
        const on = (v === 'on' || v === 'true' || v === '1' || v === 'ON');
        const off = (v === 'off' || v === 'false' || v === '0' || v === 'OFF');
        return '<select id="' + id + '">' +
            '<option value="on"' + (on || (!on && !off && !v) ? ' selected' : '') + '>' + t('rules.opt_on') + '</option>' +
            '<option value="off"' + (off ? ' selected' : '') + '>' + t('rules.opt_off') + '</option></select>';
    }
    if ((w === 'dropdown' || w === 'segmented') && meta.options && meta.options.length) {
        let body = '';
        meta.options.forEach(function(o) {
            const val = (o && typeof o === 'object') ? o.value : o;
            const L = (o && o.labels) ? o.labels : {};
            const lbl = L[window.mhCatalogLang()] || L.zh || (o && o.label) || (o && o.value) || o;
            const sel = (v !== '' && String(val) === v) ? ' selected' : '';
            body += '<option value="' + escAttr(String(val)) + '"' + sel + '>' + esc(String(lbl)) + '</option>';
        });
        return '<select id="' + id + '">' + body + '</select>';
    }
    if (meta.min != null && meta.max != null) {
        const min = meta.min;
        const max = meta.max;
        const step = meta.step != null ? meta.step : 1;
        const num = (v !== '' && !isNaN(Number(v))) ? Number(v) : min;
        const unit = meta.unit ? (' <span class="rule-ed-unit">' + esc(meta.unit) + '</span>') : '';
        return '<input type="number" id="' + id + '" min="' + escAttr(String(min)) +
            '" max="' + escAttr(String(max)) + '" step="' + escAttr(String(step)) +
            '" value="' + escAttr(String(num)) +
            '" oninput="rulesClampInput(this,\'max\')" onchange="rulesClampInput(this)" />' + unit;
    }
    return '<input type="text" id="' + id + '" value="' + escAttr(v) + '" placeholder="' + t('rules.field_value') + '" />';
}

function rulesSelectHtml(id, optsHtml, onchange) {
    return '<select id="' + id + '"' + (onchange ? ' onchange="' + onchange + '"' : '') + '>' + optsHtml + '</select>';
}

function rulesPointCondFields(prefix, cond, onDevChange, onParamChange) {
    const c = cond && typeof cond === 'object' ? cond : {};
    const pointId = c.point_id || '';
    const parameter = c.parameter || '';
    const op = rulesCondOp(c);
    const value = rulesCondValue(c);
    const meta = (pointId && parameter) ? rulesParamMeta(pointId, parameter) : null;
    const boolOnly = !!(meta && (meta.widget === 'toggle' || meta.widget === 'bool' || parameter === 'power'));
    return '<div class="row2">' +
        '<div><label>' + t('rules.field_device') + '</label>' + rulesSelectHtml(prefix + '-dev', rulesDeviceOptionsHtml(pointId), onDevChange) + '</div>' +
        '<div><label>' + t('rules.field_parameter') + '</label>' + rulesSelectHtml(prefix + '-param', rulesParamOptionsHtml(pointId, parameter), onParamChange) + '</div>' +
        '</div>' +
        '<div class="row2 rule-ed-opval">' +
        '<div><label>' + t('rules.field_condition') + '</label>' + rulesSelectHtml(prefix + '-op', rulesOpOptionsHtml(op, boolOnly)) + '</div>' +
        '<div><label>' + t('rules.field_value') + '</label><div class="rule-ed-val">' + rulesValueInputHtml(prefix + '-val', pointId, parameter, value) + (meta && meta.min != null && meta.max != null ? '<span class="rule-ed-range">' + esc(meta.min + '–' + meta.max + (meta.unit || '')) + '</span>' : '') + '</div></div>' +
        '</div>';
}

function rulesCollectPointCond(prefix) {
    const dev = document.getElementById(prefix + '-dev');
    const param = document.getElementById(prefix + '-param');
    const opEl = document.getElementById(prefix + '-op');
    const valEl = document.getElementById(prefix + '-val');
    const point_id = ((dev && dev.value) || '').trim();
    const parameter = ((param && param.value) || '').trim();
    const op = ((opEl && opEl.value) || 'eq').trim() || 'eq';
    let value = '';
    if (valEl) {
        if (valEl.type === 'checkbox') value = valEl.checked ? 'on' : 'off';
        else value = String(valEl.value != null ? valEl.value : '').trim();
    }
    value = rulesClampValue(rulesParamMeta(point_id, parameter), value);
    const out = {
        point_id: point_id,
        parameter: parameter,
        op: op,
        value: value
    };

    if (op === 'eq') out.equals = value;
    return out;
}

function rulesBack() {
    rulesSel = null;
    rulesEditMode = null;
    renderRules();
}
async function rulesEditNew() {
    if (!pts || !pts.length) {
        try {
            await loadPoints();
        } catch (e) {}
    }
    try {
        await dwRuleLoadCatalog();
    } catch (e) {}
    rulesSel = null;
    rulesEditMode = 'new';
    rulesEditRender(null);
}
async function rulesEditOpen(id) {
    if (!pts || !pts.length) {
        try {
            await loadPoints();
        } catch (e) {}
    }
    try {
        await dwRuleLoadCatalog();
    } catch (e) {}
    const rule = (rulesCfg.rules || []).find(function(x) {
        return x.id === id;
    });
    if (!rule) {
        toast(t('rules.rule_not_found'));
        return;
    }
    rulesEditMode = id;
    rulesEditRender(rule);
}

function rulesEditCancel() {
    rulesEditMode = null;
    rulesSel = null;
    renderRules();
}

function rulesEditWhenFields(when) {
    const w = when && typeof when === 'object' ? when : {
        type: 'point'
    };
    const typ = w.type === 'time' ? 'time' : (w.type === 'delay' ? 'delay' : 'point');
    let html = '<label>' + t('rules.trigger_type') + '</label><select id="re-when-type" onchange="rulesEditSyncWhenType()">' +
        '<option value="point"' + (typ === 'point' ? ' selected' : '') + '>' + t('rules.device_state_change') + '</option>' +
        '<option value="time"' + (typ === 'time' ? ' selected' : '') + '>' + t('rules.opt_time_type') + '</option>' +
        '<option value="delay"' + (typ === 'delay' ? ' selected' : '') + '>' + t('rules.when_delay_type') + '</option></select>';
    if (typ === 'point') {
        const first = rulesDeviceInstances()[0];
        const cond = {
            point_id: w.point_id || (first && first.point_id) || '',
            parameter: w.parameter || '',
            op: rulesCondOp(w),
            value: rulesCondValue(w) || (w.equals != null ? String(w.equals) : '')
        };
        if (!cond.parameter && cond.point_id) {
            const names = rulesVisibleParams(cond.point_id);
            cond.parameter = names[0] || '';
        }
        if (!cond.value && cond.parameter === 'power') cond.value = 'on';
        html += '<div id="re-when-point">' + rulesPointCondFields('re-when', cond,
            'rulesEditSyncWhenDevice()', 'rulesEditSyncWhenParam()') + '</div>';
    } else if (typ === 'time') {
        html += '<label>' + t('rules.field_time_at') + '</label><input type="time" id="re-when-at" value="' + escAttr(w.at || '23:00') + '" />';
    } else {
        html += '<div class="rule-ed-when-delay"><label>' + t('rules.delay_seconds') + '</label>' +
            '<input type="number" id="re-when-delay-sec" min="0" value="' + (w.delay_sec > 0 ? w.delay_sec : '') + '" placeholder="0" />' +
            '<span class="rule-ed-unit">s</span></div>';
    }
    return html;
}

/* THEN is modelled as an ordered sequence of "steps". A step runs its actions
 * together, then (if not the last step and delay_after > 0) waits before the
 * next step. Each step carries at most one delay, which is exactly what separates
 * the moments. The firmware executes steps non-blocking (esp_timer chain). */
const MAX_STEPS_WEB = 5;
const MAX_ACTIONS_PER_STEP_WEB = 4;

/* Normalize any `then` payload into the step form:
 *   [ { actions:[...], delay_after:N }, ... ]
 * Also handles the legacy flat form (a bare action object, or an array of actions
 * where a `delay` action becomes the preceding step's delay_after and opens a new
 * step), so existing rules keep loading and editing. */
function rulesThenList(then) {
    const steps = [];
    const isSteps = Array.isArray(then) && then.length > 0 &&
        then[0] && typeof then[0] === 'object' && then[0].actions;
    if (isSteps) {
        then.forEach(function (s) {
            if (!s || typeof s !== 'object') return;
            const actions = Array.isArray(s.actions) ? s.actions.filter(function (a) {
                return a && typeof a === 'object';
            }) : [];
            const raw = (s.delay_after != null) ? s.delay_after : (s.delay != null ? s.delay : 0);
            steps.push({ actions: actions, delay_after: (raw > 0 ? raw : 0) });
        });
    } else if (Array.isArray(then)) {
        let cur = 0;
        then.forEach(function (a) {
            if (!a || typeof a !== 'object') return;
            if (a.type === 'delay') {
                if (steps.length <= cur) {
                    if (steps.length < MAX_STEPS_WEB) steps.push({ actions: [], delay_after: 0 });
                    else return;
                }
                steps[cur].delay_after = (a.delay_sec > 0 ? a.delay_sec : (a.seconds > 0 ? a.seconds : 0));
                cur++;
            } else {
                if (steps.length <= cur) {
                    if (steps.length < MAX_STEPS_WEB) steps.push({ actions: [], delay_after: 0 });
                    else return;
                }
                steps[cur].actions.push(a);
            }
        });
    } else if (then && typeof then === 'object') {
        steps.push({ actions: [then], delay_after: 0 });
    }
    if (steps.length === 0) {
        steps.push({ actions: [{ type: 'command', point_id: '', cmd: 'turn_on' }], delay_after: 0 });
    }
    return steps;
}

function rulesEditActionHtml(si, aj, a) {
    const act = (a && typeof a === 'object') ? a : { type: 'command' };
    const typ = (act.type === 'command_by_type' || act.type === 'notify' || act.type === 'delay') ? act.type : 'command';
    const idx = String(si);
    const j = String(aj);
    const sel = function (v) { return typ === v ? ' selected' : ''; };
    let html = '<div class="rule-then-row" data-step-i="' + idx + '" data-act-j="' + j + '">' +
        '<div class="rule-then-row-top"><label>' + t('rules.action_type') + '</label>' +
        '<select id="re-step-' + idx + '-act-' + j + '-type" onchange="rulesEditRerenderThen()">' +
        '<option value="command"' + sel('command') + '>' + t('rules.opt_cmd_type') + '</option>' +
        /* `command_by_type` is no longer offered; keep it visible (unselectable) only
         * for rules that already carry one, so editing them cannot silently rewrite it. */
        (typ === 'command_by_type' ?
            '<option value="command_by_type" selected disabled>' + t('rules.opt_cmd_by_type') + '</option>' : '') +
        '<option value="notify"' + sel('notify') + '>' + t('rules.opt_notify') + '</option>' +
        '<option value="delay"' + sel('delay') + '>' + t('rules.opt_delay') + '</option></select>';
    /* The row is always removable: emptying a step is a normal intermediate state
     * (the card then shows “no action yet” and can be refilled). */
    html += '<button class="btn-sm bad" type="button" onclick="rulesEditRemoveStepAction(' + idx + ',' + j + ')">' + t('rules.delete') + '</button>';
    html += '</div><div class="rule-then-fields">';
    if (typ === 'command') {
        const pid = act.point_id || '';
        const type = (rulesFindPoint(pid) || {}).type || '';
        const defs = rulesCmdDefsForType(type);
        const cmd = act.cmd || (defs[0] && defs[0].cmd) || 'turn_on';
        const cmdOpts = defs.map(function (d) {
            return '<option value="' + d.cmd + '"' + (d.cmd === cmd ? ' selected' : '') + '>' + esc(rulesCmdLabel(d)) + '</option>';
        }).join('');
        const extra = (rulesCmdDefFor(cmd, type) == null) ?
            '<option value="' + escAttr(cmd) + '" selected>' + esc(cmd) + '</option>' : '';
        html += '<div><label>' + t('rules.target_device') + '</label>' +
            rulesSelectHtml('re-step-' + idx + '-act-' + j + '-pid', rulesDeviceOptionsHtml(pid), 'rulesEditRerenderThen()') + '</div>';
        html += '<div><label>' + t('rules.field_cmd') + '</label>' +
            '<select id="re-step-' + idx + '-act-' + j + '-cmd" onchange="rulesEditRerenderThen()">' + extra + cmdOpts + '</select></div>';
        const active = rulesCmdDefFor(cmd, type);
        if (active && active.value) {
            const curVal = (active.param && act.params && act.params[active.param] != null) ? act.params[active.param] : '';
            /* Resolved exactly like the WHEN row: the device picked in this row
             * decides the range, so both rows of the same rule agree. */
            const rng = rulesActionValueRange(pid, active.param, active);
            const unit = rng.unit ? ' (' + rng.unit + ')' : '';
            const min = (rng.min != null) ? ' min="' + rng.min + '"' : '';
            const max = (rng.max != null) ? ' max="' + rng.max + '"' : '';
            const rngHint = (rng.min != null && rng.max != null)
                ? '<span class="rule-ed-range">' + esc(rng.min + '–' + rng.max + (rng.unit || '')) + '</span>'
                : '';
            html += '<div><label>' + t('rules.field_value') + esc(unit) + '</label>' +
                '<div class="rule-ed-val">' +
                '<input type="number" id="re-step-' + idx + '-act-' + j + '-value"' + min + max +
                ' value="' + escAttr(curVal) + '" placeholder="' + escAttr(active.param || '') +
                '" oninput="rulesClampInput(this,\'max\')" onchange="rulesClampInput(this)" />' +
                rngHint + '</div></div>';
        }
    } else if (typ === 'command_by_type') {
        const cmd = act.cmd || 'turn_on';
        const types = act.types || 'light,switch';
        html += '<div><label>' + t('rules.field_cmd') + '</label>' +
            '<input type="text" id="re-step-' + idx + '-act-' + j + '-cmd" value="' + escAttr(cmd) + '" /></div>';
        html += '<div><label>' + t('rules.target_types') + '</label>' +
            '<input type="text" id="re-step-' + idx + '-act-' + j + '-types" value="' + escAttr(types) + '" /></div>';
    } else if (typ === 'delay') {
        const sec = (a.delay_sec > 0 || a.seconds > 0) ? (a.delay_sec || a.seconds) : '';
        html += '<div><label>' + t('rules.delay_seconds') + '</label>' +
            /* Typing the seconds must reveal “+ add step” right away, but a full
             * re-render would steal the focus — only the footer is refreshed. */
            '<input type="number" id="re-step-' + idx + '-act-' + j + '-sec" min="0" value="' + escAttr(sec) + '" placeholder="0" oninput="rulesEditSyncThenFoot()" />' +
            '<span class="rule-ed-unit">s</span></div>';
    } else {
        const msg = act.message || '';
        html += '<div><label>' + t('rules.field_message') + '</label>' +
            '<input type="text" id="re-step-' + idx + '-act-' + j + '-msg" value="' + escAttr(msg) + '" /></div>';
    }
    html += '</div></div>';
    return html;
}

/* Editor view of THEN: every moment is one step, and the wait before the next
 * moment lives inside the step as its trailing `delay` action — i.e. the delay is
 * picked from the action-type dropdown instead of a separate field.
 * Idempotent: inline `delay` rows and a `delay_after` both normalize to at most
 * one trailing wait, so a re-render never duplicates or loses rows. A zero-second
 * wait stays where it is (it is only being typed in). */
function rulesEditSteps(then) {
    return rulesThenList(then).map(function (m) {
        const actions = [];
        let wait = (m.delay_after > 0) ? m.delay_after : 0;
        (m.actions || []).forEach(function (a) {
            if (a && a.type === 'delay') {
                const sec = parseInt(a.delay_sec || a.seconds || 0, 10) || 0;
                if (sec > 0) wait = sec;
                else actions.push({ type: 'delay', delay_sec: 0 });
            } else {
                actions.push(a);
            }
        });
        if (wait > 0) actions.push({ type: 'delay', delay_sec: wait });
        return { actions: actions };
    });
}

/* Actions that the firmware actually stores; a wait row is not one of them. */
function rulesEditRealActionCount(step) {
    return ((step && step.actions) || []).filter(function (a) {
        return !a || a.type !== 'delay';
    }).length;
}

/* The next moment only makes sense once the current one ends with a wait, so
 * "+ add step" stays hidden until the last step carries a delay action. */
function rulesEditLastHasDelay(steps) {
    const last = steps[steps.length - 1];
    if (!last) return false;
    const acts = last.actions || [];
    const tail = acts[acts.length - 1];
    if (!tail || tail.type !== 'delay') return false;
    return ((tail.delay_sec > 0) || (tail.seconds > 0));
}

function rulesEditStepHtml(si, step, totalSteps) {
    const idx = String(si);
    const actions = Array.isArray(step.actions) ? step.actions : [];
    let html = '<div class="rule-step-card" data-step-i="' + idx + '">';
    if (totalSteps > 1) {
        html += '<div class="rule-step-head"><button class="btn-sm bad" type="button" onclick="rulesEditRemoveStep(' + idx + ')">' + t('rules.delete') + '</button></div>';
    }
    html += '<div class="rule-step-actions" data-step-actions="' + idx + '">';
    if (actions.length === 0) {
        html += '<div class="rule-step-empty">' + t('rules.step_no_action') + '</div>';
    } else {
        actions.forEach(function (a, aj) {
            if (aj > 0) html += '<div class="rule-then-sep"></div>';
            html += rulesEditActionHtml(si, aj, a, actions.length);
        });
    }
    html += '</div>';
    if (rulesEditRealActionCount(step) < MAX_ACTIONS_PER_STEP_WEB) {
        html += '<button class="btn-sm" type="button" onclick="rulesEditAddStepAction(' + idx + ')">' + t('rules.add_action') + '</button>';
    }
    html += '</div>';
    return html;
}

/* Footer under all step cards: “+ add step” appears only once the last step ends
 * with a real wait, otherwise a hint tells the user what is still missing. */
function rulesEditThenFootHtml(steps) {
    if (steps.length >= MAX_STEPS_WEB) return '';
    return rulesEditLastHasDelay(steps) ?
        '<button class="btn-sm on" type="button" onclick="rulesEditAddStep()">' + t('rules.add_step') + '</button>' :
        '<div class="rules-ai-hint">' + t('rules.hint_step_need_delay') + '</div>';
}

function rulesEditThenFields(then) {
    const steps = rulesEditSteps(then);
    let html = steps.map(function (s, i) {
        return rulesEditStepHtml(i, s, steps.length);
    }).join('');
    html += '<div id="re-then-foot">' + rulesEditThenFootHtml(steps) + '</div>';
    return html;
}

/* Refresh only the footer: typing a delay's seconds must reveal “+ add step”
 * immediately, but re-rendering the box would rebuild the input and blur it. */
function rulesEditSyncThenFoot() {
    const foot = document.getElementById('re-then-foot');
    if (!foot) return;
    foot.innerHTML = rulesEditThenFootHtml(rulesEditSteps(rulesEditCollectThen()));
}

function rulesEditCollectThen() {
    const box = document.getElementById('re-then-box');
    if (!box) return [{ actions: [{ type: 'command', point_id: '', cmd: 'turn_on' }], delay_after: 0 }];
    const steps = [];
    /* Only step cards carry `.rule-step-card`; action rows also set data-step-i,
     * so selecting by that attribute would count every action as its own step. */
    box.querySelectorAll('.rule-step-card').forEach(function (card) {
        const si = card.getAttribute('data-step-i');
        const actions = [];
        /* A `delay` row in a step is the wait before the NEXT moment, so it is not
         * kept as an action: the last delay in the step becomes its `delay_after`.
         * That also means picking "delay" anywhere in the list always settles as the
         * trailing wait of that step. */
        let delaySec = 0;
        card.querySelectorAll('[data-act-j]').forEach(function (row) {
            const j = row.getAttribute('data-act-j');
            const typEl = document.getElementById('re-step-' + si + '-act-' + j + '-type');
            const ttype = (typEl && typEl.value) || 'command';
            if (ttype === 'delay') {
                const secEl = document.getElementById('re-step-' + si + '-act-' + j + '-sec');
                let sec = parseInt((secEl && secEl.value) || '0', 10);
                if (isNaN(sec) || sec < 0) sec = 0;
                if (sec > 0) {
                    delaySec = sec;
                } else {
                    /* An empty wait is still being typed in: keep the row visible
                     * instead of dropping it on the next re-render. It is stripped
                     * from the saved rule (see rulesThenForSave). */
                    actions.push({ type: 'delay', delay_sec: 0 });
                }
            } else if (ttype === 'notify') {
                const msgEl = document.getElementById('re-step-' + si + '-act-' + j + '-msg');
                actions.push({ type: 'notify', message: ((msgEl && msgEl.value) || '').trim() });
            } else if (ttype === 'command_by_type') {
                const cmdEl = document.getElementById('re-step-' + si + '-act-' + j + '-cmd');
                const typesEl = document.getElementById('re-step-' + si + '-act-' + j + '-types');
                actions.push({
                    type: 'command_by_type',
                    cmd: ((cmdEl && cmdEl.value) || '').trim(),
                    types: ((typesEl && typesEl.value) || '').trim()
                });
            } else {
                const pidEl = document.getElementById('re-step-' + si + '-act-' + j + '-pid');
                const cmdEl = document.getElementById('re-step-' + si + '-act-' + j + '-cmd');
                const pid = ((pidEl && pidEl.value) || '').trim();
                const cmd = ((cmdEl && cmdEl.value) || '').trim();
                const entry = { type: 'command', point_id: pid, cmd: cmd };
                const type = (rulesFindPoint(pid) || {}).type || '';
                const def = rulesCmdDefFor(cmd, type);
                if (def && def.value && def.param) {
                    const vEl = document.getElementById('re-step-' + si + '-act-' + j + '-value');
                    const v = ((vEl && vEl.value) || '').trim();
                    if (v !== '') {
                        /* Same range the row rendered, so save-time clamping
                         * matches what the user saw. */
                        const rng = rulesActionValueRange(pid, def.param, def);
                        /* Whether the value is a number is a property of the
                         * command, not of the device, so keep judging it by the
                         * command def; only the bounds come from the device. */
                        const numeric = (def.unit === '%' || def.unit === 'K' || def.unit === '°C' || def.kind === 'int');
                        if (numeric) {
                            let n = parseInt(v, 10);
                            if (isNaN(n)) n = 0;
                            if (rng.min != null) n = Math.max(rng.min, n);
                            if (rng.max != null) n = Math.min(rng.max, n);
                            const params = {};
                            params[def.param] = n;
                            entry.params = params;
                        } else {
                            const params = {};
                            params[def.param] = v;
                            entry.params = params;
                        }
                    }
                }
                actions.push(entry);
            }
        });
        steps.push({ actions: actions, delay_after: delaySec });
    });
    if (steps.length === 0) {
        steps.push({ actions: [{ type: 'command', point_id: '', cmd: 'turn_on' }], delay_after: 0 });
    }
    return steps;
}

function rulesEditRerenderThenList(list) {
    const box = document.getElementById('re-then-box');
    if (!box) return;
    box.innerHTML = rulesEditThenFields(list);
}

function rulesEditRerenderThen() {
    rulesEditRerenderThenList(rulesEditCollectThen());
}

function rulesEditAddStep() {
    const steps = rulesEditSteps(rulesEditCollectThen());
    if (steps.length >= MAX_STEPS_WEB) {
        toast(t('rules.err_step_max', { n: MAX_STEPS_WEB }));
        return;
    }
    if (!rulesEditLastHasDelay(steps)) {
        toast(t('rules.hint_step_need_delay'));
        rulesEditSyncThenFoot();
        return;
    }
    const first = (rulesDeviceInstances()[0] || {}).point_id || '';
    steps.push({ actions: [{ type: 'command', point_id: first, cmd: 'turn_on' }] });
    rulesEditRerenderThenList(steps);
}

function rulesEditRemoveStep(i) {
    const steps = rulesEditSteps(rulesEditCollectThen());
    if (steps.length <= 1) return;
    steps.splice(i, 1);
    rulesEditRerenderThenList(steps);
}

/* All row level edits work on the *editor* shape (delay rows included), because
 * the collected step form folds the wait into `delay_after` — counting rows there
 * made a step of one action plus one wait look like a single action, so neither
 * row could be deleted. */
function rulesEditAddStepAction(i) {
    const steps = rulesEditSteps(rulesEditCollectThen());
    const step = steps[i];
    if (!step) return;
    if (rulesEditRealActionCount(step) >= MAX_ACTIONS_PER_STEP_WEB) {
        toast(t('rules.err_action_max', { n: MAX_ACTIONS_PER_STEP_WEB }));
        return;
    }
    const first = (rulesDeviceInstances()[0] || {}).point_id || '';
    /* Insert before a trailing wait so the card reads in execution order. */
    const tail = step.actions[step.actions.length - 1];
    const at = (tail && tail.type === 'delay') ? step.actions.length - 1 : step.actions.length;
    step.actions.splice(at, 0, { type: 'command', point_id: first, cmd: 'turn_on' });
    rulesEditRerenderThenList(steps);
}

function rulesEditRemoveStepAction(i, j) {
    const steps = rulesEditSteps(rulesEditCollectThen());
    const step = steps[i];
    if (!step) return;
    step.actions.splice(j, 1);
    rulesEditRerenderThenList(steps);
}

function rulesEditIfRows(iff) {
    const arr = Array.isArray(iff) ? iff : [];
    if (!arr.length) return '<div class="rules-ai-hint" id="re-if-empty">' + t('rules.no_extra') + '</div><div id="re-if-list"></div>';
    let rows = '';
    arr.forEach(function(c, i) {
        const cond = {
            point_id: (c && c.point_id) || '',
            parameter: (c && c.parameter) || '',
            op: rulesCondOp(c),
            value: rulesCondValue(c)
        };
        if (!cond.parameter && cond.point_id) {
            const names = rulesVisibleParams(cond.point_id);
            cond.parameter = names[0] || '';
        }
        rows += '<div class="rule-if-row" data-if-i="' + i + '">' +
            rulesPointCondFields('re-if-' + i, cond,
                'rulesEditSyncIfDevice(' + i + ')', 'rulesEditSyncIfParam(' + i + ')') +
            '<button class="btn-sm bad" type="button" onclick="rulesEditRemoveIf(' + i + ')">' + t('rules.delete') + '</button></div>';
    });
    return '<div id="re-if-list">' + rows + '</div>';
}

function rulesEditRender(rule) {
    const edit = document.getElementById('rules-edit');
    const list = document.getElementById('rules-list');
    const detail = document.getElementById('rules-detail');
    if (!edit) return;
    if (list) list.hidden = true;
    if (detail) detail.hidden = true;
    edit.hidden = false;
    const isNew = rulesEditMode === 'new';
    const first = rulesDeviceInstances()[0];
    const defaultPid = (first && first.point_id) || ((pts[0] && pts[0].point_id) || '');
    const defaultParams = rulesVisibleParams(defaultPid);
    const r = rule || {
        id: '',
        name: '',
        enabled: true,
        when: {
            type: 'point',
            point_id: defaultPid,
            parameter: defaultParams[0] || 'power',
            op: 'eq',
            value: 'on'
        },
        if: [],
        then: [{
            actions: [{ type: 'command', point_id: defaultPid, cmd: 'turn_on' }],
            delay_after: 0
        }]
    };

    edit.innerHTML = '<div class="rule-ed">' +
        '<button class="btn-sm" type="button" onclick="rulesEditCancel()">' + t('rules.back_cancel') + '</button>' +
        '<h3 style="margin-top:12px">' + (isNew ? t('rules.add_rule') : t('rules.edit_rule')) + '</h3>' +
        '<input type="hidden" id="re-id" value="' + escAttr(r.id || '') + '" />' +
        '<label>' + t('rules.field_name') + '</label><input type="text" id="re-name" value="' + escAttr(r.name || '') + '" placeholder="' + t('rules.name_ph') + '" />' +
        '<label class="rule-ed-check"><input type="checkbox" id="re-enabled"' + (r.enabled !== false ? ' checked' : '') + ' /> ' + t('rules.enabled') + '</label>' +
        '<div class="rule-ed-sec"><h4>' + t('rules.section_when') + '</h4><div id="re-when-box">' + rulesEditWhenFields(r.when) + '</div></div>' +
        '<div class="rule-ed-sec"><h4>' + t('rules.section_if') + '</h4>' + rulesEditIfRows(r.if) +
        '<button class="btn-sm" type="button" style="margin-top:8px" onclick="rulesEditAddIf()">' + t('rules.add_condition') + '</button></div>' +
        '<div class="rule-ed-sec"><h4>' + t('rules.section_then') + '</h4><div id="re-then-box">' + rulesEditThenFields(r.then) + '</div></div>' +
        '<div class="rule-ed-actions">' +
        '<button class="btn-sm on" type="button" onclick="rulesEditSave()">' + t('rules.save_apply') + '</button>' +
        '<button class="btn-sm" type="button" onclick="rulesEditCancel()">' + t('rules.cancel') + '</button>' +
        (isNew ? '' : '<button class="btn-sm bad" type="button" onclick="rulesEditDelete()">' + t('rules.delete_rule') + '</button>') +
        '</div><p class="rules-ai-hint">' + t('rules.save_hint') + '</p></div>';
}

function rulesEditCollectIf() {
    const box = document.getElementById('re-if-list');
    if (!box) return [];
    const rows = box.querySelectorAll('[data-if-i]');
    const out = [];
    rows.forEach(function(row) {
        const i = row.getAttribute('data-if-i');
        const c = rulesCollectPointCond('re-if-' + i);
        if (c.point_id && c.value !== '') out.push(c);
    });
    return out;
}

/* Saved payload: drop the placeholder waits kept for editing (a wait with 0
 * seconds is not a real step separator) and drop steps that ended up empty. */
function rulesThenForSave(steps) {
    const out = [];
    (steps || []).forEach(function (s) {
        const actions = (s && Array.isArray(s.actions)) ? s.actions.filter(function (a) {
            return !(a && a.type === 'delay');
        }) : [];
        const delay = (s && s.delay_after > 0) ? s.delay_after : 0;
        if (actions.length === 0 && delay === 0) return;
        const entry = { actions: actions };
        if (delay > 0) entry.delay_after = delay;
        out.push(entry);
    });
    return out;
}

function rulesEditSnapshot() {
    const nameEl = document.getElementById('re-name');
    const idEl = document.getElementById('re-id');
    const enEl = document.getElementById('re-enabled');
    const name = ((nameEl && nameEl.value) || '').trim();
    let id = ((idEl && idEl.value) || '').trim();
    const enabled = !!(enEl && enEl.checked);
    const wtypeEl = document.getElementById('re-when-type');
    const wtype = (wtypeEl && wtypeEl.value) || 'point';
    let when;
    if (wtype === 'time') {
        const atEl = document.getElementById('re-when-at');
        let at = ((atEl && atEl.value) || '23:00').trim();
        if (at.length === 4 && at.indexOf(':') === 1) at = '0' + at;
        when = { type: 'time', at: at };
    } else if (wtype === 'delay') {
        const dEl = document.getElementById('re-when-delay-sec');
        let d = parseInt((dEl && dEl.value) || '0', 10);
        if (isNaN(d) || d < 0) d = 0;
        when = { type: 'delay', delay_sec: d };
    } else {
        when = Object.assign({ type: 'point' }, rulesCollectPointCond('re-when'));
    }
    const then = rulesThenForSave(rulesEditCollectThen());
    return {
        id: id,
        name: name,
        enabled: enabled,
        when: when,
        if: rulesEditCollectIf(),
        then: then
    };
}

/* True when at least one real action remains. Checked on the raw payload (the
 * snapshot already dropped empty steps and wait rows) because rulesThenList()
 * injects a placeholder action for an empty list, which would otherwise be
 * reported as a half-filled action instead of “nothing to do”. */
function rulesEditHasAction(then) {
    const list = Array.isArray(then) ? then : (then ? [then] : []);
    const isSteps = list.some(function (s) {
        return s && s.actions;
    });
    if (!isSteps) {
        return list.some(function (a) {
            return a && a.type && a.type !== 'delay';
        });
    }
    return list.some(function (s) {
        return s && Array.isArray(s.actions) && s.actions.length > 0;
    });
}

function rulesEditValidate(r) {
    if (!r.name) return t('rules.err_name');
    if (r.when.type === 'point') {
        if (!r.when.point_id) return t('rules.err_trigger_device');
        if (!r.when.parameter) return t('rules.err_trigger_param');
        if (r.when.value == null || r.when.value === '') return t('rules.err_trigger_value');
    } else if (r.when.type === 'time') {
        if (!/^\d{2}:\d{2}$/.test(r.when.at || '')) return t('rules.err_time_hhmm');
    } else if (r.when.type === 'delay') {
        if (!(r.when.delay_sec > 0)) return t('rules.err_trigger_delay');
    } else return t('rules.err_trigger_type');
    const steps = rulesThenList(r.then);
    /* Any row can be deleted, so a rule could end up with nothing to do. */
    if (!rulesEditHasAction(r.then)) return t('rules.err_action_none');
    for (let s = 0; s < steps.length; s++) {
        const step = steps[s];
        const actions = (step && Array.isArray(step.actions)) ? step.actions : [];
        for (let k = 0; k < actions.length; k++) {
            const th = actions[k];
            if (th.type === 'command') {
                if (!th.point_id || !th.cmd) return t('rules.err_action_fields');
            } else if (th.type === 'command_by_type') {
                if (!th.cmd) return t('rules.err_action_fields');
            } else if (th.type === 'notify') {
                /* message is optional */
            } else if (th.type === 'delay') {
                /* wait inside a step: kept while editing, stripped before saving */
            } else return t('rules.err_action_type');
        }
    }
    return null;
}

function rulesEditSyncWhenType() {
    const box = document.getElementById('re-when-box');
    if (!box) return;
    const typ = document.getElementById('re-when-type').value;
    if (typ === 'delay') {
        box.innerHTML = rulesEditWhenFields({ type: 'delay', delay_sec: 0 });
    } else if (typ === 'time') {
        box.innerHTML = rulesEditWhenFields({
            type: 'time',
            at: '23:00'
        });
    } else {
        const first = rulesDeviceInstances()[0];
        const pid = (first && first.point_id) || '';
        const names = rulesVisibleParams(pid);
        box.innerHTML = rulesEditWhenFields({
            type: 'point',
            point_id: pid,
            parameter: names[0] || 'power',
            op: 'eq',
            value: 'on'
        });
    }
}

function rulesEditRebuildPointBox(prefix, onDev, onParam) {
    const cur = rulesCollectPointCond(prefix);
    const names = rulesVisibleParams(cur.point_id);
    if (!cur.parameter || names.indexOf(cur.parameter) < 0) {
        cur.parameter = names[0] || '';
    }
    const meta = cur.parameter ? rulesParamMeta(cur.point_id, cur.parameter) : null;
    if (meta && (meta.widget === 'toggle' || cur.parameter === 'power') &&
        !(cur.value === 'on' || cur.value === 'off')) {
        cur.value = 'on';
    } else if (meta && meta.min != null && (cur.value === '' || cur.value === 'on' || cur.value === 'off')) {
        cur.value = String(meta.min);
    }
    return rulesPointCondFields(prefix, cur, onDev, onParam);
}

function rulesEditSyncWhenDevice() {
    const box = document.getElementById('re-when-point');
    if (!box) return;
    box.innerHTML = rulesEditRebuildPointBox('re-when', 'rulesEditSyncWhenDevice()', 'rulesEditSyncWhenParam()');
}

function rulesEditSyncWhenParam() {
    rulesEditSyncWhenDevice();
}

function rulesEditSyncIfDevice(i) {
    const row = document.querySelector('.rule-if-row[data-if-i="' + i + '"]');
    if (!row) return;
    const btn = '<button class="btn-sm bad" type="button" onclick="rulesEditRemoveIf(' + i + ')">' + t('rules.delete') + '</button>';
    row.innerHTML = rulesEditRebuildPointBox('re-if-' + i,
        'rulesEditSyncIfDevice(' + i + ')', 'rulesEditSyncIfParam(' + i + ')') + btn;
}

function rulesEditSyncIfParam(i) {
    rulesEditSyncIfDevice(i);
}

function rulesEditAddIf() {
    const list = document.getElementById('re-if-list');
    const empty = document.getElementById('re-if-empty');
    if (empty) empty.hidden = true;
    if (!list) return;
    const i = list.querySelectorAll('[data-if-i]').length;
    const first = rulesDeviceInstances()[0];
    const pid = (first && first.point_id) || '';
    const names = rulesVisibleParams(pid);
    const wrap = document.createElement('div');
    wrap.className = 'rule-if-row';
    wrap.setAttribute('data-if-i', String(i));
    wrap.innerHTML = rulesPointCondFields('re-if-' + i, {
            point_id: pid,
            parameter: names[0] || 'power',
            op: 'eq',
            value: 'on'
        }, 'rulesEditSyncIfDevice(' + i + ')', 'rulesEditSyncIfParam(' + i + ')') +
        '<button class="btn-sm bad" type="button" onclick="rulesEditRemoveIf(' + i + ')">' + t('rules.delete') + '</button>';
    list.appendChild(wrap);
}

function rulesEditRemoveIf(i) {
    const cur = rulesEditSnapshot();
    const list = document.getElementById('re-if-list');
    if (!list) return;
    const rows = [].slice.call(list.querySelectorAll('[data-if-i]'));
    const kept = [];
    rows.forEach(function(row) {
        const idx = row.getAttribute('data-if-i');
        if (String(idx) === String(i)) return;
        kept.push(rulesCollectPointCond('re-if-' + idx));
    });
    const nameEl = document.getElementById('re-name');
    const idEl = document.getElementById('re-id');
    const enEl = document.getElementById('re-enabled');
    const base = {
        id: idEl && idEl.value,
        name: nameEl && nameEl.value,
        enabled: enEl && enEl.checked,
        when: cur.when,
        if: kept,
        then: cur.then
    };
    rulesEditRender(base);
}
async function rulesSaveAll(nextRules) {
    const r = await fetch('/api/rules/import', {
        method: 'POST',
        headers: {
            'Content-Type': 'application/json'
        },
        body: JSON.stringify({
            rules: nextRules
        })
    });
    const j = await r.json().catch(function() {
        return {};
    });
    if (!r.ok) throw new Error(j.message || String(r.status));
    return j;
}

/* Flip a single rule's enabled flag and re-import the whole table (same hot
 * reload as the editor). Optimistic local update keeps the UI instant. */
async function rulesToggleEnabled(id) {
    const rules = (rulesCfg.rules || []).map(function (r) {
        if (r.id === id) {
            return Object.assign({}, r, { enabled: r.enabled === false ? true : false });
        }
        return r;
    });
    try {
        await rulesSaveAll(rules);
    } catch (e) {
        toast(t('rules.save_failed', { err: (e && e.message) ? e.message : '' }));
        return;
    }
    rulesCfg.rules = rules;
    toast(t('rules.saved'));
    if (typeof renderRules === 'function') renderRules();
}

function rulesNextId() {
    const used = {};
    (rulesCfg.rules || []).forEach(function(r) {
        if (r && r.id) used[r.id] = 1;
    });
    let i = 1;
    while (true) {
        const id = 'rule_' + String(i).padStart(3, '0');
        if (!used[id]) return id;
        i++;
    }
}
async function rulesEditSave() {
    const snap = rulesEditSnapshot();
    const err = rulesEditValidate(snap);
    if (err) {
        toast(err);
        return;
    }
    if (rulesEditMode === 'new') {
        if (!snap.id) snap.id = rulesNextId();
        else if ((rulesCfg.rules || []).some(function(r) {
                return r.id === snap.id;
            })) {
            toast(t('rules.err_id_exists'));
            return;
        }
    } else {
        snap.id = rulesEditMode;
    }
    let next;
    if (rulesEditMode === 'new') next = (rulesCfg.rules || []).concat([snap]);
    else next = (rulesCfg.rules || []).map(function(r) {
        return r.id === snap.id ? snap : r;
    });
    try {
        await rulesSaveAll(next);
        toast(t('rules.saved'));
        rulesEditMode = null;
        rulesSel = snap.id;
        await loadRules();
    } catch (e) {
        toast(t('rules.save_failed', { err: (e.message || e) }));
    }
}
async function rulesEditDelete() {
    if (rulesEditMode === 'new' || !rulesEditMode) return;
    if (!confirm(t('rules.confirm_delete', { id: rulesEditMode }))) return;
    const next = (rulesCfg.rules || []).filter(function(r) {
        return r.id !== rulesEditMode;
    });
    try {
        await rulesSaveAll(next);
        toast(t('rules.deleted'));
        rulesEditMode = null;
        rulesSel = null;
        await loadRules();
    } catch (e) {
        toast(t('rules.delete_failed', { err: (e.message || e) }));
    }
}

/* Delete a rule straight from its list card (the editor has its own path). */
async function rulesDelete(id) {
    if (!id) return;
    if (!confirm(t('rules.confirm_delete', { id: id }))) return;
    const next = (rulesCfg.rules || []).filter(function(r) {
        return r.id !== id;
    });
    try {
        await rulesSaveAll(next);
        toast(t('rules.deleted'));
        if (rulesSel === id) rulesSel = null;
        await loadRules();
    } catch (e) {
        toast(t('rules.delete_failed', { err: (e.message || e) }));
    }
}

async function importRules() {
    const u = (document.getElementById('rules-import-url').value || '').trim();
    if (!u) {
        toast(t('rules.err_url'));
        return;
    }
    const btn = document.getElementById('rules-import-btn');
    if (btn) btn.disabled = true;
    try {
        const r = await fetch('/api/rules/import', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify({
                url: u
            })
        });
        const j = await r.json().catch(() => ({}));
        if (!r.ok) {
            toast(t('rules.import_failed_detail', { err: (j.message || r.status) }));
            return;
        }
        toast(t('rules.imported', { n: ((j.count != null) ? j.count : '') }));
        rulesSel = null;
        await loadRules();
    } catch (e) {
        toast(t('rules.import_failed'));
    } finally {
        if (btn) btn.disabled = false;
    }
}

function exportRules() {
    const text = JSON.stringify(rulesCfg, null, 2);
    if (navigator.clipboard && navigator.clipboard.writeText) {
        navigator.clipboard.writeText(text).then(() => toast(t('rules.copied'))).catch(() => downloadRules(text));
    } else downloadRules(text);
}

function downloadRules(text) {
    const a = document.createElement('a');
    a.href = URL.createObjectURL(new Blob([text], {
        type: 'application/json'
    }));
    a.download = 'rules.json';
    a.click();
    URL.revokeObjectURL(a.href);
    toast(t('rules.downloaded'));
}
