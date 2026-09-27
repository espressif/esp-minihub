/* MiniHub web — rules page */
let rulesCfg = {
    rules: []
};
let rulesSel = null;
let rulesEditMode = null; /* null|new|id */


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
        error: '缺少 rules 数组'
    };
    for (let i = 0; i < file.rules.length; i++) {
        const r = file.rules[i];
        if (!r || typeof r !== 'object') return {
            ok: false,
            error: '规则 ' + (i + 1) + ' 无效'
        };
        if (!r.id || !r.name || !r.when || !r.then) return {
            ok: false,
            error: '规则 ' + (i + 1) + ' 缺 id/name/when/then'
        };
        const w = r.when;
        if (w.type === 'point') {
            const hasVal = (w.value != null && w.value !== '') || (w.equals != null && w.equals !== '');
            if (!w.point_id || !hasVal) return {
                ok: false,
                error: '规则 ' + (i + 1) + ' when.point 不完整'
            };
        } else if (w.type === 'time') {
            if (!/^\d{2}:\d{2}$/.test(w.at || '')) return {
                ok: false,
                error: '规则 ' + (i + 1) + ' when.at 须 HH:MM'
            };
        } else return {
            ok: false,
            error: '规则 ' + (i + 1) + ' when.type 无效'
        };
        const th = r.then;
        if (th.type === 'command') {
            if (!th.point_id || !th.cmd) return {
                ok: false,
                error: '规则 ' + (i + 1) + ' then.command 不完整'
            };
        } else if (th.type === 'delay') {
            if (!(th.seconds > 0)) return {
                ok: false,
                error: '规则 ' + (i + 1) + ' then.delay.seconds 无效'
            };
        } else return {
            ok: false,
            error: '规则 ' + (i + 1) + ' then.type 无效'
        };
        if (r.if != null && !Array.isArray(r.if)) return {
            ok: false,
            error: '规则 ' + (i + 1) + ' if 须为数组'
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
            '<div class="card-top"><div class="card-name">' + esc(rule.name || rule.id || ('草稿 ' + (idx + 1))) + '</div>' +
            (rule.enabled === false ? '<span class="badge">禁用</span>' : '<span class="badge sensor">启用</span>') + '</div>' +
            '<div class="card-meta"><span>' + esc(rule.id || '') + '</span></div>' +
            '<div class="rule-flow">' + ruleStepHtml('当 · 触发', w) + '<div class="rule-arrow">↓</div>' + ruleStepHtml('且 · 条件', i) + '<div class="rule-arrow">↓</div>' + ruleStepHtml('则 · 动作', th) + '</div></div>';
    });
    html += '<details class="rule-tech"><summary>技术详情（JSON）</summary><pre class="rule-pre">' + esc(JSON.stringify(file, null, 2)) + '</pre></details>';
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
        toast('请先填写规则描述');
        return;
    }
    rulesAiLoadCfg();
    const base = ((document.getElementById('rules-ai-base').value || '').trim() || 'https://api.openai.com/v1').replace(/\/$/, '');
    const model = (document.getElementById('rules-ai-model').value || '').trim() || 'gpt-4o-mini';
    const key = (document.getElementById('rules-ai-key').value || '').trim();
    if (!key) {
        toast('请先在 API 设置中填写 API Key');
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
    toast('正在生成…');
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
            toast('云端失败: ' + msg);
            return;
        }
        const content = (j.choices && j.choices[0] && j.choices[0].message && j.choices[0].message.content) || '';
        const parsed = rulesAiExtractJson(content);
        if (!parsed) {
            toast('模型未返回合法 JSON');
            return;
        }
        const v = rulesAiValidate(parsed);
        if (!v.ok) {
            toast('校验失败: ' + v.error);
            return;
        }
        parsed.rules.forEach(function(rule) {
            if (typeof rule.enabled !== 'boolean') rule.enabled = true;
            if (!Array.isArray(rule.if)) rule.if = [];
        });
        rulesAiDraft = parsed;
        rulesAiRenderDraft(parsed);
        toast('已生成 ' + parsed.rules.length + ' 条，请确认');
    } catch (e) {
        toast('生成失败（Hub 须能访问云端 API）');
    } finally {
        if (btn) btn.disabled = false;
    }
}
async function rulesAiApply() {
    if (!rulesAiDraft || !Array.isArray(rulesAiDraft.rules) || !rulesAiDraft.rules.length) {
        toast('没有可生效的草稿');
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
            toast('生效失败: ' + (j.message || r.status));
            return;
        }
        toast('已生效 ' + (j.count != null ? j.count : nextRules.length) + ' 条');
        rulesAiCancel();
        rulesSel = null;
        await loadRules();
    } catch (e) {
        toast('生效失败');
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
    if (w.type === 'time') return '定时 ' + (w.at || '');
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

function thenSummary(t) {
    if (!t || typeof t !== 'object') return '-';
    if (t.type === 'delay') {
        const n = t.ms != null ? t.ms : (t.seconds != null ? t.seconds * 1000 : '');
        return '延时 ' + n + ' ms';
    }
    if (t.type === 'command') return (t.cmd || '') + ' → ' + rulesDeviceLabel(t.point_id || '');
    return t.type || '-';
}

function whenPlain(w) {
    if (!w || typeof w !== 'object') return {
        title: '未知触发',
        body: '未配置触发条件'
    };
    if (w.type === 'time') return {
        title: '定时到达',
        body: '每天 <b>' + esc(w.at || '??:??') + '</b> 触发'
    };
    if (w.type === 'point') {
        const dev = rulesDeviceLabel(w.point_id);
        const param = w.parameter ? String(w.parameter) : '';
        const op = rulesOpLabel(rulesCondOp(w));
        const val = rulesCondValue(w);
        const target = param ? (esc(dev) + ' · <b>' + esc(param) + '</b>') : ('<b>' + esc(dev) + '</b>');
        return {
            title: '设备状态变化',
            body: '当 ' + target + ' ' + esc(op) + ' <b>' + esc(val || '?') + '</b> 时'
        };
    }
    return {
        title: esc(w.type || '触发'),
        body: esc(JSON.stringify(w))
    };
}

function ifPlain(iff) {
    const arr = Array.isArray(iff) ? iff : [];
    if (!arr.length) return {
        title: '无额外条件',
        body: '触发后直接执行动作'
    };
    const lines = arr.map(function(c, i) {
        if (!c || typeof c !== 'object') return (i + 1) + '. （无效条件）';
        const dev = rulesDeviceLabel(c.point_id);
        const param = c.parameter ? String(c.parameter) : '';
        const op = rulesOpLabel(rulesCondOp(c));
        const val = rulesCondValue(c);
        const target = param ? (esc(dev) + ' · <b>' + esc(param) + '</b>') : ('<b>' + esc(dev) + '</b>');
        return (i + 1) + '. ' + target + ' ' + esc(op) + ' <b>' + esc(val || '?') + '</b>';
    }).join('<br>');
    return {
        title: '还需同时满足',
        body: lines
    };
}

function thenPlain(th) {
    if (!th || typeof th !== 'object') return {
        title: '未知动作',
        body: '未配置动作'
    };
    if (th.type === 'delay') {
        const n = th.ms != null ? th.ms : (th.seconds != null ? th.seconds * 1000 : null);
        return {
            title: '延时等待',
            body: '等待 <b>' + esc(n != null ? String(n) : '?') + '</b> 毫秒（阻塞，同条规则无后续动作）'
        };
    }
    if (th.type === 'command') return {
        title: '下发指令',
        body: '对 <b>' + esc(rulesDeviceLabel(th.point_id || '?')) + '</b> 执行 <b>' + esc(th.cmd || '?') + '</b>'
    };
    return {
        title: esc(th.type || '动作'),
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
        toast('规则加载失败');
        const el = document.getElementById('rules-list');
        if (el) el.innerHTML = '<div class="empty">加载失败</div>';
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

/* Type catalog from GET /api/device_widgets — labels / widget / bounds / options. */
let dwRuleCatalog = null;

async function dwRuleLoadCatalog() {
    try {
        const r = await fetch('/api/device_widgets');
        if (r.ok) dwRuleCatalog = await r.json();
    } catch (e) { /* non-fatal */ }
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

/** One card = one device point (props.params or 2-seg id). Prefer dwBuildInstances. */
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

/** Visible params = live props.params ∩ catalog (catalog optional). */
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
        const label = rulesDeviceLabel(id) + (pt && pt.type ? '（' + pt.type + '）' : '');
        return '<option value="' + escAttr(id) + '"' + (id === selected ? ' selected' : '') + '>' +
            esc(label || '(无设备)') + '</option>';
    }).join('');
}

function rulesParamOptionsHtml(pointId, selected) {
    const names = rulesVisibleParams(pointId);
    if (selected && names.indexOf(selected) < 0) names.unshift(selected);
    if (!names.length) {
        return '<option value="">（无可用参数）</option>';
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
        return '<input type="text" id="' + id + '" value="' + escAttr(v) + '" placeholder="值" />';
    }
    const meta = rulesParamMeta(pointId, parameter);
    const w = meta.widget || '';
    if (w === 'toggle' || w === 'bool' || parameter === 'power') {
        const on = (v === 'on' || v === 'true' || v === '1' || v === 'ON');
        const off = (v === 'off' || v === 'false' || v === '0' || v === 'OFF');
        return '<select id="' + id + '">' +
            '<option value="on"' + (on || (!on && !off && !v) ? ' selected' : '') + '>开 (on)</option>' +
            '<option value="off"' + (off ? ' selected' : '') + '>关 (off)</option></select>';
    }
    if ((w === 'dropdown' || w === 'segmented') && meta.options && meta.options.length) {
        let body = '';
        meta.options.forEach(function(o) {
            const val = (o && typeof o === 'object') ? o.value : o;
            const lbl = (o && typeof o === 'object') ? o.label : o;
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
    return '<input type="text" id="' + id + '" value="' + escAttr(v) + '" placeholder="值" />';
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
        '<div><label>设备</label>' + rulesSelectHtml(prefix + '-dev', rulesDeviceOptionsHtml(pointId), onDevChange) + '</div>' +
        '<div><label>参数</label>' + rulesSelectHtml(prefix + '-param', rulesParamOptionsHtml(pointId, parameter), onParamChange) + '</div>' +
        '</div>' +
        '<div class="row2 rule-ed-opval">' +
        '<div><label>条件</label>' + rulesSelectHtml(prefix + '-op', rulesOpOptionsHtml(op)) + '</div>' +
        '<div><label>值</label><div class="rule-ed-val">' + rulesValueInputHtml(prefix + '-val', pointId, parameter, value) + '</div></div>' +
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
    /* Keep legacy equals mirror for older tooling / display. */
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
        toast('规则不存在');
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
    let html = '<label>触发类型</label><select id="re-when-type" onchange="rulesEditSyncWhenType()">' +
        '<option value="point"' + (typ === 'point' ? ' selected' : '') + '>设备状态变化</option>' +
        '<option value="time"' + (typ === 'time' ? ' selected' : '') + '>定时（HH:MM）</option></select>';
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
        html += '<label>时间 at</label><input type="time" id="re-when-at" value="' + escAttr(w.at || '23:00') + '" />';
    }
    return html;
}

function rulesEditThenFields(th) {
    const t = th && typeof th === 'object' ? th : {
        type: 'command'
    };
    const typ = t.type === 'delay' ? 'delay' : 'command';
    let html = '<label>动作类型</label><select id="re-then-type" onchange="rulesEditSyncThenType()">' +
        '<option value="command"' + (typ === 'command' ? ' selected' : '') + '>下发指令（command）</option>' +
        '<option value="delay"' + (typ === 'delay' ? ' selected' : '') + '>延时（delay，无后续动作）</option></select>';
    if (typ === 'command') {
        const cmd = t.cmd || 'turn_on';
        const cmdOpts = RULE_CMDS.map(function(c) {
            return '<option value="' + c + '"' + (c === cmd ? ' selected' : '') + '>' + c + '</option>';
        }).join('');
        const extra = (RULE_CMDS.indexOf(cmd) < 0) ? '<option value="' + escAttr(cmd) + '" selected>' + esc(cmd) + '</option>' : '';
        html += '<div class="row2"><div><label>目标设备</label>' +
            rulesSelectHtml('re-then-pid', rulesDeviceOptionsHtml(t.point_id || '')) + '</div>' +
            '<div><label>指令</label><select id="re-then-cmd">' + extra + cmdOpts + '</select></div></div>';
    } else {
        html += '<label>秒数 seconds</label><input type="number" id="re-then-sec" min="1" value="' + (t.seconds > 0 ? t.seconds : 5) + '" />';
    }
    return html;
}

function rulesEditIfRows(iff) {
    const arr = Array.isArray(iff) ? iff : [];
    if (!arr.length) return '<div class="rules-ai-hint" id="re-if-empty">无附加条件（触发后直接执行）</div><div id="re-if-list"></div>';
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
            '<button class="btn-sm bad" type="button" onclick="rulesEditRemoveIf(' + i + ')">删除</button></div>';
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
    /* Hidden id keeps snapshot/save working without showing the field. */
    edit.innerHTML = '<div class="rule-ed">' +
        '<button class="btn-sm" type="button" onclick="rulesEditCancel()">← 取消</button>' +
        '<h3 style="margin-top:12px">' + (isNew ? '添加规则' : '编辑规则') + '</h3>' +
        '<input type="hidden" id="re-id" value="' + escAttr(r.id || '') + '" />' +
        '<label>名称</label><input type="text" id="re-name" value="' + escAttr(r.name || '') + '" placeholder="如 亮度过高提醒" />' +
        '<label class="rule-ed-check"><input type="checkbox" id="re-enabled"' + (r.enabled !== false ? ' checked' : '') + ' /> 启用</label>' +
        '<div class="rule-ed-sec"><h4>当 · When（触发）</h4><div id="re-when-box">' + rulesEditWhenFields(r.when) + '</div></div>' +
        '<div class="rule-ed-sec"><h4>且 · And If（条件，可选）</h4>' + rulesEditIfRows(r.if) +
        '<button class="btn-sm" type="button" style="margin-top:8px" onclick="rulesEditAddIf()">+ 添加条件</button></div>' +
        '<div class="rule-ed-sec"><h4>则 · Then（动作）</h4><div id="re-then-box">' + rulesEditThenFields(r.then) + '</div></div>' +
        '<div class="rule-ed-actions">' +
        '<button class="btn-sm on" type="button" onclick="rulesEditSave()">保存并生效</button>' +
        '<button class="btn-sm" type="button" onclick="rulesEditCancel()">取消</button>' +
        (isNew ? '' : '<button class="btn-sm bad" type="button" onclick="rulesEditDelete()">删除规则</button>') +
        '</div><p class="rules-ai-hint">保存将整表写入 Hub（与导入相同热重载）。选择设备与参数；原始 point_id 仅内部存储。</p></div>';
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
    if (!r.name) return '请填写名称';
    if (r.when.type === 'point') {
        if (!r.when.point_id) return '请选择触发设备';
        if (!r.when.parameter) return '请选择触发参数';
        if (r.when.value == null || r.when.value === '') return '请填写触发值';
    } else if (r.when.type === 'time') {
        if (!/^\d{2}:\d{2}$/.test(r.when.at || '')) return '定时须为 HH:MM';
    } else return '触发类型无效';
    if (r.then.type === 'command') {
        if (!r.then.point_id || !r.then.cmd) return '请完整填写动作设备与指令';
    } else if (r.then.type === 'delay') {
        if (!(r.then.seconds > 0)) return '延时秒数无效';
    } else return '动作类型无效';
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
    const btn = '<button class="btn-sm bad" type="button" onclick="rulesEditRemoveIf(' + i + ')">删除</button>';
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
        '<button class="btn-sm bad" type="button" onclick="rulesEditRemoveIf(' + i + ')">删除</button>';
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
            toast('ID 已存在');
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
        toast('已保存');
        rulesEditMode = null;
        rulesSel = snap.id;
        await loadRules();
    } catch (e) {
        toast('保存失败: ' + (e.message || e));
    }
}
async function rulesEditDelete() {
    if (rulesEditMode === 'new' || !rulesEditMode) return;
    if (!confirm('删除规则 ' + rulesEditMode + '？')) return;
    const next = (rulesCfg.rules || []).filter(function(r) {
        return r.id !== rulesEditMode;
    });
    try {
        await rulesSaveAll(next);
        toast('已删除');
        rulesEditMode = null;
        rulesSel = null;
        await loadRules();
    } catch (e) {
        toast('删除失败: ' + (e.message || e));
    }
}

async function importRules() {
    const u = (document.getElementById('rules-import-url').value || '').trim();
    if (!u) {
        toast('请填写 URL');
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
            toast('导入失败: ' + (j.message || r.status));
            return;
        }
        toast('已导入 ' + ((j.count != null) ? j.count : '') + ' 条');
        rulesSel = null;
        await loadRules();
    } catch (e) {
        toast('导入失败');
    } finally {
        if (btn) btn.disabled = false;
    }
}

function exportRules() {
    const text = JSON.stringify(rulesCfg, null, 2);
    if (navigator.clipboard && navigator.clipboard.writeText) {
        navigator.clipboard.writeText(text).then(() => toast('已复制到剪贴板')).catch(() => downloadRules(text));
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
    toast('已下载 rules.json');
}
