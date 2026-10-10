#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace maplocator
{

// 地图模型的 ncnn 版本与 onnx 同目录同名：<stem>.ncnn.param + <stem>.ncnn.bin。
struct NcnnModelFiles
{
    std::filesystem::path param;
    std::filesystem::path bin;
};

// 两份 ncnn 文件都不存在时返回 std::nullopt（该模型走 onnxruntime）；只要有一份存在就返回路径，
// 由 NcnnModel::Load 对缺失的那份报错，不回退 onnxruntime。
std::optional<NcnnModelFiles> FindNcnnModel(const std::filesystem::path& onnx_path);

// 单输入单输出的 ncnn 推理（CPU、fp32，与 onnxruntime 的数值对齐）。
// 仅编入 ncnn 的构建（MAAEND_WITH_NCNN，Android）可用；其余构建 IsAvailable() 为 false。
class NcnnModel
{
public:
    static bool IsAvailable();
    static std::unique_ptr<NcnnModel> Load(const NcnnModelFiles& files, int threads, const char* tag);

    ~NcnnModel();

    // chw：channels × height × width 连续 float32；out：输出张量展平。
    bool Run(const float* chw, int channels, int height, int width, std::vector<float>& out);

private:
    NcnnModel();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace maplocator
