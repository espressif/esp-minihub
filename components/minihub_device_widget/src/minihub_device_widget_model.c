/*
 * Model CSV (devices.csv + parameters.csv) → flat runtime /data/config/devices.csv.
 * Mirrors tools/gen_device_widget.py build_runtime_entries / emit_csv (no services).
 */
#include "minihub_device_widget.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_log.h"

static const char *TAG = "dw_model";

#define DW_MODEL_MAX_BYTES (128 * 1024)
#define DW_CSV_MAX_COLS 24
#define DW_CSV_MAX_ROWS 256

static const char *const CLOSED_UI[] = {
    "ui.toggle", "ui.slider", "ui.dropdown", "ui.text", "ui.hidden",
};
/* Deprecated aliases still accepted (same as gen_device_widget.py). */
static const char *const UI_ALIAS[] = {
    "ui.hue-slider", "ui.hue-circle",
};

static const char *const DEVICE_REQ_COLS[] = {
    "device_id", "parameters", "primary_param", "mandatory_params", "image",
};
static const char *const PARAM_REQ_COLS[] = {
    "parameter", "label", "data_type", "properties", "min", "max", "step", "unit",
    "ui_element", "options", "description", "visible", "css", "image",
};

typedef struct {
    char **fields;
    int ncols;
} dw_csv_row_t;

typedef struct {
    dw_csv_row_t *rows;
    int nrows;
    char **header;
    int ncols;
} dw_csv_table_t;

static void dw_err(char *errbuf, size_t errbuf_len, const char *msg)
{
    if (errbuf && errbuf_len) {
        snprintf(errbuf, errbuf_len, "%s", msg ? msg : "error");
    }
}

static void dw_errf(char *errbuf, size_t errbuf_len, const char *fmt, ...)
{
    if (!errbuf || !errbuf_len) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(errbuf, errbuf_len, fmt, ap);
    va_end(ap);
}

static bool dw_ui_ok(const char *ui)
{
    if (!ui || !ui[0]) {
        return true; /* empty allowed; widget falls back to text */
    }
    for (size_t i = 0; i < sizeof(CLOSED_UI) / sizeof(CLOSED_UI[0]); i++) {
        if (strcmp(ui, CLOSED_UI[i]) == 0) {
            return true;
        }
    }
    for (size_t i = 0; i < sizeof(UI_ALIAS) / sizeof(UI_ALIAS[0]); i++) {
        if (strcmp(ui, UI_ALIAS[i]) == 0) {
            return true;
        }
    }
    return false;
}

static const char *dw_ui_to_widget(const char *ui)
{
    if (!ui || !ui[0]) {
        return "text";
    }
    if (strcmp(ui, "ui.toggle") == 0) {
        return "toggle";
    }
    if (strcmp(ui, "ui.slider") == 0 || strcmp(ui, "ui.hue-slider") == 0 ||
        strcmp(ui, "ui.hue-circle") == 0) {
        return "slider";
    }
    if (strcmp(ui, "ui.dropdown") == 0) {
        return "dropdown";
    }
    if (strcmp(ui, "ui.text") == 0) {
        return "text";
    }
    if (strcmp(ui, "ui.hidden") == 0) {
        return "hidden";
    }
    if (strncmp(ui, "ui.", 3) == 0) {
        return ui + 3; /* best-effort */
    }
    return "text";
}

static void dw_csv_row_free(dw_csv_row_t *r)
{
    if (!r || !r->fields) {
        return;
    }
    for (int i = 0; i < r->ncols; i++) {
        free(r->fields[i]);
    }
    free(r->fields);
    r->fields = NULL;
    r->ncols = 0;
}

static void dw_csv_table_free(dw_csv_table_t *t)
{
    if (!t) {
        return;
    }
    if (t->header) {
        for (int i = 0; i < t->ncols; i++) {
            free(t->header[i]);
        }
        free(t->header);
    }
    if (t->rows) {
        for (int i = 0; i < t->nrows; i++) {
            dw_csv_row_free(&t->rows[i]);
        }
        free(t->rows);
    }
    memset(t, 0, sizeof(*t));
}

