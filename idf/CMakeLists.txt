cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_tinystories)

# project() completes the partition-table configuration. Register the model
# image only afterwards, otherwise esptool cannot resolve its offset.
set(MODEL_BIN "${CMAKE_CURRENT_LIST_DIR}/../artifacts/tinystories/model.bin")
if(NOT EXISTS "${MODEL_BIN}")
    message(FATAL_ERROR
        "TinyStories model is missing. Run scripts/fetch_model.ps1 -ModelKind tinystories")
endif()
esptool_py_flash_to_partition(flash "model" "${MODEL_BIN}")
