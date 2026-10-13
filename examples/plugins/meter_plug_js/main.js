"use strict";

/*
 * meter_plug_js — L2 JS demo (QuickJS on Hub)
 *
 * This is the JS (L2) replacement for the old examples/plugins/meter_plug L3 C
 * demo, which was removed from the tree. It registers one simulated smart plug
 * (type=switch) with a writable power param plus read-only voltage / current /
 * watts telemetry, and handles POST /api/devices/control via
 * host.command.on(adapter, fn). See color_bulb_js for the light-port of the
 * same approach.
 *
 * Requires Host bindings in components/minihub_js/minihub_js.c:
 *   host.point.register({ ..., params|props })
 *   host.point.set(id, value, props?)
 *   host.point.get → includes props
 *   host.command.on(adapter, handler)
 */

var ADAPTER = "meter_plug_js";
var MANUFACTURER = "Espressif";
var CATEGORY = "Smart Plug";

/* Embedded instance list (L2 has no module_read_file). */
var DEVICES = [
    {
        device: "plug",
        type: "switch",
        params: [
            { name: "power", init: false, access: "RW" },
            { name: "voltage", init: "220", access: "RO", unit: "V" },
            { name: "current", init: "0.0", access: "RO", unit: "A" },
            { name: "watts", init: "0", access: "RO", unit: "W" }
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
        if (typeof p.unit === "string" && p.unit) {
            ent.unit = p.unit;
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
        host.log.warn("meter_plug_js: no params for " + cmd.point_id);
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
    } else if (cname === "toggle") {
        param = "power";
        var cur = summaryValue(params);
        buf = cur === "on" ? "off" : "on";
    } else if (cmd.params) {
        if (typeof cmd.params.parameter === "string" && cmd.params.parameter) {
            param = cmd.params.parameter;
        }
        if (cmd.params.value != null) {
            buf = jsonToStr(cmd.params.value);
        }
        /* voltage / current / watts are read-only: refuse client writes. */
        if (param && params[param] && params[param].access === "RO") {
            host.log.warn("meter_plug_js: " + param + " is read-only");
            return false;
        }
        if (!buf) {
            host.log.warn("meter_plug_js: command needs params: " + cname);
            return false;
        }
    } else {
        host.log.warn("meter_plug_js: command needs params: " + cname);
        return false;
    }

    if (!param || !buf) {
        return false;
    }
    if (!setParam(params, param, buf)) {
        host.log.warn("meter_plug_js: unknown param " + param + " on " + cmd.point_id);
        return false;
    }
    publish(host, cmd.point_id, params);
    host.log.info("meter_plug_js: " + cmd.point_id + "." + param + " -> " + buf);
    return true;
}

function registerOne(host, desc) {
    var params = paramsFromDesc(desc.params);
    var pid = pointIdFor(desc.device);
    var typ = desc.type || "switch";
    host.point.register({
        point_id: pid,
        type: typ,
        adapter: ADAPTER,
        value: summaryValue(params),
        online: true,
        props: identityProps(params)
    });
    host.log.info("meter_plug_js: registered " + pid + " (type=" + typ + ")");
}

function setup(host) {
    host.log.info("meter_plug_js: setup");

    host.command.on(ADAPTER, function (cmd) {
        return handleCommand(host, cmd);
    });

    for (var i = 0; i < DEVICES.length; i++) {
        try {
            registerOne(host, DEVICES[i]);
        } catch (e) {
            host.log.error("meter_plug_js: register failed for " + DEVICES[i].device);
        }
    }
}
