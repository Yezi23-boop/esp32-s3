idf_component_register(
    SRCS "touch_ft5x06.c"
    INCLUDE_DIRS "include"
    REQUIRES driver co5300_panel i2c_manager
)
