file(GLOB UI_FONT_SRCS "../../app/ui/fonts/*.c")

idf_component_register(
    SRCS
        "main.c"
        "app_config.c"
        "charge.c"
        "debug_log.c"
        "feedback.c"
        "serial_input.c"
        "rtc_time.c"
        "web_server.c"
        "wifi_manager.c"
        "usb_hid_input.c"
        "ulanzi_d100h.c"
        "usb_host_core.c"
        "usbmux_host.c"
        "lockdown_client.c"
        "afc_client.c"
        "thumb_cache.c"
        "star_store.c"
        "iap2_identity.c"
        "iap2_auth.c"
        "iap2_host.c"
        "lvgl_ppa_idf_compat.c"
        "../../app/backend/mock_player.c"
        "../../app/backend/ble_remote.c"
        "../../app/ui/ui.c"
        "../../app/ui/boot.c"
        ${UI_FONT_SRCS}
    INCLUDE_DIRS
        "."
        "../../app/backend"
        "../../app/ui"
        "../managed_components/lvgl__lvgl/src/libs/tjpgd"
    REQUIRES
        bt
        esp_hosted
        json
        spiffs
        esp_codec_dev
        esp_http_server
        esp_http_client
        esp_driver_jpeg
        app_update
        esp_wifi
        esp_netif
        esp_event
        nvs_flash
        mbedtls
)

# esp_cache_get_line_size_by_addr() is only referenced by liblvgl, which comes
# after libmain on the link line. Keep the implementation linked in.
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--undefined=esp_cache_get_line_size_by_addr")

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    # Generated LVGL fonts use "lvgl.h" with this switch instead of the
    # desktop-only "lvgl/lvgl.h" include path.
    LV_LVGL_H_INCLUDE_SIMPLE=1
)
