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
        'when: {"type":"point","point_id":"...","equals":"..."} OR {"type":"time","at":"HH:MM"}.\n' +
        'if item: {"point_id":"...","equals":"..."}. Empty if ok.\n' +
        'then: {"type":"command","point_id":"...","cmd":"..."} OR {"type":"delay","seconds":N}. One then only.\n' +
        'Prefer these point_id values when relevant: ' + JSON.stringify(pointIds.slice(0, 80)) + '\n' +
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
            if (!w.point_id || w.equals == null || w.equals === '') return {
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

function whenSummary(w) {
    if (!w || typeof w !== 'object') return '-';
    if (w.type === 'time') return '定时 ' + (w.at || '');
    if (w.type === 'point') return (w.point_id || '') + ' = ' + (w.equals != null ? w.equals : '');
    return w.type || '-';
}

function thenSummary(t) {
    if (!t || typeof t !== 'object') return '-';
    if (t.type === 'delay') {
        const n = t.ms != null ? t.ms : (t.seconds != null ? t.seconds * 1000 : '');
        return '延时 ' + n + ' ms';
    }
    if (t.type === 'command') return (t.cmd || '') + ' → ' + (t.point_id || '');
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
    if (w.type === 'point') return {
        title: '设备状态变化',
        body: '当 <b>' + esc(w.point_id || '?') + '</b> 变为 <b>' + esc(w.equals != null ? String(w.equals) : '?') + '</b> 时'
    };
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
        return (i + 1) + '. <b>' + esc(c.point_id || '?') + '</b> 等于 <b>' + esc(c.equals != null ? String(c.equals) : '?') + '</b>';
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
        body: '对 <b>' + esc(th.point_id || '?') + '</b> 执行 <b>' + esc(th.cmd || '?') + '</b>'
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
const RULE_EQ_HINTS = ['on', 'off', 'pressed', 'released', 'true', 'false', '1', '0'];

function rulesPointOptions(selected) {
    const ids = [];
    (pts || []).forEach(function(p) {
        if (p && p.point_id) ids.push(p.point_id);
    });
    if (selected && ids.indexOf(selected) < 0) ids.unshift(selected);
    if (!ids.length) ids.push('');
    return ids.map(function(id) {
        return '<option value="' + escAttr(id) + '"' + (id === selected ? ' selected' : '') + '>' + esc(id || '(无能力点)') + '</option>';
    }).join('');
}

function rulesSelectHtml(id, optsHtml) {
    return '<select id="' + id + '">' + optsHtml + '</select>';
}

function rulesEqInput(id, val) {
    const v = val != null ? String(val) : '';
    return '<input type="text" id="' + id + '" list="rules-eq-hints" value="' + escAttr(v) + '" placeholder="如 on / pressed" />';
}

function rulesEnsureEqDatalist() {
    if (document.getElementById('rules-eq-hints')) return;
    const d = document.createElement('datalist');
    d.id = 'rules-eq-hints';
    d.innerHTML = RULE_EQ_HINTS.map(function(x) {
        return '<option value="' + x + '"></option>';
    }).join('');
    document.body.appendChild(d);
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
        '<option value="point"' + (typ === 'point' ? ' selected' : '') + '>能力点变化</option>' +
        '<option value="time"' + (typ === 'time' ? ' selected' : '') + '>定时（HH:MM）</option></select>';
    if (typ === 'point') {
        html += '<div class="row2"><div><label>能力点 point_id</label>' + rulesSelectHtml('re-when-pid', rulesPointOptions(w.point_id || '')) + '</div>' +
            '<div><label>变为 equals</label>' + rulesEqInput('re-when-eq', w.equals != null ? w.equals : 'pressed') + '</div></div>';
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
        html += '<div class="row2"><div><label>目标 point_id</label>' + rulesSelectHtml('re-then-pid', rulesPointOptions(t.point_id || '')) + '</div>' +
            '<div><label>指令 cmd</label><select id="re-then-cmd">' + extra + cmdOpts + '</select></div></div>';
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
        rows += '<div class="row3 rule-if-row" data-if-i="' + i + '">' +
            '<div><label>point_id</label>' + rulesSelectHtml('re-if-pid-' + i, rulesPointOptions(c && c.point_id || '')) + '</div>' +
            '<div><label>equals</label>' + rulesEqInput('re-if-eq-' + i, c && c.equals != null ? c.equals : 'on') + '</div>' +
            '<button class="btn-sm bad" type="button" onclick="rulesEditRemoveIf(' + i + ')">删除</button></div>';
    });
    return '<div id="re-if-list">' + rows + '</div>';
}

function rulesEditRender(rule) {
    rulesEnsureEqDatalist();
    const edit = document.getElementById('rules-edit');
    const list = document.getElementById('rules-list');
    const detail = document.getElementById('rules-detail');
    if (!edit) return;
    if (list) list.hidden = true;
    if (detail) detail.hidden = true;
    edit.hidden = false;
    const isNew = rulesEditMode === 'new';
    const r = rule || {
        id: '',
        name: '',
        enabled: true,
        when: {
            type: 'point',
            point_id: (pts[0] && pts[0].point_id) || '',
            equals: 'pressed'
        },
        if: [],
        then: {
            type: 'command',
            point_id: (pts[0] && pts[0].point_id) || '',
            cmd: 'turn_on'
        }
    };
    edit.innerHTML = '<div class="rule-ed">' +
        '<button class="btn-sm" type="button" onclick="rulesEditCancel()">← 取消</button>' +
        '<h3 style="margin-top:12px">' + (isNew ? '添加规则' : '编辑规则') + '</h3>' +
        '<div class="row2"><div><label>名称 name</label><input type="text" id="re-name" value="' + escAttr(r.name || '') + '" placeholder="如 按钮开灯" /></div>' +
        '<div><label>ID</label><input type="text" id="re-id" value="' + escAttr(r.id || '') + '" placeholder="自动生成" ' + (isNew ? '' : 'readonly') + ' /></div></div>' +
        '<label class="rule-ed-check"><input type="checkbox" id="re-enabled"' + (r.enabled !== false ? ' checked' : '') + ' /> 启用</label>' +
        '<div class="rule-ed-sec"><h4>当 · When（触发）</h4><div id="re-when-box">' + rulesEditWhenFields(r.when) + '</div></div>' +
        '<div class="rule-ed-sec"><h4>且 · And If（条件，可选）</h4>' + rulesEditIfRows(r.if) +
        '<button class="btn-sm" type="button" style="margin-top:8px" onclick="rulesEditAddIf()">+ 添加条件</button></div>' +
        '<div class="rule-ed-sec"><h4>则 · Then（动作）</h4><div id="re-then-box">' + rulesEditThenFields(r.then) + '</div></div>' +
        '<div class="rule-ed-actions">' +
        '<button class="btn-sm on" type="button" onclick="rulesEditSave()">保存并生效</button>' +
        '<button class="btn-sm" type="button" onclick="rulesEditCancel()">取消</button>' +
        (isNew ? '' : '<button class="btn-sm bad" type="button" onclick="rulesEditDelete()">删除规则</button>') +
        '</div><p class="rules-ai-hint">保存将整表写入 Hub（与导入相同热重载）。能力点列表来自当前 /api/points。</p></div>';
}

function rulesEditCollectIf() {
    const box = document.getElementById('re-if-list');
    if (!box) return [];
    const rows = box.querySelectorAll('[data-if-i]');
    const out = [];
    rows.forEach(function(row) {
        const i = row.getAttribute('data-if-i');
        const pid = document.getElementById('re-if-pid-' + i);
        const eq = document.getElementById('re-if-eq-' + i);
        const point_id = (pid && pid.value || '').trim();
        const equals = (eq && eq.value || '').trim();
        if (point_id && equals) out.push({
            point_id: point_id,
            equals: equals
        });
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
        const pidEl = document.getElementById('re-when-pid');
        const eqEl = document.getElementById('re-when-eq');
        when = {
            type: 'point',
            point_id: ((pidEl && pidEl.value) || '').trim(),
            equals: ((eqEl && eqEl.value) || '').trim()
        };
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
        if (!r.when.point_id || !r.when.equals) return '请完整填写触发能力点与 equals';
    } else if (r.when.type === 'time') {
        if (!/^\d{2}:\d{2}$/.test(r.when.at || '')) return '定时须为 HH:MM';
    } else return '触发类型无效';
    if (r.then.type === 'command') {
        if (!r.then.point_id || !r.then.cmd) return '请完整填写动作 point_id 与 cmd';
    } else if (r.then.type === 'delay') {
        if (!(r.then.seconds > 0)) return '延时秒数无效';
    } else return '动作类型无效';
    return null;
}

function rulesEditSyncWhenType() {
    const snap = rulesEditSnapshot();
    snap.when = snap.when.type === 'time' ? {
        type: 'time',
        at: snap.when.at || '23:00'
    } : {
        type: 'point',
        point_id: snap.when.point_id || '',
        equals: snap.when.equals || 'pressed'
    };
    /* type already from select — rebuild when box only */
    const typ = document.getElementById('re-when-type').value;
    const box = document.getElementById('re-when-box');
    if (!box) return;
    const cur = rulesEditSnapshot();
    if (typ === 'time') box.innerHTML = rulesEditWhenFields({
        type: 'time',
        at: (cur.when && cur.when.at) || '23:00'
    });
    else box.innerHTML = rulesEditWhenFields({
        type: 'point',
        point_id: (pts[0] && pts[0].point_id) || '',
        equals: 'pressed'
    });
}

function rulesEditSyncThenType() {
    const typ = document.getElementById('re-then-type').value;
    const box = document.getElementById('re-then-box');
    if (!box) return;
    if (typ === 'delay') box.innerHTML = rulesEditThenFields({
        type: 'delay',
        seconds: 5
    });
    else box.innerHTML = rulesEditThenFields({
        type: 'command',
        point_id: (pts[0] && pts[0].point_id) || '',
        cmd: 'turn_on'
    });
}

function rulesEditAddIf() {
    const list = document.getElementById('re-if-list');
    const empty = document.getElementById('re-if-empty');
    if (empty) empty.hidden = true;
    if (!list) return;
    const i = list.querySelectorAll('[data-if-i]').length;
    const wrap = document.createElement('div');
    wrap.className = 'row3 rule-if-row';
    wrap.setAttribute('data-if-i', String(i));
    wrap.innerHTML = '<div><label>point_id</label>' + rulesSelectHtml('re-if-pid-' + i, rulesPointOptions((pts[0] && pts[0].point_id) || '')) + '</div>' +
        '<div><label>equals</label>' + rulesEqInput('re-if-eq-' + i, 'on') + '</div>' +
        '<button class="btn-sm bad" type="button" onclick="rulesEditRemoveIf(' + i + ')">删除</button>';
    list.appendChild(wrap);
}

function rulesEditRemoveIf(i) {
    const cur = rulesEditSnapshot();
    const next = (cur.if || []).slice();
    /* rebuild from DOM excluding i */
    const list = document.getElementById('re-if-list');
    if (!list) return;
    const rows = [].slice.call(list.querySelectorAll('[data-if-i]'));
    const kept = [];
    rows.forEach(function(row) {
        const idx = row.getAttribute('data-if-i');
        if (String(idx) === String(i)) return;
        const pid = document.getElementById('re-if-pid-' + idx);
        const eq = document.getElementById('re-if-eq-' + idx);
        kept.push({
            point_id: (pid && pid.value) || '',
            equals: (eq && eq.value) || ''
        });
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
    if (rulesEditMode === 'new' && !(base.id)) {
        /* keep new */ } else if (rulesEditMode === 'new') rulesEditMode = 'new';
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