/* Parse one RFC4180-ish row; returns field count, -1 EOF, -2 OOM/error. */
static int dw_csv_parse_row(const char **cursor, char ***out_fields, int *out_n)
{
    const char *p = *cursor;
    *out_fields = NULL;
    *out_n = 0;
    if (*p == '\0') {
        return -1;
    }

    char **fields = NULL;
    int n = 0, cap = 0;
    char *fb = malloc(128);
    size_t fcap = 128, flen = 0;
    if (!fb) {
        return -2;
    }
    bool inq = false;

    for (;;) {
        char c = *p;
        if (c == '\0' || (!inq && (c == '\n' || c == '\r'))) {
            /* emit last field */
            char *cell = malloc(flen + 1);
            if (!cell) {
                goto oom;
            }
            memcpy(cell, fb, flen);
            cell[flen] = '\0';
            if (n == cap) {
                int nc = cap ? cap * 2 : 8;
                char **nf = realloc(fields, (size_t)nc * sizeof(*nf));
                if (!nf) {
                    free(cell);
                    goto oom;
                }
                fields = nf;
                cap = nc;
            }
            fields[n++] = cell;
            if (c == '\r' && p[1] == '\n') {
                p += 2;
            } else if (c != '\0') {
                p++;
            }
            free(fb);
            *cursor = p;
            *out_fields = fields;
            *out_n = n;
            return n;
        }
        if (c == '"') {
            if (inq && p[1] == '"') {
                if (flen + 1 >= fcap) {
                    char *nb = realloc(fb, fcap * 2);
                    if (!nb) {
                        goto oom;
                    }
                    fb = nb;
                    fcap *= 2;
                }
                fb[flen++] = '"';
                p += 2;
                continue;
            }
            inq = !inq;
            p++;
            continue;
        }
        if (!inq && c == ',') {
            char *cell = malloc(flen + 1);
            if (!cell) {
                goto oom;
            }
            memcpy(cell, fb, flen);
            cell[flen] = '\0';
            if (n == cap) {
                int nc = cap ? cap * 2 : 8;
                char **nf = realloc(fields, (size_t)nc * sizeof(*nf));
                if (!nf) {
                    free(cell);
                    goto oom;
                }
                fields = nf;
                cap = nc;
            }
            fields[n++] = cell;
            flen = 0;
            p++;
            continue;
        }
        if (flen + 1 >= fcap) {
            char *nb = realloc(fb, fcap * 2);
            if (!nb) {
                goto oom;
            }
            fb = nb;
            fcap *= 2;
        }
        fb[flen++] = c;
        p++;
    }

oom:
    free(fb);
    if (fields) {
        for (int i = 0; i < n; i++) {
            free(fields[i]);
        }
        free(fields);
    }
    return -2;
}

static esp_err_t dw_csv_parse_table(const char *text, size_t len, dw_csv_table_t *out,
                                    char *errbuf, size_t errbuf_len)
{
    memset(out, 0, sizeof(*out));
    if (!text || len == 0) {
        dw_err(errbuf, errbuf_len, "empty csv");
        return ESP_ERR_INVALID_ARG;
    }
    /* Ensure NUL-terminated working copy */
    char *buf = malloc(len + 1);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(buf, text, len);
    buf[len] = '\0';

    const char *cur = buf;
    char **hdr = NULL;
    int nh = 0;
    int rc = dw_csv_parse_row(&cur, &hdr, &nh);
    if (rc < 0) {
        free(buf);
        dw_err(errbuf, errbuf_len, rc == -2 ? "oom" : "missing header");
        return rc == -2 ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_ARG;
    }
    if (nh > DW_CSV_MAX_COLS) {
        for (int i = 0; i < nh; i++) {
            free(hdr[i]);
        }
        free(hdr);
        free(buf);
        dw_err(errbuf, errbuf_len, "too many columns");
        return ESP_ERR_INVALID_ARG;
    }
    out->header = hdr;
    out->ncols = nh;

    dw_csv_row_t *rows = NULL;
    int nrows = 0, rcap = 0;
    while (*cur) {
        char **fields = NULL;
        int nf = 0;
        rc = dw_csv_parse_row(&cur, &fields, &nf);
        if (rc == -1) {
            break;
        }
        if (rc == -2) {
            for (int i = 0; i < nrows; i++) {
                dw_csv_row_free(&rows[i]);
            }
            free(rows);
            dw_csv_table_free(out);
            free(buf);
            return ESP_ERR_NO_MEM;
        }
        /* skip blank lines */
        if (nf == 1 && fields[0] && fields[0][0] == '\0') {
            free(fields[0]);
            free(fields);
            continue;
        }
        if (nrows >= DW_CSV_MAX_ROWS) {
            for (int i = 0; i < nf; i++) {
                free(fields[i]);
            }
            free(fields);
            for (int i = 0; i < nrows; i++) {
                dw_csv_row_free(&rows[i]);
            }
            free(rows);
            dw_csv_table_free(out);
            free(buf);
            dw_err(errbuf, errbuf_len, "too many rows");
            return ESP_ERR_INVALID_ARG;
        }
        if (nrows == rcap) {
            int nc = rcap ? rcap * 2 : 8;
            dw_csv_row_t *nr = realloc(rows, (size_t)nc * sizeof(*nr));
            if (!nr) {
                for (int i = 0; i < nf; i++) {
                    free(fields[i]);
                }
                free(fields);
                for (int i = 0; i < nrows; i++) {
                    dw_csv_row_free(&rows[i]);
                }
                free(rows);
                dw_csv_table_free(out);
                free(buf);
                return ESP_ERR_NO_MEM;
            }
            rows = nr;
            rcap = nc;
        }
        rows[nrows].fields = fields;
        rows[nrows].ncols = nf;
        nrows++;
    }
    out->rows = rows;
    out->nrows = nrows;
    free(buf);
    return ESP_OK;
}

