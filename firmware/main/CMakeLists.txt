idf_component_register(SRCS "main.c" "rf_s3.c" "sym_player.c" "cam_bench.c" "beacon_cfg.c" "web_ui.c" "dvbs2_enc.c" "s2_test.c" "ts_mux.c" "live_tx.c" "overlay.c" "cam_live.c" "media_store.c"
                       EMBED_TXTFILES "web/index.html"
                       REQUIRES esp_http_server esp_netif esp_driver_gpio esp_partition esp_phy esp_wifi nvs_flash esp_rom esp_timer esp_driver_uart esp_hw_support)
