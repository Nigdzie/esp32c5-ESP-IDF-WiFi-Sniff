idf_build_get_property(project_dir PROJECT_DIR)

spiffs_create_partition_image(spiffs
  "${project_dir}/spiffs_image"
  FLASH_IN_PROJECT
)