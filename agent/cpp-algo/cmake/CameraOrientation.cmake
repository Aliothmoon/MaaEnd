# 摄像机朝向前处理的定义版本核对（构建期）。
#
# 前处理由 MapLocator/CameraOrientationPreprocess.cpp 的 C++ 等价实现执行，preprocess.onnx 不在运行时加载，
# 只在这里用来核对版本：图 metadata 里的 definition_hash 必须等于 C++ 的 kPreprocessDefinitionHash，
# 否则条带与分类器（polar_with_ref.onnx）训练时的输入不一致，配置直接失败。
#
# 不一致时二选一：
#   - 新交付的模型换了定义：在 minimap-camera-orientation 更新 cpp/src 后用 cpp/tools/sync_maaend.py 同步过来；
#   - 只是模型子模块指错了版本：把 assets/resource/model 换回与 C++ 对应的提交。
#
# 模型子模块未检出时（如 Android agents 构建只初始化 MaaUtils）跳过核对。

# metadata_props 条目是 StringStringEntryProto { key = 1, value = 2 }，按字节匹配
#   0x0A 0x0F "definition_hash" 0x12 0x40 <64 字节十六进制哈希>
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
                "(kPreprocessDefinitionHash in ${header}). The preprocess definition and its C++ port are out of sync; "
                "see agent/cpp-algo/cmake/CameraOrientation.cmake.")
    endif()
    message(STATUS "CameraOrientation: preprocess definition_hash ${expected} matches ${onnx}")
endfunction()
