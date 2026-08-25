idf_component_register(
    SRCS
        "minihub_core.c"
        "core_task.c"
        "heap_monitor.c"
        "point_registry.c"
        "state_store.c"
        "point_history.c"
        "event_bus.c"
        "command_router.c"
        "http_worker.c"
    INCLUDE_DIRS "include"
    REQUIRES espressif__cjson esp_timer freertos esp_system
)
