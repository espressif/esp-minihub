#!/usr/bin/env python3
"""
Build-time guard for the device catalog and console string files.

Three tiers, and the tiers matter: a missing language COLUMN breaks the catalog,
while a missing or duplicate translation is normal for proper nouns (PM2.5, HSV,
CCT, Wi-Fi read the same in both languages). Reporting those as errors would
make the first run fail and teach everyone to ignore the tool.

  ERROR (exit 1)   label_zh / label_en column absent; empty label_zh cell;
                   option items without a labels map; the two web dictionaries
                   disagreeing on their key set; a t('key') or data-i18n key in
                   the console that no dictionary provides.
  WARNING          empty label_en cell; label_en identical to label_zh; the same
                   two cases per option item. Fix it, or it is a proper noun.

The runtime fallback chain (labels[lang] -> labels.zh -> parameter name) is
unchanged, so a warning really does degrade to Chinese rather than breaking.

Usage:
    python3 tools/check_catalog_i18n.py            # repo layout is assumed
Run from anywhere; paths come from this file's location.
"""
import csv
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
COMP = os.path.dirname(HERE)                      # components/minihub_device_widget
REPO = os.path.dirname(os.path.dirname(COMP))     # repo root
PARAMS_CSV = os.path.join(COMP, "model", "parameters.csv")
WEB_DIR = os.path.join(REPO, "components", "minihub_web", "web")
I18N_DIR = os.path.join(WEB_DIR, "i18n")
# One source for the language list: the generator's CSV-column mapping. Importing
# it rather than repeating the tuple means a new language cannot be added on one
# side only and silently pass this check.
sys.path.insert(0, HERE)
from gen_device_widget import LABEL_COLS

DEFAULT_LANG = "zh"           # the catalog's fallback language; see model.c
LANGS = tuple(LABEL_COLS.values())
# zh_CN is a region-tagged locale file; other codes map to <code>.json.
DICT_FILE = {"zh": "zh_CN.json"}
KEY_RE = re.compile(r"^[a-z][a-z0-9_-]*(?:\.[a-z0-9_-]+)+$")
CALL_RE = re.compile(r"\bt\(\s*([^)]*)\)")
STR_RE = re.compile(r"['\"]([^'\"]+)['\"]")
ATTR_RE = re.compile(r'data-i18n(?:-ph|-title|-aria|-alt)?="([^"]+)"')


def _s(v):
    return "" if v is None else str(v).strip()


def _has_cjk(text):
    return any("\u4e00" <= c <= "\u9fff" for c in text)


def check_label_pair(zh, en, where, errors, warnings):
    """One (zh, en) pair, for a parameter row or a single option item."""
    if not zh:
        errors.append(f"{where}: empty label_{DEFAULT_LANG}")
    if not en:
        warnings.append(f"{where}: empty label_en (falls back to Chinese)")
    elif zh and en == zh and _has_cjk(zh):
        # Equality is only worth flagging when there was Chinese to translate:
        # MQTT, Wi-Fi, PM2.5 and HSV read the same in both languages, and
        # shouting about them every build would train everyone to ignore this.
        warnings.append(f"{where}: label_en equals label_zh ('{en}'), "
                        f"fine for a proper noun, otherwise a missing translation")


def check_parameters(errors, warnings):
    if not os.path.exists(PARAMS_CSV):
        errors.append(f"model file not found: {PARAMS_CSV}")
        return
    with open(PARAMS_CSV, newline="", encoding="utf-8") as f:
        rdr = csv.DictReader(f)
        cols = rdr.fieldnames or []
        for col in ("label_zh", "label_en"):
            if col not in cols:
                errors.append(f"parameters.csv: missing column '{col}'")
        if "label_zh" not in cols or "label_en" not in cols:
            return
        for row in rdr:
            name = _s(row.get("parameter")) or "<no parameter>"
            check_label_pair(_s(row.get("label_zh")), _s(row.get("label_en")),
                             f"parameters.csv '{name}'", errors, warnings)
            opts = _s(row.get("options"))
            if not opts:
                continue
            try:
                items = json.loads(opts)
            except json.JSONDecodeError as exc:
                errors.append(f"parameters.csv '{name}': options is not valid JSON: {exc}")
                continue
            for i, o in enumerate(items):
                where = f"parameters.csv '{name}' options[{i}]"
                if not isinstance(o, dict) or "labels" not in o:
                    errors.append(f"{where}: needs a labels map "
                                  f'(no flat "label" — the card renderer would prefer it '
                                  "over the per-request language)")
                    continue
                labels = o["labels"]
                if not isinstance(labels, dict):
                    errors.append(f"{where}: labels is not an object")
                    continue
                check_label_pair(_s(labels.get("zh")), _s(labels.get("en")), where,
                                 errors, warnings)


