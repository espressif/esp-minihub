# IDF 6.x removed the built-in `json` component (cJSON wrapper). esp-matter still
# REQUIRES `json`; this shim forwards to the managed espressif/cjson package.
idf_component_register(
    REQUIRES espressif__cjson
)
