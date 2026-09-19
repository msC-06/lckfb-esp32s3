# app 组件：上层应用业务 / LVGL 界面
# 只依赖 my_drivers 暴露的外设接口，不直接依赖 bsp 底层外设
idf_component_register(
    SRCS
        "app_ui.c"
        "app_splash.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        my_drivers
        esp_lvgl_port
)
