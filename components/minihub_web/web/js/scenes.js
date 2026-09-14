/* MiniHub web — scenes page */
let scenesCfg = {
    scenes: []
};
let scenesSel = null;
let scenesEditMode = null;
async function loadScenes() {
    try {
        const r = await fetch('/api/scenes');
        if (!r.ok) throw new Error('HTTP ' + r.status);
        scenesCfg = await r.json();
        if (!scenesCfg || !Array.isArray(scenesCfg.scenes)) scenesCfg = {
            scenes: []
        };
        if (view === 'scenes') renderScenes();
    } catch (e) {
        toast('场景加载失败');
        const el = document.getElementById('scenes-list');
        if (el) el.innerHTML = '<div class="empty">加载失败</div>';
    }
}

function sceneCmdSummary(cmds) {
    const arr = Array.isArray(cmds) ? cmds : [];
    if (!arr.length) return '无指令';
    return arr.slice(0, 3).map(function(c) {
        return (c.cmd || '?') + ' → ' + (c.point_id || '?');
    }).join(' · ') + (arr.length > 3 ? ' …' : '');
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
        const sc = (scenesCfg.scenes || []).find(function(x) {
            return x.id === scenesSel;
        });
        list.hidden = true;
        detail.hidden = false;
        if (!sc) {
            scenesSel = null;
            renderScenes();
            return;
        }
        const cmds = Array.isArray(sc.commands) ? sc.commands : [];
        const steps = cmds.map(function(c, i) {
            return '<div class="rule-step"><div class="rule-step-label">步骤 ' + (i + 1) + '</div><div class="rule-step-title">' + esc(c.cmd || '?') + '</div><div class="rule-step-body">目标 <b>' + esc(c.point_id || '?') + '</b>' + (c.delay_ms ? (' · 延时 ' + esc(String(c.delay_ms)) + ' ms') : '') + '</div></div>';
        }).join('<div class="rule-arrow">↓</div>');
        detail.innerHTML = '<button class="btn-sm" type="button" onclick="scenesBack()">← 返回列表</button>' +
            '<button class="btn-sm on" type="button" style="margin-left:8px" onclick="scenesEditOpen(\'' + escAttr(sc.id) + '\')">编辑</button>' +
            '<button class="btn-sm on" type="button" style="margin-left:8px" onclick="scenesInvoke(\'' + escAttr(sc.id) + '\')">执行</button>' +
            '<div class="dcard" style="margin-top:12px"><div class="card-top"><div class="card-name">' + esc(sc.name || sc.id || '场景') + '</div><span class="badge sensor">' + cmds.length + ' 步</span></div>' +
            '<div class="card-meta"><span>' + esc(sc.id || '') + '</span></div><div class="rule-flow">' + (steps || '<div class="empty">无指令</div>') + '</div></div>';
        return;
    }
    detail.hidden = true;
    list.hidden = false;
    const arr = scenesCfg.scenes || [];
    if (!arr.length) {
        list.className = '';
        list.innerHTML = '<div class="empty">暂无场景 — 点击「添加场景」创建跨适配器一键指令</div>';
        return;
    }
    list.className = 'grid';
    list.innerHTML = arr.map(function(s) {
        const n = (Array.isArray(s.commands) ? s.commands : []).length;
        return '<div class="dcard" style="cursor:pointer" data-scene-id="' + escAttr(s.id || '') + '">' +
            '<div class="card-top"><div class="card-name">' + esc(s.name || s.id || '(unnamed)') + '</div><span class="badge sensor">' + n + ' 步</span></div>' +
            '<div class="card-meta"><span>' + esc(sceneCmdSummary(s.commands)) + '</span></div>' +
            '<div class="card-actions" style="margin-top:10px"><button class="btn-sm on" type="button" data-invoke="' + escAttr(s.id || '') + '">执行</button></div></div>';
    }).join('');
    list.onclick = function(ev) {
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
    toast('执行中…');
    try {
        const r = await fetch('/api/scenes/invoke', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify({
                scene_id: id
            })
        });
        if (r.ok) toast('场景已执行');
        else {
            let m = '失败';
            try {
                const j = await r.json();
                m = j.error || j.message || m;
            } catch (e) {}
            toast(String(m).slice(0, 80));
        }
    } catch (e) {
        toast('网络错误');
    }
}
async function scenesEditNew() {
    if (!pts || !pts.length) {
        try {
            await loadPoints();
        } catch (e) {}
    }
    scenesSel = null;
    scenesEditMode = 'new';
    scenesEditRender(null);
}
async function scenesEditOpen(id) {
    if (!pts || !pts.length) {
        try {
            await loadPoints();
        } catch (e) {}
    }
    const sc = (scenesCfg.scenes || []).find(function(x) {
        return x.id === id;
    });
    if (!sc) {
        toast('场景不存在');
        return;
    }
    scenesEditMode = id;
    scenesEditRender(sc);
}

function scenesEditCancel() {
    scenesEditMode = null;
    renderScenes();
}

function scenesEditCmdRow(c, idx) {
    c = c || {};
    return '<div class="scenes-cmd-row" data-idx="' + idx + '"><div><label>point_id</label>' + rulesSelectHtml('sc-pid-' + idx, rulesPointOptions(c.point_id || '')) + '</div>' +
        '<div><label>cmd</label><input type="text" id="sc-cmd-' + idx + '" value="' + escAttr(c.cmd || 'turn_off') + '" placeholder="turn_on / turn_off" /></div>' +
        '<div><label>delay_ms</label><input type="number" id="sc-delay-' + idx + '" min="0" step="100" value="' + (c.delay_ms != null ? escAttr(String(c.delay_ms)) : '0') + '" /></div>' +
        '<button class="btn-sm bad" type="button" onclick="scenesEditRmCmd(' + idx + ')" title="删除">×</button></div>';
}

