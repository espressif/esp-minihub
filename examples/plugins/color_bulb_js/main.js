"use strict";

/*
 * color_bulb_js — L2 JS demo (QuickJS on Hub)
 *
 * Registers three simulated lights (type=light) with props.params so the
 * Devices page shows power / brightness / hue / saturation widgets, and
 * handles POST /api/devices/control via host.command.on(adapter, fn).
 *
 * This is the JS (L2) replacement for the old examples/plugins/color_bulb L3 C
 * demo (removed from the tree). See meter_plug_js for the plug equivalent.
 *
 * Requires Host bindings in components/minihub_js/minihub_js.c:
 *   host.point.register({ ..., params|props })
 *   host.point.set(id, value, props?)
 *   host.point.get → includes props
 *   host.command.on(adapter, handler)
 */

var ADAPTER = "color_bulb_js";
var MANUFACTURER = "Espressif";
var CATEGORY = "Color Bulb";

/* Embedded instance list (L2 has no module_read_file). */
var DEVICES = [
    {
        device: "js_kitchen",
        type: "light",
        params: [
            { name: "power", init: false, access: "RW" },
            { name: "brightness", init: "80", access: "RW", max: 100, requires: "power" },
            { name: "hue", init: "0", access: "RW", requires: { all: ["power"] } },
            { name: "saturation", init: "100", access: "RW", requires: { any: ["power"] } },
            { name: "cct", init: "4000", visible: false, access: "RW", requires: "power" }
        ]
    },
    {
        device: "js_desk",
        type: "light",
        params: [
            { name: "power", init: true, access: "RW" },
            { name: "brightness", init: "40", access: "RW", max: 100, requires: "power" },
            { name: "hue", init: "120", access: "RW", requires: "power" },
            { name: "saturation", init: "80", access: "RW", requires: "power" }
        ]
    },
    {
        device: "js_bedroom",
        type: "light",
        params: [
            { name: "power", init: true, access: "RW" },
            { name: "brightness", init: "60", access: "RW", requires: "power" },
            { name: "hue", init: "240", access: "RW", requires: "power" },
            { name: "saturation", init: "50", access: "RW", requires: "power" }
        ]
    }
];

function initToStr(v) {
    if (typeof v === "boolean") {
        return v ? "on" : "off";
    }
    if (v == null) {
        return "";
    }
    return String(v);
}

function paramsFromDesc(arr) {
    var out = {};
    if (!arr || !arr.length) {
        return out;
    }
    for (var i = 0; i < arr.length; i++) {
        var p = arr[i];
        if (!p || !p.name) {
            continue;
        }
        var ent = { value: initToStr(p.init), access: p.access || "RW" };
        if (p.visible === false) {
            ent.visible = false;
        }
        if (typeof p.min === "number") {
            ent.min = p.min;
        }
        if (typeof p.max === "number") {
            ent.max = p.max;
        }
        if (typeof p.step === "number") {
            ent.step = p.step;
        }
        if (typeof p.unit === "string" && p.unit) {
            ent.unit = p.unit;
        }
        if (typeof p.requires === "string" && p.requires) {
            ent.requires = p.requires;
        } else if (p.requires && typeof p.requires === "object" && !Array.isArray(p.requires)) {
            ent.requires = p.requires;
        }
        out[p.name] = ent;
    }
    return out;
}

function summaryValue(params) {
    if (!params || !params.power) {
        return "";
    }
    var v = params.power.value;
    return v != null ? String(v) : "";
}

function pointIdFor(device) {
    return "sim." + device;
}

function identityProps(params) {
    return {
        manufacturer: MANUFACTURER,
        category: CATEGORY,
        params: params
    };
}

function publish(host, pointId, params) {
    var value = summaryValue(params);
    if (host.point.mergeParams) {
        host.point.mergeParams(pointId, params, value);
    } else {
        host.point.set(pointId, value, identityProps(params));
    }
}

function setParam(params, name, valStr) {
    if (!params || !name || valStr == null) {
        return false;
    }
    if (!params[name] || typeof params[name] !== "object") {
        return false;
    }
    params[name].value = String(valStr);
    return true;
}

