/* MiniHub web — startup */
/* mhWebBuild:2026-08-17-scenes-cam */
window.mhWebBuild = '2026-09-01-matter-card-v7';
document.querySelectorAll('.nav-item').forEach(n => n.onclick = () => switchView(n.dataset.view));
document.getElementById('menu-btn').onclick = () => document.getElementById('sidebar').classList.toggle('open');

document.getElementById('user-row').onclick = () => {
    const n = prompt('设置本地显示名称', localName());
    if (n && n.trim()) {
        localStorage.setItem('minihub_display_name', n.trim());
        renderLocalUser();
    }
};

document.querySelectorAll('#store-tabs .chip').forEach(c => {
    c.onclick = () => {
        storeTab = c.dataset.tab;
        renderStore();
    };
}); /* Adapters → modules (auth UI) → status → points. Catalog is non-blocking. */
window.addEventListener('hashchange', applyLoginHash);
loadAdapters().then(async () => {
    await loadPoints();
    loadModulesForAuth().then(() => {
        loadMeta();
        applyLoginHash();
    });
    loadRtspCamStatusSafe();
    loadStoreData();
});
bindLightDetailControls();
renderLocalUser();
setInterval(loadPoints, 8000);
setInterval(loadRtspCamStatusSafe, 12000);
setInterval(() => loadAdapters().then(async () => {
    await loadModulesForAuth();
    await loadMeta();
}), 30000);
setInterval(() => {
    if (!(adapterEnabled('zigbee') || adapterEnabled('ble_mesh'))) return;
    refreshRadioProv();
}, 5000);
