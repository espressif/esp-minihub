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
        'then: {"type":"command","point_id":"...","cmd":"..."} OR {"type":"delay","seconds":N}. One then only.\n' +
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
        const th = r.then;
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

function thenSummary(th) {
    if (!th || typeof th !== 'object') return '-';
    if (th.type === 'delay') {
        const n = th.ms != null ? th.ms : (th.seconds != null ? th.seconds * 1000 : '');
        return t('rules.summary_delay', { n: n });
    }
    if (th.type === 'command') return (th.cmd || '') + ' → ' + rulesDeviceLabel(th.point_id || '');
    return th.type || '-';
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
    if (th.type === 'delay') {
        const n = th.ms != null ? th.ms : (th.seconds != null ? th.seconds * 1000 : null);
        return {
            title: t('rules.then_delay'),
            body: t('rules.then_delay_body', { n: esc(n != null ? String(n) : '?') })
        };
    }
    if (th.type === 'command') return {
        title: t('rules.send_command'),
        body: t('rules.then_command_body', { dev: esc(rulesDeviceLabel(th.point_id || '?')), cmd: esc(th.cmd || '?') })
    };
    return {
        title: esc(th.type || t('rules.action')),
        body: esc(JSON.stringify(th))
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

const RULE_CMDS = ['turn_on', 'turn_off', 'toggle', 'set', 'set_brightness'];
const RULE_OPS = [
    { op: 'lt', label: '<' },
    { op: 'le', label: '≦' },
    { op: 'eq', label: '=' },
    { op: 'gt', label: '>' },
    { op: 'ge', label: '≧' }
];

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
            if (p && p.point_id) ids.push(p.point_id);
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

function rulesOpOptionsHtml(selected) {
    const sel = selected || 'eq';
    return RULE_OPS.map(function(o) {
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
            '" value="' + escAttr(String(num)) + '" />' + unit;
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
    return '<div class="row2">' +
        '<div><label>' + t('rules.field_device') + '</label>' + rulesSelectHtml(prefix + '-dev', rulesDeviceOptionsHtml(pointId), onDevChange) + '</div>' +
        '<div><label>' + t('rules.field_parameter') + '</label>' + rulesSelectHtml(prefix + '-param', rulesParamOptionsHtml(pointId, parameter), onParamChange) + '</div>' +
        '</div>' +
        '<div class="row2 rule-ed-opval">' +
        '<div><label>' + t('rules.field_condition') + '</label>' + rulesSelectHtml(prefix + '-op', rulesOpOptionsHtml(op)) + '</div>' +
        '<div><label>' + t('rules.field_value') + '</label><div class="rule-ed-val">' + rulesValueInputHtml(prefix + '-val', pointId, parameter, value) + '</div></div>' +
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
    renderRules();
}

function rulesEditWhenFields(when) {
    const w = when && typeof when === 'object' ? when : {
        type: 'point'
    };
    const typ = w.type === 'time' ? 'time' : 'point';
    let html = '<label>' + t('rules.trigger_type') + '</label><select id="re-when-type" onchange="rulesEditSyncWhenType()">' +
        '<option value="point"' + (typ === 'point' ? ' selected' : '') + '>' + t('rules.device_state_change') + '</option>' +
        '<option value="time"' + (typ === 'time' ? ' selected' : '') + '>' + t('rules.opt_time_type') + '</option></select>';
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
    } else {
        html += '<label>' + t('rules.field_time_at') + '</label><input type="time" id="re-when-at" value="' + escAttr(w.at || '23:00') + '" />';
    }
    return html;
}

function rulesEditThenFields(th) {
    const act = th && typeof th === 'object' ? th : {
        type: 'command'
    };
    const typ = act.type === 'delay' ? 'delay' : 'command';
    let html = '<label>' + t('rules.action_type') + '</label><select id="re-then-type" onchange="rulesEditSyncThenType()">' +
        '<option value="command"' + (typ === 'command' ? ' selected' : '') + '>' + t('rules.opt_cmd_type') + '</option>' +
        '<option value="delay"' + (typ === 'delay' ? ' selected' : '') + '>' + t('rules.opt_delay_type') + '</option></select>';
    if (typ === 'command') {
        const cmd = act.cmd || 'turn_on';
        const cmdOpts = RULE_CMDS.map(function(c) {
            return '<option value="' + c + '"' + (c === cmd ? ' selected' : '') + '>' + c + '</option>';
        }).join('');
        const extra = (RULE_CMDS.indexOf(cmd) < 0) ? '<option value="' + escAttr(cmd) + '" selected>' + esc(cmd) + '</option>' : '';
        html += '<div class="row2"><div><label>' + t('rules.target_device') + '</label>' +
            rulesSelectHtml('re-then-pid', rulesDeviceOptionsHtml(act.point_id || '')) + '</div>' +
            '<div><label>' + t('rules.field_cmd') + '</label><select id="re-then-cmd">' + extra + cmdOpts + '</select></div></div>';
    } else {
        html += '<label>' + t('rules.field_seconds') + '</label><input type="number" id="re-then-sec" min="1" value="' + (act.seconds > 0 ? act.seconds : 5) + '" />';
    }
    return html;
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
        then: {
            type: 'command',
            point_id: defaultPid,
            cmd: 'turn_on'
        }
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
        when = {
            type: 'time',
            at: at
        };
    } else {
        when = Object.assign({
            type: 'point'
        }, rulesCollectPointCond('re-when'));
    }
    const ttypeEl = document.getElementById('re-then-type');
    const ttype = (ttypeEl && ttypeEl.value) || 'command';
    let then;
    if (ttype === 'delay') {
        const secEl = document.getElementById('re-then-sec');
        then = {
            type: 'delay',
            seconds: Math.max(1, parseInt(((secEl && secEl.value) || '5'), 10) || 5)
        };
    } else {
        const pidEl = document.getElementById('re-then-pid');
        const cmdEl = document.getElementById('re-then-cmd');
        then = {
            type: 'command',
            point_id: ((pidEl && pidEl.value) || '').trim(),
            cmd: ((cmdEl && cmdEl.value) || '').trim()
        };
    }
    return {
        id: id,
        name: name,
        enabled: enabled,
        when: when,
        if: rulesEditCollectIf(),
        then: then
    };
}

function rulesEditValidate(r) {
    if (!r.name) return t('rules.err_name');
    if (r.when.type === 'point') {
        if (!r.when.point_id) return t('rules.err_trigger_device');
        if (!r.when.parameter) return t('rules.err_trigger_param');
        if (r.when.value == null || r.when.value === '') return t('rules.err_trigger_value');
    } else if (r.when.type === 'time') {
        if (!/^\d{2}:\d{2}$/.test(r.when.at || '')) return t('rules.err_time_hhmm');
    } else return t('rules.err_trigger_type');
    if (r.then.type === 'command') {
        if (!r.then.point_id || !r.then.cmd) return t('rules.err_action_fields');
    } else if (r.then.type === 'delay') {
        if (!(r.then.seconds > 0)) return t('rules.err_delay_seconds');
    } else return t('rules.err_action_type');
    return null;
}

function rulesEditSyncWhenType() {
    const box = document.getElementById('re-when-box');
    if (!box) return;
    const typ = document.getElementById('re-when-type').value;
    if (typ === 'time') {
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

function rulesEditSyncThenType() {
    const typ = document.getElementById('re-then-type').value;
    const box = document.getElementById('re-then-box');
    if (!box) return;
    if (typ === 'delay') box.innerHTML = rulesEditThenFields({
        type: 'delay',
        seconds: 5
    });
    else {
        const first = rulesDeviceInstances()[0];
        box.innerHTML = rulesEditThenFields({
            type: 'command',
            point_id: (first && first.point_id) || '',
            cmd: 'turn_on'
        });
    }
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
