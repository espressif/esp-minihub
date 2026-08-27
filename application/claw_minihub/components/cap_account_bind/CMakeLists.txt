idf_component_register(
    SRCS "cap_account_bind.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES claw_cap espressif__cjson minihub_net
)