static int dw_col_index(const dw_csv_table_t *t, const char *name)
{
    for (int i = 0; i < t->ncols; i++) {
        if (t->header[i] && strcasecmp(t->header[i], name) == 0) {
            return i;
        }
    }
    return -1;
}

static const char *dw_cell(const dw_csv_table_t *t, int row, int col)
{
    if (row < 0 || row >= t->nrows || col < 0) {
        return "";
    }
    const dw_csv_row_t *r = &t->rows[row];
    if (col >= r->ncols || !r->fields[col]) {
        return "";
    }
    return r->fields[col];
}

static esp_err_t dw_require_cols(const dw_csv_table_t *t, const char *const *cols, size_t n,
                                 const char *where, char *errbuf, size_t errbuf_len)
{
    for (size_t i = 0; i < n; i++) {
        if (dw_col_index(t, cols[i]) < 0) {
            dw_errf(errbuf, errbuf_len, "%s: missing column '%s'", where, cols[i]);
            return ESP_ERR_INVALID_ARG;
        }
    }
    return ESP_OK;
}

static void dw_trim_inplace(char *s)
{
    if (!s) {
        return;
    }
    char *start = s;
    while (*start == ' ' || *start == '\t') {
        start++;
    }
    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
}

static esp_err_t dw_mkdir_p(const char *dir)
{
    char tmp[MINIHUB_DW_MODEL_PATH_MAX];
    size_t n = strnlen(dir, sizeof(tmp) - 1);
    if (n == 0 || n >= sizeof(tmp)) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(tmp, dir, n);
    tmp[n] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') {
            continue;
        }
        *p = '\0';
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
            struct stat st;
            if (stat(tmp, &st) != 0 || !S_ISDIR(st.st_mode)) {
                return ESP_FAIL;
            }
        }
        *p = '/';
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        struct stat st;
        if (stat(tmp, &st) != 0 || !S_ISDIR(st.st_mode)) {
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

static esp_err_t dw_write_file(const char *path, const void *data, size_t len)
{
    char dir[MINIHUB_DW_MODEL_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        if (dw_mkdir_p(dir) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        return ESP_FAIL;
    }
    if (len > 0 && fwrite(data, 1, len, f) != len) {
        fclose(f);
        unlink(path);
        return ESP_FAIL;
    }
    fclose(f);
    return ESP_OK;
}

static esp_err_t dw_read_file(const char *path, char **out, size_t *out_len)
{
    *out = NULL;
    *out_len = 0;
    FILE *f = fopen(path, "rb");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    long sz = ftell(f);
    if (sz < 0 || (size_t)sz > DW_MODEL_MAX_BYTES) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return ESP_FAIL;
    }
    fclose(f);
    buf[sz] = '\0';
    *out = buf;
    *out_len = (size_t)sz;
    return ESP_OK;
}

static void dw_csv_write_field(FILE *f, const char *s, bool force_quote)
{
    if (!s) {
        s = "";
    }
    bool need = force_quote;
    if (!need) {
        for (const char *p = s; *p; p++) {
            if (*p == ',' || *p == '"' || *p == '\n' || *p == '\r') {
                need = true;
                break;
            }
        }
    }
    if (!need) {
        fputs(s, f);
        return;
    }
    fputc('"', f);
    for (const char *p = s; *p; p++) {
        if (*p == '"') {
            fputc('"', f);
        }
        fputc(*p, f);
    }
    fputc('"', f);
}

static cJSON *dw_build_style(const dw_csv_table_t *params, int row, int *cols)
{
    /* cols: parameter,label,...,ui_element,options,...,css,image indices */
    const char *ui = dw_cell(params, row, cols[8]); /* ui_element */
    const char *label = dw_cell(params, row, cols[1]);
    cJSON *style = cJSON_CreateObject();
    if (!style) {
        return NULL;
    }
    cJSON_AddStringToObject(style, "widget", dw_ui_to_widget(ui));
    cJSON_AddStringToObject(style, "label", label ? label : "");
    for (int k = 0; k < 3; k++) {
        const char *keys[] = { "min", "max", "step" };
        const char *v = dw_cell(params, row, cols[4 + k]);
        if (v && v[0]) {
            char *end = NULL;
            double d = strtod(v, &end);
            if (end != v) {
                cJSON_AddNumberToObject(style, keys[k], d);
            }
        }
    }
    const char *unit = dw_cell(params, row, cols[7]);
    if (unit && unit[0]) {
        cJSON_AddStringToObject(style, "unit", unit);
    }
    /* properties "Read" → access R; "Read/Write" → RW (UI defaults when
     * instance props omit access). */
    const char *props = dw_cell(params, row, cols[3]);
    if (props && props[0]) {
        char lower[64];
        size_t n = 0;
        for (; props[n] && n + 1 < sizeof(lower); n++) {
            char c = props[n];
            lower[n] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        }
        lower[n] = '\0';
        if (strstr(lower, "write") != NULL) {
            cJSON_AddStringToObject(style, "access", "RW");
        } else if (strstr(lower, "read") != NULL) {
            cJSON_AddStringToObject(style, "access", "R");
        }
    }
    const char *opts = dw_cell(params, row, cols[9]);
    if (opts && opts[0]) {
        cJSON *o = cJSON_Parse(opts);
        if (o) {
            cJSON_AddItemToObject(style, "options", o);
        }
    }
    const char *css = dw_cell(params, row, cols[12]);
    if (css && css[0]) {
        cJSON_AddStringToObject(style, "css", css);
    }
    if (ui && ui[0]) {
        cJSON_AddStringToObject(style, "ui_element", ui);
    }
    /* Optional. Empty = no precondition. A name is {"all":[name]}.
     * {"all":[...]} AND, {"any":[...]} OR, {"not":[...]} disables while any
     * listed name is on. `not` may accompany all or any. Missing column is fine. */
    {
        int c_req = dw_col_index(params, "requires");
        const char *req = dw_cell(params, row, c_req);
        while (req && (*req == ' ' || *req == '\t')) {
            req++;
        }
        if (req && req[0] == '{') {
            cJSON *obj = cJSON_Parse(req);
            if (obj) {
                cJSON_AddItemToObject(style, "requires", obj);
            }
        } else if (req && req[0]) {
            cJSON_AddStringToObject(style, "requires", req);
        }
    }
    return style;
}

static int dw_find_param_row(const dw_csv_table_t *params, int pcol, const char *name);

static esp_err_t dw_check_requires_name(const dw_csv_table_t *par, int c_p, const char *key,
                                        const char *name, char *errbuf, size_t errbuf_len)
{
    if (!name || !name[0] || strcmp(name, key) == 0 || dw_find_param_row(par, c_p, name) < 0) {
        dw_errf(errbuf, errbuf_len,
                "parameters.csv: '%s' requires unknown parameter '%s'", key, name ? name : "");
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

/* A name, {"all":[...]} (AND), {"any":[...]} (OR), and/or {"not":[...]}.
 * A name means all-of that one. `not` may accompany all or any, not both. */
static esp_err_t dw_check_requires_list(const dw_csv_table_t *par, int c_p, const char *key,
                                        const cJSON *arr, const char **seen, int *seen_n, int seen_max,
                                        char *errbuf, size_t errbuf_len)
{
    if (!cJSON_IsArray(arr) || cJSON_GetArraySize(arr) < 1) {
        dw_errf(errbuf, errbuf_len,
                "parameters.csv: '%s' requires must be a name or {all|any|not} list", key);
        return ESP_ERR_INVALID_ARG;
    }
    int n = cJSON_GetArraySize(arr);
    for (int i = 0; i < n; i++) {
        const cJSON *item = cJSON_GetArrayItem(arr, i);
        const char *name = (cJSON_IsString(item) && item->valuestring) ? item->valuestring : "";
        if (dw_check_requires_name(par, c_p, key, name, errbuf, errbuf_len) != ESP_OK) {
            return ESP_ERR_INVALID_ARG;
        }
        for (int j = 0; j < *seen_n; j++) {
            if (strcmp(seen[j], name) == 0) {
                dw_errf(errbuf, errbuf_len,
                        "parameters.csv: '%s' requires lists '%s' more than once", key, name);
                return ESP_ERR_INVALID_ARG;
            }
        }
        if (*seen_n >= seen_max) {
            dw_errf(errbuf, errbuf_len, "parameters.csv: '%s' requires list is too long", key);
            return ESP_ERR_INVALID_ARG;
        }
        seen[(*seen_n)++] = name;
    }
    return ESP_OK;
}

static esp_err_t dw_check_requires_cell(const dw_csv_table_t *par, int c_p, const char *key,
                                        const char *req, char *errbuf, size_t errbuf_len)
{
    while (req && (*req == ' ' || *req == '\t')) {
        req++;
    }
    if (!req || !req[0]) {
        return ESP_OK;
    }
    if (req[0] != '{') {
        return dw_check_requires_name(par, c_p, key, req, errbuf, errbuf_len);
    }
    cJSON *obj = cJSON_Parse(req);
    if (!cJSON_IsObject(obj) || !obj->child) {
        cJSON_Delete(obj);
        dw_errf(errbuf, errbuf_len,
                "parameters.csv: '%s' requires must be a name or {all|any|not} list", key);
        return ESP_ERR_INVALID_ARG;
    }
    bool has_all = false;
    bool has_any = false;
    bool has_not = false;
    const char *seen[32];
    int seen_n = 0;
    for (const cJSON *ch = obj->child; ch; ch = ch->next) {
        const char *op = ch->string ? ch->string : "";
        if (strcmp(op, "all") == 0) {
            has_all = true;
        } else if (strcmp(op, "any") == 0) {
            has_any = true;
        } else if (strcmp(op, "not") == 0) {
            has_not = true;
        } else {
            cJSON_Delete(obj);
            dw_errf(errbuf, errbuf_len,
                    "parameters.csv: '%s' requires must be a name or {all|any|not} list", key);
            return ESP_ERR_INVALID_ARG;
        }
        if (dw_check_requires_list(par, c_p, key, ch, seen, &seen_n, 32, errbuf, errbuf_len) != ESP_OK) {
            cJSON_Delete(obj);
            return ESP_ERR_INVALID_ARG;
        }
    }
    cJSON_Delete(obj);
    if (has_all && has_any) {
        dw_errf(errbuf, errbuf_len,
                "parameters.csv: '%s' requires cannot set both all and any", key);
        return ESP_ERR_INVALID_ARG;
    }
    if (!has_all && !has_any && !has_not) {
        dw_errf(errbuf, errbuf_len,
                "parameters.csv: '%s' requires must be a name or {all|any|not} list", key);
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static int dw_find_param_row(const dw_csv_table_t *params, int pcol, const char *name)
{
    for (int i = 0; i < params->nrows; i++) {
        if (strcmp(dw_cell(params, i, pcol), name) == 0) {
            return i;
        }
    }
    return -1;
}

static esp_err_t dw_validate_devices(const dw_csv_table_t *dev, char *errbuf, size_t errbuf_len)
{
    esp_err_t err = dw_require_cols(dev, DEVICE_REQ_COLS,
                                    sizeof(DEVICE_REQ_COLS) / sizeof(DEVICE_REQ_COLS[0]),
                                    "devices.csv", errbuf, errbuf_len);
    if (err != ESP_OK) {
        return err;
    }
    int c_id = dw_col_index(dev, "device_id");
    int c_par = dw_col_index(dev, "parameters");
    for (int i = 0; i < dev->nrows; i++) {
        const char *id = dw_cell(dev, i, c_id);
        if (!id || !id[0]) {
            dw_err(errbuf, errbuf_len, "devices.csv: empty device_id");
            return ESP_ERR_INVALID_ARG;
        }
        for (int j = 0; j < i; j++) {
            if (strcmp(dw_cell(dev, j, c_id), id) == 0) {
                dw_errf(errbuf, errbuf_len, "devices.csv: duplicate device_id '%s'", id);
                return ESP_ERR_INVALID_ARG;
            }
        }
        const char *plist = dw_cell(dev, i, c_par);
        bool any = false;
        char *tmp = strdup(plist ? plist : "");
        if (!tmp) {
            return ESP_ERR_NO_MEM;
        }
        for (char *tok = strtok(tmp, ","); tok; tok = strtok(NULL, ",")) {
            dw_trim_inplace(tok);
            if (tok[0]) {
                any = true;
                break;
            }
        }
        free(tmp);
        if (!any) {
            dw_errf(errbuf, errbuf_len, "devices.csv: device '%s' lists no parameters", id);
            return ESP_ERR_INVALID_ARG;
        }
    }
    return ESP_OK;
}

static esp_err_t dw_validate_parameters(const dw_csv_table_t *par, char *errbuf, size_t errbuf_len)
{
    esp_err_t err = dw_require_cols(par, PARAM_REQ_COLS,
                                    sizeof(PARAM_REQ_COLS) / sizeof(PARAM_REQ_COLS[0]),
                                    "parameters.csv", errbuf, errbuf_len);
    if (err != ESP_OK) {
        return err;
    }
    int c_p = dw_col_index(par, "parameter");
    int c_ui = dw_col_index(par, "ui_element");
    for (int i = 0; i < par->nrows; i++) {
        const char *key = dw_cell(par, i, c_p);
        if (!key || !key[0]) {
            dw_err(errbuf, errbuf_len, "parameters.csv: empty parameter");
            return ESP_ERR_INVALID_ARG;
        }
        for (int j = 0; j < i; j++) {
            if (strcmp(dw_cell(par, j, c_p), key) == 0) {
                dw_errf(errbuf, errbuf_len, "parameters.csv: duplicate '%s'", key);
                return ESP_ERR_INVALID_ARG;
            }
        }
        const char *ui = dw_cell(par, i, c_ui);
        if (!dw_ui_ok(ui)) {
            dw_errf(errbuf, errbuf_len,
                    "parameters.csv: '%s' ui_element '%s' not in closed set", key, ui);
            return ESP_ERR_INVALID_ARG;
        }
    }
    int c_req = dw_col_index(par, "requires");
    if (c_req >= 0) {
        for (int i = 0; i < par->nrows; i++) {
            const char *key = dw_cell(par, i, c_p);
            const char *req = dw_cell(par, i, c_req);
            if (!req || !req[0]) {
                continue;
            }
            if (dw_check_requires_cell(par, c_p, key, req, errbuf, errbuf_len) != ESP_OK) {
                return ESP_ERR_INVALID_ARG;
            }
        }
    }
    return ESP_OK;
}

static esp_err_t dw_flatten_to_file(const dw_csv_table_t *dev, const dw_csv_table_t *par,
                                    const char *out_path, char *errbuf, size_t errbuf_len)
{
    int d_id = dw_col_index(dev, "device_id");
    int d_par = dw_col_index(dev, "parameters");
    int d_img = dw_col_index(dev, "image");
    int p_cols[14];
    static const char *pnames[] = {
        "parameter", "label", "data_type", "properties", "min", "max", "step", "unit",
        "ui_element", "options", "description", "visible", "css", "image",
    };
    for (int i = 0; i < 14; i++) {
        p_cols[i] = dw_col_index(par, pnames[i]);
    }

    FILE *f = fopen(out_path, "wb");
    if (!f) {
        /* ensure parent */
        char dir[MINIHUB_DW_MODEL_PATH_MAX];
        snprintf(dir, sizeof(dir), "%s", out_path);
        char *slash = strrchr(dir, '/');
        if (slash) {
            *slash = '\0';
            dw_mkdir_p(dir);
        }
        f = fopen(out_path, "wb");
        if (!f) {
            dw_err(errbuf, errbuf_len, "cannot write runtime devices.csv");
            return ESP_FAIL;
        }
    }
    fputs("device,parameter,style,image,device_image\n", f);

    for (int di = 0; di < dev->nrows; di++) {
        const char *dev_id = dw_cell(dev, di, d_id);
        const char *dev_img = dw_cell(dev, di, d_img);
        char *plist = strdup(dw_cell(dev, di, d_par));
        if (!plist) {
            fclose(f);
            return ESP_ERR_NO_MEM;
        }
        for (char *tok = strtok(plist, ","); tok; tok = strtok(NULL, ",")) {
            dw_trim_inplace(tok);
            if (!tok[0]) {
                continue;
            }
            int pr = dw_find_param_row(par, p_cols[0], tok);
            if (pr < 0) {
                free(plist);
                fclose(f);
                unlink(out_path);
                dw_errf(errbuf, errbuf_len,
                        "parameters.csv: no catalog entry for '%s' (device '%s')",
                        tok, dev_id);
                return ESP_ERR_INVALID_ARG;
            }
            cJSON *style = dw_build_style(par, pr, p_cols);
            if (!style) {
                free(plist);
                fclose(f);
                unlink(out_path);
                return ESP_ERR_NO_MEM;
            }
            char *style_s = cJSON_PrintUnformatted(style);
            cJSON_Delete(style);
            if (!style_s) {
                free(plist);
                fclose(f);
                unlink(out_path);
                return ESP_ERR_NO_MEM;
            }
            const char *pimg = dw_cell(par, pr, p_cols[13]);
            dw_csv_write_field(f, dev_id, false);
            fputc(',', f);
            dw_csv_write_field(f, tok, false);
            fputc(',', f);
            dw_csv_write_field(f, style_s, true);
            fputc(',', f);
            dw_csv_write_field(f, pimg, false);
            fputc(',', f);
            dw_csv_write_field(f, dev_img, false);
            fputc('\n', f);
            free(style_s);
        }
        free(plist);
    }
    fclose(f);
    return ESP_OK;
}

static bool dw_which_ok(const char *which)
{
    return which && (strcmp(which, "devices.csv") == 0 || strcmp(which, "parameters.csv") == 0);
}

static const char *dw_model_path(const char *which)
{
    if (strcmp(which, "devices.csv") == 0) {
        return MINIHUB_DW_MODEL_DEVICES_PATH;
    }
    if (strcmp(which, "parameters.csv") == 0) {
        return MINIHUB_DW_MODEL_PARAMETERS_PATH;
    }
    return NULL;
}

esp_err_t minihub_device_widget_get_model(const char *which, char **out, size_t *out_len)
{
    if (!dw_which_ok(which) || !out || !out_len) {
        return ESP_ERR_INVALID_ARG;
    }
    return dw_read_file(dw_model_path(which), out, out_len);
}

esp_err_t minihub_device_widget_flatten_stored_model(char *errbuf, size_t errbuf_len)
{
    if (errbuf && errbuf_len) {
        errbuf[0] = '\0';
    }
    char *dev_buf = NULL, *par_buf = NULL;
    size_t dev_len = 0, par_len = 0;
    esp_err_t e1 = dw_read_file(MINIHUB_DW_MODEL_DEVICES_PATH, &dev_buf, &dev_len);
    esp_err_t e2 = dw_read_file(MINIHUB_DW_MODEL_PARAMETERS_PATH, &par_buf, &par_len);
    if (e1 == ESP_ERR_NOT_FOUND || e2 == ESP_ERR_NOT_FOUND) {
        free(dev_buf);
        free(par_buf);
        return ESP_ERR_NOT_FOUND;
    }
    if (e1 != ESP_OK || e2 != ESP_OK) {
        free(dev_buf);
        free(par_buf);
        dw_err(errbuf, errbuf_len, "read peer model failed");
        return ESP_FAIL;
    }

    dw_csv_table_t dev_t, par_t;
    esp_err_t err = dw_csv_parse_table(dev_buf, dev_len, &dev_t, errbuf, errbuf_len);
    free(dev_buf);
    if (err != ESP_OK) {
        free(par_buf);
        return err;
    }
    err = dw_csv_parse_table(par_buf, par_len, &par_t, errbuf, errbuf_len);
    free(par_buf);
    if (err != ESP_OK) {
        dw_csv_table_free(&dev_t);
        return err;
    }
    err = dw_validate_devices(&dev_t, errbuf, errbuf_len);
    if (err == ESP_OK) {
        err = dw_validate_parameters(&par_t, errbuf, errbuf_len);
    }
    if (err != ESP_OK) {
        dw_csv_table_free(&dev_t);
        dw_csv_table_free(&par_t);
        return err;
    }

    /* Write flat to temp then rename to avoid half-written corrupt catalog */
    char tmp_path[MINIHUB_DW_MODEL_PATH_MAX];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", MINIHUB_DW_CSV_PATH);
    err = dw_flatten_to_file(&dev_t, &par_t, tmp_path, errbuf, errbuf_len);
    dw_csv_table_free(&dev_t);
    dw_csv_table_free(&par_t);
    if (err != ESP_OK) {
        return err;
    }
    if (rename(tmp_path, MINIHUB_DW_CSV_PATH) != 0) {
        /* littlefs may lack rename — copy */
        char *flat = NULL;
        size_t flat_len = 0;
        err = dw_read_file(tmp_path, &flat, &flat_len);
        unlink(tmp_path);
        if (err != ESP_OK) {
            dw_err(errbuf, errbuf_len, "flatten write failed");
            return err;
        }
        err = dw_write_file(MINIHUB_DW_CSV_PATH, flat, flat_len);
        free(flat);
        if (err != ESP_OK) {
            dw_err(errbuf, errbuf_len, "write runtime devices.csv failed");
            return err;
        }
    }
    return ESP_OK;
}

esp_err_t minihub_device_widget_put_model(const char *which, const void *data, size_t len,
                                          bool *rebuilt, char *errbuf, size_t errbuf_len)
{
    if (rebuilt) {
        *rebuilt = false;
    }
    if (errbuf && errbuf_len) {
        errbuf[0] = '\0';
    }
    if (!dw_which_ok(which) || !data) {
        dw_err(errbuf, errbuf_len, "bad which/body");
        return ESP_ERR_INVALID_ARG;
    }
    if (len == 0 || len > DW_MODEL_MAX_BYTES) {
        dw_err(errbuf, errbuf_len, "body empty or too large");
        return ESP_ERR_INVALID_SIZE;
    }

    dw_csv_table_t tbl;
    esp_err_t err = dw_csv_parse_table((const char *)data, len, &tbl, errbuf, errbuf_len);
    if (err != ESP_OK) {
        return err;
    }
    if (strcmp(which, "devices.csv") == 0) {
        err = dw_validate_devices(&tbl, errbuf, errbuf_len);
    } else {
        err = dw_validate_parameters(&tbl, errbuf, errbuf_len);
    }
    dw_csv_table_free(&tbl);
    if (err != ESP_OK) {
        return err;
    }

    err = dw_write_file(dw_model_path(which), data, len);
    if (err != ESP_OK) {
        dw_err(errbuf, errbuf_len, "write model file failed");
        return err;
    }
    ESP_LOGI(TAG, "stored model %s (%u bytes)", which, (unsigned)len);

    err = minihub_device_widget_flatten_stored_model(errbuf, errbuf_len);
    if (err == ESP_ERR_NOT_FOUND) {
        dw_err(errbuf, errbuf_len,
               "stored; upload the other model file to flatten+reload");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    err = minihub_device_widget_reload(MINIHUB_DW_CSV_PATH);
    if (err != ESP_OK) {
        dw_err(errbuf, errbuf_len, "reload failed");
        return err;
    }
    if (rebuilt) {
        *rebuilt = true;
    }
    ESP_LOGI(TAG, "catalog flattened+reloaded (%u devices)",
             (unsigned)minihub_device_widget_device_count());
    return ESP_OK;
}
