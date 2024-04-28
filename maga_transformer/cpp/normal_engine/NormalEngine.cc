#include "maga_transformer/cpp/normal_engine/NormalExecutor.h"
#include "maga_transformer/cpp/normal_engine/NormalEngine.h"
#include "maga_transformer/cpp/normal_engine/NormalBatchStreamProcessor.h"
#include "maga_transformer/cpp/common/status_util.h"
#include "maga_transformer/cpp/schedulers/FIFOScheduler.h"
#include "maga_transformer/cpp/cache/CacheConfigCreator.h"
#include "maga_transformer/cpp/ptuning/PtuningConstructor.h"
#include "src/fastertransformer/core/Types.h"
#include "src/fastertransformer/utils/logger.h"

using namespace std;

namespace rtp_llm {

NormalEngine::NormalEngine(const MagaInitParams&                                                   params,
                           const std::vector<std::unordered_map<std::string, ft::ConstBufferPtr>>& layer_weights,
                           const std::unordered_map<std::string, ft::ConstBufferPtr>&              weights) : params_(params) {
    executor_.reset(new NormalExecutor(params, layer_weights, weights));
    initCacheManager();
    scheduler_.reset(new FIFOScheduler(params, resource_context_.cache_manager));
    (void)startLoop();
}

NormalEngine::~NormalEngine() {
    FT_LOG_INFO("destory normal engine");
    (void)stop();
}

void NormalEngine::initCacheManager() {
    auto [success, cache_config] = CacheConfigCreator::createConfig(*params_.gpt_init_parameter);
    if (!success) {
        // TODO(xinfei.sxf) fix abort
        FT_LOG_ERROR("create cache config failed");
        abort();
    }

    auto device = ft::DeviceFactory::getDevice(ft::DeviceType::Cuda);
    resource_context_.cache_manager = make_shared<CacheManager>(cache_config, device);  
}

void NormalEngine::initPtuning() {
    // TODO(xinfei.sxf) deal with old option : USE_BLOCK_CACHE 
    char* reuse_cache_env  = std::getenv("REUSE_CACHE");
    if (reuse_cache_env && strcmp(reuse_cache_env, "1") == 0) {
        resource_context_.reuse_cache = true;
    }
    auto ptuning_param = PtuningConstructor::construct(*params_.gpt_init_parameter, this, resource_context_.cache_manager.get());
    if (!ptuning_param.empty()) {
        resource_context_.reuse_cache = true;
        resource_context_.ptuning.reset(new MultiTaskPtuning(*resource_context_.cache_manager, ptuning_param));
    }
}

void NormalEngine::addLoRA(const int64_t                                                   lora_id,
                           const std::vector<std::unordered_map<std::string, ft::ConstBufferPtr>>& lora_a_weights,
                           const std::vector<std::unordered_map<std::string, ft::ConstBufferPtr>>& lora_b_weights) {
    executor_->addLoRA(lora_id, lora_a_weights, lora_b_weights);
}

void NormalEngine::removeLoRA(const int64_t lora_id) {
    executor_->removeLoRA(lora_id);
}

absl::Status NormalEngine::startLoop() {
    FT_LOG_INFO("start normal engine");
    running_ = true;
    loop_thread_ = std::thread(&NormalEngine::loop, this);
    initPtuning(); // ptuning constructor depends on engine startup
    return absl::OkStatus();
}

absl::Status NormalEngine::stop() {
    FT_LOG_INFO("stop normal engine");
    running_ = false;
    RETURN_IF_STATUS_ERROR(scheduler_->stop());
    if (loop_thread_.joinable()) {
        loop_thread_.join();
    }
    return absl::OkStatus();
}

void NormalEngine::loop() {
    FT_LOG_INFO("loop begin");
    while (running_) {
        auto status = step();
        if (!status.ok()) {
            FT_LOG_ERROR("step running error: %s", status.ToString().c_str());
            THROW_IF_STATUS_ERROR(trySaveStepError());
        }
    }
}

absl::Status NormalEngine::trySaveStepError() const {
    return absl::UnimplementedError("can not save yet!");
}

absl::Status NormalEngine::enqueue(std::shared_ptr<GenerateStream>& stream) {
    FT_LOG_DEBUG("enqueue stream: %s", stream->debugString().c_str());
    return scheduler_->enqueue(stream);
}

absl::Status NormalEngine::step() {
    FT_LOG_DEBUG(__PRETTY_FUNCTION__);
    const auto streams_status = scheduler_->schedule();
    RETURN_IF_STATUS_OR_ERROR(streams_status);
    const auto& streams = streams_status.value();
    if (streams.empty()) {
        FT_LOG_WARNING("no query run and sleep");
        return absl::OkStatus();
    }
    return executor_->process(streams);
}

}  // namespace rtp_llm
