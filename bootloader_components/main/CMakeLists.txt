idf_component_register(SRCS "bootloader_start.c" "boot_support_function.c"
                       INCLUDE_DIRS "."
                    REQUIRES bootloader bootloader_support hal esp_hal_uart)

# Use the default linker scripts
idf_build_get_property(scripts BOOTLOADER_LINKER_SCRIPT)
target_linker_script(${COMPONENT_LIB} INTERFACE "${scripts}")