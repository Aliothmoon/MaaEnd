#include "NcnnModel.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <system_error>
#include <tuple>

#include <MaaUtils/Logger.h>
#include <MaaUtils/Platform.h>

#ifdef MAAEND_WITH_NCNN
#include <ncnn/net.h>
#endif

namespace maplocator
{

std::optional<NcnnModelFiles> FindNcnnModel(const std::filesystem::path& onnx_path)
{
    NcnnModelFiles files { .param = onnx_path, .bin = onnx_path };
    files.param.replace_extension(".ncnn.param");
    files.bin.replace_extension(".ncnn.bin");
    std::error_code ec;
    if (!std::filesystem::exists(files.param, ec) && !std::filesystem::exists(files.bin, ec)) {
        return std::nullopt;
    }
    return files;
}

#ifdef MAAEND_WITH_NCNN

struct NcnnModel::Impl
{
    ncnn::Net net;
    std::string input_name;
    std::string output_name;
};

NcnnModel::NcnnModel()
    : impl_(std::make_unique<Impl>())
{
}

NcnnModel::~NcnnModel() = default;

bool NcnnModel::IsAvailable()
{
    return true;
}

std::unique_ptr<NcnnModel> NcnnModel::Load(const NcnnModelFiles& files, int threads, const char* tag)
{
    const std::string param = MAA_NS::path_to_utf8_string(files.param);
    const std::string bin = MAA_NS::path_to_utf8_string(files.bin);

    std::unique_ptr<NcnnModel> model(new NcnnModel());
    ncnn::Option& opt = model->impl_->net.opt;
    opt.use_vulkan_compute = false;
    // fp32：概率输出要与 onnxruntime 对齐（下游有置信度阈值与次峰门限）。
    opt.use_fp16_packed = false;
    opt.use_fp16_storage = false;
    opt.use_fp16_arithmetic = false;
    opt.use_bf16_storage = false;
    opt.num_threads = std::max(1, threads);

    if (model->impl_->net.load_param(param.c_str()) != 0 || model->impl_->net.load_model(bin.c_str()) != 0) {
        LogError << "ncnn model is incomplete or failed to load" << VAR(tag) << VAR(param) << VAR(bin);
        return nullptr;
    }

    const auto& inputs = model->impl_->net.input_names();
    const auto& outputs = model->impl_->net.output_names();
    if (inputs.size() != 1 || outputs.size() != 1) {
        LogError << "ncnn model must have exactly one input and one output" << VAR(tag) << VAR(inputs.size()) << VAR(outputs.size());
        return nullptr;
    }
    model->impl_->input_name = inputs.front();
    model->impl_->output_name = outputs.front();

    LogInfo << "ncnn model loaded" << VAR(tag) << VAR(param) << VAR(opt.num_threads);
    return model;
}

bool NcnnModel::Run(const float* chw, int channels, int height, int width, std::vector<float>& out)
{
    // ncnn::Mat 的通道按 cstep 对齐，逐通道拷贝而不是直接包装外部内存。
    ncnn::Mat input(width, height, channels);
    const size_t plane = static_cast<size_t>(width) * static_cast<size_t>(height);
    for (int c = 0; c < channels; ++c) {
        std::memcpy(input.channel(c), chw + static_cast<size_t>(c) * plane, plane * sizeof(float));
    }

    ncnn::Extractor extractor = impl_->net.create_extractor();
    if (extractor.input(impl_->input_name.c_str(), input) != 0) {
        return false;
    }
    ncnn::Mat output;
    if (extractor.extract(impl_->output_name.c_str(), output) != 0 || output.empty()) {
        return false;
    }

    const ncnn::Mat flat = output.reshape(output.w * output.h * output.d * output.c);
    const float* data = flat;
    out.assign(data, data + flat.w);
    return true;
}

#else

struct NcnnModel::Impl
{
};

NcnnModel::NcnnModel() = default;

NcnnModel::~NcnnModel() = default;

bool NcnnModel::IsAvailable()
{
    return false;
}

std::unique_ptr<NcnnModel> NcnnModel::Load(const NcnnModelFiles& files, int threads, const char* tag)
{
    std::ignore = files;
    std::ignore = threads;
    LogError << "cpp-algo built without ncnn" << VAR(tag);
    return nullptr;
}

bool NcnnModel::Run(const float* chw, int channels, int height, int width, std::vector<float>& out)
{
    std::ignore = chw;
    std::ignore = channels;
    std::ignore = height;
    std::ignore = width;
    out.clear();
    return false;
}

#endif

} // namespace maplocator
