/* Client-side localisation for the console: one page tree, one string file per
 * language, switched in the top bar and remembered per origin.
 *
 * The resolution order lives in resolveLang() and nowhere else:
 *   localStorage['minihub.lang'] > browser language (zh* -> zh_CN) > en
 * A manually chosen language always wins over the browser default, which is what
 * makes the "clear your browser language and revisit" acceptance case deterministic.
 */
(function () {
    'use strict';

    const LANG_KEY = 'minihub.lang';
    const FALLBACK = 'zh_CN';
    const I18N_DIR = '/web/i18n/';
    const PENDING_CLASS = 'i18n-pending';
    /* If the dictionary never arrives (flash read stalled, mid-upgrade), still show
     * the page: a blank console is worse than a Chinese one. */
    const READY_TIMEOUT_MS = 3000;

    const LOCALES = { zh_CN: 'zh-CN', en: 'en-US' };

    function isLang(v) {
        return v === 'zh_CN' || v === 'en';
    }

    function resolveLang() {
        let stored = null;
        try {
            stored = localStorage.getItem(LANG_KEY);
        } catch (e) {
            /* Private mode / storage disabled: fall through to the browser. */
        }
        if (isLang(stored)) {
            return stored;
        }
        const wanted = (navigator.languages && navigator.languages.length)
            ? navigator.languages : [navigator.language || ''];
        for (let i = 0; i < wanted.length; i++) {
            if (String(wanted[i]).toLowerCase().indexOf('zh') === 0) {
                return 'zh_CN';
            }
        }
        return 'en';
    }

    const lang = resolveLang();
    /* strings = the active language, base = zh_CN as the fallback layer. An unknown
     * key renders as its own name, so a missing translation is visible in the UI
     * instead of silently showing nothing. */
    let strings = {};
    let base = {};

    function fetchDict(name) {
        return fetch(I18N_DIR + name + '.json', { cache: 'no-store' })
            .then(function (r) {
                return (r && r.ok) ? r.json() : null;
            })
            .catch(function () {
                return null;
            });
    }

    function t(key, vars) {
        let s = Object.prototype.hasOwnProperty.call(strings, key) ? strings[key] : null;
        if (s === null && Object.prototype.hasOwnProperty.call(base, key)) {
            s = base[key];
        }
        if (s === null) {
            return key;
        }
        if (vars) {
            for (const k in vars) {
                if (Object.prototype.hasOwnProperty.call(vars, k)) {
                    s = s.split('{' + k + '}').join(String(vars[k]));
                }
            }
        }
        return s;
    }

    /* Catalog endpoints key languages by ISO 639-1 (the catalog has no region
     * dimension), while localStorage holds a locale. Map it in exactly one place. */
    function mhCatalogLang() {
        return lang.split('_')[0];
    }

    function mhCatalogUrl(path) {
        return path + (path.indexOf('?') >= 0 ? '&' : '?') + 'lang=' + mhCatalogLang();
    }

    function mhLocale() {
        return LOCALES[lang] || 'en-US';
    }

    function applyI18n(root) {
        const scope = root || document;
        scope.querySelectorAll('[data-i18n]').forEach(function (el) {
            el.textContent = t(el.getAttribute('data-i18n'));
        });
        scope.querySelectorAll('[data-i18n-ph]').forEach(function (el) {
            el.placeholder = t(el.getAttribute('data-i18n-ph'));
        });
        scope.querySelectorAll('[data-i18n-title]').forEach(function (el) {
            el.title = t(el.getAttribute('data-i18n-title'));
        });
        scope.querySelectorAll('[data-i18n-aria]').forEach(function (el) {
            el.setAttribute('aria-label', t(el.getAttribute('data-i18n-aria')));
        });
        scope.querySelectorAll('[data-i18n-alt]').forEach(function (el) {
            el.setAttribute('alt', t(el.getAttribute('data-i18n-alt')));
        });
        if (scope === document) {
            document.documentElement.lang = (lang === 'zh_CN') ? 'zh-CN' : 'en';
            const sel = document.getElementById('lang-sel');
            if (sel) {
                sel.value = lang;
            }
        }
    }

    function reveal() {
        document.body.classList.remove(PENDING_CLASS);
    }

    const i18nReady = (function () {
        const wanted = (lang === FALLBACK) ? [fetchDict(FALLBACK)] : [fetchDict(lang), fetchDict(FALLBACK)];
        return Promise.all(wanted).then(function (parts) {
            base = parts[1] || parts[0] || {};
            strings = parts[0] || base;
            applyI18n(document);
        });
    })();

    /* Whichever way this settles, the page becomes visible: a rejected or timed-out
     * dictionary must not leave a blank screen. */
    i18nReady.catch(function () {}).then(reveal);
    setTimeout(reveal, READY_TIMEOUT_MS);

    function setLang(next) {
        if (!isLang(next) || next === lang) {
            return;
        }
        try {
            localStorage.setItem(LANG_KEY, next);
        } catch (e) {}
        /* Re-rendering every view by hand is more code than a reload on a device
         * console, and the catalog is refetched anyway. */
        location.reload();
    }

    window.mhLang = lang;
    window.t = t;
    window.applyI18n = applyI18n;
    window.i18nReady = i18nReady;
    window.setLang = setLang;
    window.mhCatalogLang = mhCatalogLang;
    window.mhCatalogUrl = mhCatalogUrl;
    window.mhLocale = mhLocale;
})();
