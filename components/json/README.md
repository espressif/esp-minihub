# json

Empty IDF component: no sources, only `REQUIRES espressif__cjson`.

ESP-IDF 6.x dropped the built-in `json` wrapper around cJSON. esp-matter (and
any code that still `REQUIRES json`) looks for that name. This directory keeps
the old component id so those targets link, and pulls cJSON from the managed
`espressif/cjson` package declared on `cmake/minihub_bundle`.

Do not add application JSON helpers here; use `espressif/cjson` or MiniHub
headers directly.