function jsonToStr(v) {
    if (v == null) {
        return "";
    }
    if (typeof v === "boolean") {
        return v ? "on" : "off";
    }
    if (typeof v === "object" && v.value != null) {
        return jsonToStr(v.value);
    }
    return String(v);
}

function handleCommand(host, cmd) {
    if (!cmd || !cmd.point_id) {
        return false;
    }
    var pt = host.point.get(cmd.point_id);
    if (!pt || !pt.props || !pt.props.params) {
        host.log.warn("color_bulb_js: no params for " + cmd.point_id);
        return false;
    }

    /* Shallow-clone params so we can mutate then point.set */
    var params = {};
    var src = pt.props.params;
    for (var k in src) {
        if (!Object.prototype.hasOwnProperty.call(src, k)) {
            continue;
        }
        var e = src[k];
        params[k] = {
            value: e.value != null ? String(e.value) : "",
            access: e.access
        };
        if (e.visible === false) {
            params[k].visible = false;
        }
        if (typeof e.min === "number") {
            params[k].min = e.min;
        }
        if (typeof e.max === "number") {
            params[k].max = e.max;
        }
        if (typeof e.step === "number") {
            params[k].step = e.step;
        }
        if (typeof e.unit === "string") {
            params[k].unit = e.unit;
        }
    }

    var param = null;
    var buf = "";
    var cname = cmd.cmd || "";

    if (cname === "turn_on") {
        param = "power";
        buf = "on";
    } else if (cname === "turn_off") {
        param = "power";
        buf = "off";
    } else if (cmd.params) {
        if (typeof cmd.params.parameter === "string" && cmd.params.parameter) {
            param = cmd.params.parameter;
        }
        if (cmd.params.value != null) {
            buf = jsonToStr(cmd.params.value);
        }
        if (!buf) {
            if (cname === "set_brightness") {
                param = "brightness";
                buf = jsonToStr(cmd.params.brightness);
            } else if (cname === "set_color") {
                param = "hue";
                buf = jsonToStr(cmd.params.hue);
                if (!buf) {
                    buf = jsonToStr(cmd.params.color);
                }
            } else if (cname === "set_color_temp") {
                param = "cct";
                var ct = (cmd.params.cct != null) ? cmd.params.cct
                       : (cmd.params.color_temp != null) ? cmd.params.color_temp
                       : cmd.params.temperature;
                buf = jsonToStr(ct);
            } else if (cname === "toggle") {
                param = param || "power";
                var cur = summaryValue(params);
                buf = cur === "on" ? "off" : "on";
            }
        }
        if (!param) {
            param = "power";
        }
    } else {
        host.log.warn("color_bulb_js: command needs params: " + cname);
        return false;
    }

    if (!param || !buf) {
        return false;
    }
    if (!setParam(params, param, buf)) {
        host.log.warn("color_bulb_js: unknown param " + param + " on " + cmd.point_id);
        return false;
    }
    publish(host, cmd.point_id, params);
    host.log.info("color_bulb_js: " + cmd.point_id + "." + param + " -> " + buf);
    return true;
}

function registerOne(host, desc) {
    var params = paramsFromDesc(desc.params);
    var pid = pointIdFor(desc.device);
    var typ = desc.type || "light";
    host.point.register({
        point_id: pid,
        type: typ,
        adapter: ADAPTER,
        value: summaryValue(params),
        online: true,
        props: identityProps(params)
    });
    host.log.info("color_bulb_js: registered " + pid + " (type=" + typ + ")");
}

function setup(host) {
    host.log.info("color_bulb_js: setup");

    host.command.on(ADAPTER, function (cmd) {
        return handleCommand(host, cmd);
    });

    for (var i = 0; i < DEVICES.length; i++) {
        try {
            registerOne(host, DEVICES[i]);
        } catch (e) {
            host.log.error("color_bulb_js: register failed for " + DEVICES[i].device);
        }
    }
}
