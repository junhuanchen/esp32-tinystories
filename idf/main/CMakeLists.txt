set(ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/../..")
set(MODEL_DIR "${ROOT_DIR}/artifacts/tinystories")
set(TOKENIZER "${MODEL_DIR}/tokenizer.json")
set(MODEL_BIN "${MODEL_DIR}/model.bin")
set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")

if(NOT EXISTS "${TOKENIZER}" OR NOT EXISTS "${MODEL_BIN}")
    message(FATAL_ERROR
        "TinyStories artifacts are missing. Run scripts/fetch_model.ps1 -ModelKind tinystories")
endif()

find_program(UV_EXECUTABLE uv REQUIRED)
set(VOCAB_HEADER "${GENERATED_DIR}/vocab.h")
set(ENCODER_HEADER "${GENERATED_DIR}/tokenizer_encoder.h")
add_custom_command(
    OUTPUT "${VOCAB_HEADER}" "${ENCODER_HEADER}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${GENERATED_DIR}"
    COMMAND "${UV_EXECUTABLE}" run --no-project --with tokenizers==0.23.1
            python "${ROOT_DIR}/firmware/esp32_tinystories/tools/generate_vocab.py"
            --tokenizer "${TOKENIZER}" --out "${VOCAB_HEADER}"
    COMMAND "${UV_EXECUTABLE}" run --no-project python
            "${ROOT_DIR}/firmware/esp32_barista/tools/generate_tokenizer_header.py"
            --tokenizer "${TOKENIZER}" --out "${ENCODER_HEADER}"
    DEPENDS "${TOKENIZER}"
            "${ROOT_DIR}/firmware/esp32_tinystories/tools/generate_vocab.py"
            "${ROOT_DIR}/firmware/esp32_barista/tools/generate_tokenizer_header.py"
    COMMENT "Generating TinyStories tokenizer headers"
    VERBATIM)
add_custom_target(tinystories_headers DEPENDS "${VOCAB_HEADER}" "${ENCODER_HEADER}")

idf_component_register(SRCS "main.cpp"
                       INCLUDE_DIRS "." "${CMAKE_CURRENT_BINARY_DIR}" "${ROOT_DIR}/runtime"
                       REQUIRES esp_partition esp_timer esp_psram driver)
add_dependencies(${COMPONENT_LIB} tinystories_headers)

# Make `idf.py flash` write the exact verified binary to the partition that the
# runtime memory-maps. No separate esptool command is needed.
esptool_py_flash_to_partition(flash "model" "${MODEL_BIN}")
