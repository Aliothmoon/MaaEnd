# 核对 preprocess.onnx metadata 的 definition_hash 与 C++ 的 kPreprocessDefinitionHash；onnx 不存在时跳过。
# 匹配的字节：StringStringEntryProto 0x0A 0x0F "definition_hash" 0x12 0x40 <64 字节哈希>
function(maaend_check_camera_orientation_definition header onnx)
    file(STRINGS "${header}" hash_line REGEX "kPreprocessDefinitionHash = \"[0-9a-f]+\"")
    string(REGEX MATCH "\"([0-9a-f]+)\"" quoted "${hash_line}")
    set(expected "${CMAKE_MATCH_1}")
    string(LENGTH "${expected}" expected_length)
    if(NOT expected_length EQUAL 64)
        message(FATAL_ERROR "CameraOrientation: cannot read kPreprocessDefinitionHash from ${header}")
    endif()

    if(NOT EXISTS "${onnx}")
        message(STATUS "CameraOrientation: ${onnx} not found, definition_hash check skipped")
        return()
    endif()

    file(READ "${onnx}" onnx_hex HEX)
    string(HEX "definition_hash" key_hex)
    string(HEX "${expected}" value_hex)
    string(FIND "${onnx_hex}" "0a0f${key_hex}1240${value_hex}" found)
    if(found EQUAL -1)
        message(
            FATAL_ERROR
                "CameraOrientation: ${onnx} does not carry definition_hash ${expected} "
                "(kPreprocessDefinitionHash in ${header}). Sync the C++ port from minimap-camera-orientation "
                "(cpp/tools/sync_maaend.py), or point assets/resource/model back to the matching commit.")
    endif()
    message(STATUS "CameraOrientation: preprocess definition_hash ${expected} matches ${onnx}")
endfunction()
