idf_component_register(
    SRCS "main.c"
         "xhs_logic.c"
         "xhs_ui.c"
         "xhs_store.c"
         "xhs_net.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_event esp_http_client esp_http_server esp_netif esp_wifi nvs_flash
)

# 字库放在 assets/fonts，不搬进 main/。显式加入组件，避免源码必须位于组件目录内的限制。
target_sources(${COMPONENT_LIB} PRIVATE "${CMAKE_SOURCE_DIR}/assets/fonts/xhs_font_16.c")
