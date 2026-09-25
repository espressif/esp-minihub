function paintMapDevices(hub) {
    if (!deviceLayer || !hub) return;
    deviceLayer.clearLayers();
    const list = pts.filter(p => adapterEnabled(p.adapter));
    const cos = Math.cos(hub[0] * Math.PI / 180) || 1;
    list.forEach(p => {
        const o = hashOffset(p.point_id);
        const lat = hub[0] + o.dlat,
            lon = hub[1] + o.dlon / cos;
        const on = p.value === 'on' || p.value === 'true';
        const col = on ? '#22c55e' : (p.online ? '#6366f1' : '#5c6b85');
        const m = L.circleMarker([lat, lon], {
            radius: 7,
            color: '#fff',
            weight: 2,
            fillColor: col,
            fillOpacity: .9
        });
        m.bindPopup('<b>' + esc(p.point_id.split('/').pop()) + '</b><br>' + esc(p.value) + (p.online ? '' : ' （离线）'));
        deviceLayer.addLayer(m);
        if (p.online && (p.type === 'switch' || p.type === 'light')) {
            m.on('click', () => cmd(p.point_id, on ? 'turn_off' : 'turn_on'));
        }
    });
}
let leafletLoading = null;

function ensureLeaflet() {
    if (window.L) return Promise.resolve();
    if (leafletLoading) return leafletLoading;
    leafletLoading = new Promise((resolve, reject) => {
        const css = document.createElement('link');
        css.rel = 'stylesheet';
        css.href = 'https://unpkg.com/leaflet@1.9.4/dist/leaflet.css';
        document.head.appendChild(css);
        const sc = document.createElement('script');
        sc.src = 'https://unpkg.com/leaflet@1.9.4/dist/leaflet.js';
        sc.async = true;
        const t = setTimeout(() => {
            reject(new Error('leaflet timeout'));
        }, 8000);
        sc.onload = () => {
            clearTimeout(t);
            resolve();
        };
        sc.onerror = () => {
            clearTimeout(t);
            reject(new Error('leaflet load failed'));
        };
        document.head.appendChild(sc);
    }).catch(e => {
        leafletLoading = null;
        throw e;
    });
    return leafletLoading;
}
async function renderMap() {
    if (!window.L) {
        const info = document.getElementById('map-info');
        if (info) info.textContent = '正在加载地图库…';
        return;
    }
    const info = document.getElementById('map-info');
    if (typeof L === 'undefined') {
        info.textContent = '地图库未加载';
        return;
    }
    const fallback = {
        ok: true,
        source: 'default',
        lat: 31.2304,
        lon: 121.4737,
        city: '',
        address: '定位中…',
        map_provider: 'amap'
    };
    let geo = geoLoc && geoLoc.lat ? geoLoc : null;
    if (!geo) {
        info.textContent = '正在定位…（设备已显示）';
        try {
            geo = await loadGeo(false);
        } catch (e) {
            geo = null;
        }
    }
    if (!geo || !geo.lat) geo = fallback;
    const src = geo.source === 'browser' ? '浏览器 IP' : (geo.source === 'default' ? '默认中心' : 'Hub 公网 IP');
    info.innerHTML = (geo.source === 'default' ? '定位中…' : '大概位置：<b>' + esc(geo.address || geo.city || '-') + '</b>') + ' · ' + src + (geo.public_ip ? ' · 公网 ' + esc(geo.public_ip) : '') + (geo.local_ip ? ' · 局域网 ' + esc(geo.local_ip) : '');
    const cn = geo.map_provider === 'amap' || inChina(geo) || geo.source === 'default';
    const hub = cn ? wgs84togcj02(geo.lat, geo.lon) : [geo.lat, geo.lon];
    const el = document.getElementById('map-leaflet');
    if (!leafletMap) {
        leafletMap = L.map(el, {
            zoomControl: true
        }).setView(hub, 12);
        addMapBasemap(leafletMap, geo);
        deviceLayer = L.layerGroup().addTo(leafletMap);
        L.marker(hub).addTo(leafletMap).bindPopup('<b>MiniHub</b><br>' + esc(geo.address || '')).openPopup();
    } else if (geo.source !== 'default') {
        leafletMap.setView(hub, leafletMap.getZoom());
    }
    paintMapDevices(hub);
    if (geo.source === 'default' || !geoLoc) {
        loadGeo(true).then(g => {
            if (g && g.lat && view === 'map') renderMap();
        }).catch(() => {});
    }
}
