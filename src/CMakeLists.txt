# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)

idf_component_register(
	SRCS ${app_sources}
	PRIV_REQUIRES nvs_flash esp_driver_gpio esp_driver_i2c ieee802154
)