def load_dicts():
    out = {}
    for lang in LANGS:
        path = os.path.join(I18N_DIR, DICT_FILE.get(lang, lang + ".json"))
        if not os.path.exists(path):
            return None, [f"missing dictionary: {path}"]
        try:
            with open(path, encoding="utf-8") as f:
                out[lang] = json.load(f)
        except (OSError, json.JSONDecodeError) as exc:
            return None, [f"unreadable {path}: {exc}"]
    return out, []


def code_keys():
    """Keys the console asks for: t('key') arguments and data-i18n* attributes.

    t() calls are matched by span so the ternary form
    t(setup ? 'login.title.setup' : 'login.title.signin') contributes both keys.
    Only dotted lowercase names count, which keeps strings that happen to sit in
    a call (an attribute name, a separator) out of the key list."""
    found = set()
    for root, _dirs, files in os.walk(WEB_DIR):
        for fn in files:
            if not fn.endswith((".js", ".html")):
                continue
            with open(os.path.join(root, fn), encoding="utf-8") as f:
                text = f.read()
            for m in ATTR_RE.finditer(text):
                found.add(m.group(1))
            for m in CALL_RE.finditer(text):
                for lit in STR_RE.finditer(m.group(1)):
                    if KEY_RE.match(lit.group(1)):
                        found.add(lit.group(1))
    return found


def check_web(errors, warnings):
    dicts, fatal = load_dicts()
    if fatal:
        errors.extend(fatal)
        return
    zh_keys, en_keys = set(dicts["zh"]), set(dicts["en"])
    if zh_keys != en_keys:
        errors.append("web/i18n: key sets differ — "
                      f"only in zh_CN: {sorted(zh_keys - en_keys) or '-'}, "
                      f"only in en: {sorted(en_keys - zh_keys) or '-'}")
    for key in sorted(code_keys()):
        if key not in zh_keys:
            errors.append(f"console asks for '{key}' but web/i18n/zh_CN.json has no such key")
    for key in sorted(zh_keys):
        if not KEY_RE.match(key):
            warnings.append(f"web/i18n: '{key}' is not a dotted lowercase key")
        zh, en = dicts["zh"][key], (dicts["en"].get(key) or "")
        if en and en == zh and _has_cjk(zh):
            warnings.append(f"web/i18n: '{key}' is '{en}' in both languages "
                            "(fine for a proper noun)")


TAG_RE = re.compile(r'<([a-zA-Z]+)([^>]*)>([^<]*)')
ID_RE = re.compile(r'\bid="([^"]+)"')


def check_static_markup(errors, warnings):
    """Chinese left in markup that nothing ever translates.

    An element with no data-i18n* attribute is fine IF some script writes its
    text (every console modal title is set that way, and most of them are set
    from device data). When no script mentions the id either, the string is
    simply untranslated and an English console will show Chinese. That is how
    #map-info slipped through: the attribute scan cannot see it."""
    js_text = ''
    js_dir = os.path.join(WEB_DIR, 'js')
    if os.path.isdir(js_dir):
        for fn in sorted(os.listdir(js_dir)):
            if fn.endswith('.js'):
                with open(os.path.join(js_dir, fn), encoding='utf-8') as f:
                    js_text += f.read()
    for fn in ('index.html', 'login.html'):
        path = os.path.join(WEB_DIR, fn)
        if not os.path.exists(path):
            continue
        with open(path, encoding='utf-8') as f:
            text = f.read()
        body = re.sub(r'<!--.*?-->', '', text, flags=re.S)
        body = re.sub(r'<script.*?</script>', '', body, flags=re.S)
        for m in TAG_RE.finditer(body):
            _tag, attrs, content = m.group(1), m.group(2), m.group(3)
            if not _has_cjk(content):
                continue
            if 'data-i18n' in attrs:
                continue
            mid = ID_RE.search(attrs)
            owner = mid.group(1) if mid else None
            if owner is None:
                # No id of its own: fall back to the nearest id declared earlier
                # in the markup, which is the enclosing element a script would
                # rewrite (e.g. a sub-div inside #camera-preview-ph).
                prev = list(ID_RE.finditer(body[:m.start()]))
                owner = prev[-1].group(1) if prev else None
            if owner and owner in js_text:
                continue      # a script owns this element's text
            warnings.append(
                f"{fn}: untranslated Chinese in markup, no data-i18n attribute and "
                f"no script writes this element: {content.strip()[:36]!r}")


def run():
    errors, warnings = [], []
    check_parameters(errors, warnings)
    check_web(errors, warnings)
    check_static_markup(errors, warnings)
    return errors, warnings


def report(errors, warnings, quiet_ok=False):
    for w in warnings:
        print(f"  warn  {w}")
    for e in errors:
        print(f"  ERROR {e}")
    if errors:
        print(f"catalog i18n check failed: {len(errors)} error(s), {len(warnings)} warning(s)")
        return 1
    if not quiet_ok:
        print(f"catalog i18n OK ({len(warnings)} warning(s))")
    return 0


def main():
    errors, warnings = run()
    sys.exit(report(errors, warnings))


if __name__ == "__main__":
    main()