function scenesEditRender(sc) {
    const edit = document.getElementById('scenes-edit');
    const list = document.getElementById('scenes-list');
    const detail = document.getElementById('scenes-detail');
    if (!edit) return;
    if (list) list.hidden = true;
    if (detail) detail.hidden = true;
    edit.hidden = false;
    const isNew = !sc;
    const id = sc && sc.id ? sc.id : ('scene_' + Date.now().toString(36).slice(-6));
    const name = sc && sc.name ? sc.name : '';
    const cmds = Array.isArray(sc && sc.commands) ? sc.commands.slice() : [{
        point_id: '',
        cmd: 'turn_off',
        delay_ms: 0
    }];
    window._scenesEditCmds = cmds;
    edit.innerHTML = '<button class="btn-sm" type="button" onclick="scenesEditCancel()">← 取消</button>' +
        '<div class="dcard" style="margin-top:12px"><div class="card-name">' + (isNew ? '新建场景' : '编辑场景') + '</div>' +
        '<label>id</label><input id="sc-id" ' + (isNew ? '' : 'readonly ') + 'value="' + escAttr(id) + '" />' +
        '<label>名称</label><input id="sc-name" value="' + escAttr(name) + '" placeholder="如：晚安" />' +
        '<div style="margin-top:12px"><div class="card-name" style="font-size:.85rem">指令序列</div><div id="sc-cmds"></div>' +
        '<button class="btn-sm" type="button" onclick="scenesEditAddCmd()">+ 添加指令</button></div>' +
        '<div class="modal-actions" style="margin-top:14px"><button class="btn-sm on" type="button" onclick="scenesEditSave()">保存</button>' +
        (isNew ? '' : '<button class="btn-sm bad" type="button" onclick="scenesEditDelete()">删除</button>') + '</div></div>';
    scenesEditPaintCmds();
}

function scenesEditPaintCmds() {
    const box = document.getElementById('sc-cmds');
    if (!box) return;
    const cmds = window._scenesEditCmds || [];
    box.innerHTML = cmds.map(function(c, i) {
        return scenesEditCmdRow(c, i);
    }).join('');
}

function scenesEditCollectCmds() {
    const cmds = window._scenesEditCmds || [];
    const out = [];
    for (let i = 0; i < cmds.length; i++) {
        const pid = document.getElementById('sc-pid-' + i);
        const cmd = document.getElementById('sc-cmd-' + i);
        const d = document.getElementById('sc-delay-' + i);
        const item = {
            point_id: (pid && pid.value || '').trim(),
            cmd: (cmd && cmd.value || '').trim()
        };
        const dm = d ? parseInt(d.value, 10) : 0;
        if (dm > 0) item.delay_ms = dm;
        if (item.point_id && item.cmd) out.push(item);
    }
    return out;
}

function scenesEditAddCmd() {
    window._scenesEditCmds = scenesEditCollectCmds();
    window._scenesEditCmds.push({
        point_id: '',
        cmd: 'turn_off',
        delay_ms: 0
    });
    scenesEditPaintCmds();
}

function scenesEditRmCmd(idx) {
    const cur = scenesEditCollectCmds();
    cur.splice(idx, 1);
    if (!cur.length) cur.push({
        point_id: '',
        cmd: 'turn_off',
        delay_ms: 0
    });
    window._scenesEditCmds = cur;
    scenesEditPaintCmds();
}
async function scenesEditSave() {
    const id = (document.getElementById('sc-id').value || '').trim();
    const name = (document.getElementById('sc-name').value || '').trim() || id;
    if (!id) {
        toast('请填写 id');
        return;
    }
    const commands = scenesEditCollectCmds();
    if (!commands.length) {
        toast('至少一条指令');
        return;
    }
    const next = (scenesCfg.scenes || []).slice();
    const item = {
        id: id,
        name: name,
        commands: commands
    };
    const ix = next.findIndex(function(x) {
        return x.id === id;
    });
    if (scenesEditMode === 'new') {
        if (ix >= 0) {
            toast('id 已存在');
            return;
        }
        next.push(item);
    } else {
        if (ix < 0) next.push(item);
        else next[ix] = item;
    }
    try {
        const r = await fetch('/api/scenes/import', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify({
                scenes: next
            })
        });
        if (!r.ok) {
            toast('保存失败');
            return;
        }
        scenesEditMode = null;
        scenesSel = id;
        await loadScenes();
        toast('已保存');
    } catch (e) {
        toast('网络错误');
    }
}
async function scenesEditDelete() {
    const id = (document.getElementById('sc-id').value || '').trim();
    if (!id || !confirm('删除场景 ' + id + '？')) return;
    const next = (scenesCfg.scenes || []).filter(function(x) {
        return x.id !== id;
    });
    try {
        const r = await fetch('/api/scenes/import', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify({
                scenes: next
            })
        });
        if (!r.ok) {
            toast('删除失败');
            return;
        }
        scenesEditMode = null;
        scenesSel = null;
        await loadScenes();
        toast('已删除');
    } catch (e) {
        toast('网络错误');
    }
}
