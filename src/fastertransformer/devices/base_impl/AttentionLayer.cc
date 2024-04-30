#include "src/fastertransformer/devices/DeviceBase.h"
#include "src/fastertransformer/devices/utils/DebugUtils.h"

#include <numeric>

using namespace std;

namespace fastertransformer {

AttentionLayerOutput DeviceBase::attentionLayer(const AttentionLayerParams& params) {
    const auto &input = params.input;
    const auto &input_lengths = params.common.input_lengths;
    const auto &sequence_lengths = params.common.sequence_lengths;

    const auto &qkv_weight = params.weights.qkv_weight;
    const auto &output_weight = params.weights.output_weight;

    const auto &kv_cache_blocks = params.common.kv_cache_blocks;
    if (kv_cache_blocks.has_value()) {
        const auto &shape = kv_cache_blocks.value().get().shape();
        RUNTIME_ASSERT_OP_ARG(
            ((shape.size() == 3) && (shape[0] == input_lengths.shape()[0]) && (shape[1] == 2)),
            "kv_cache_blocks shape in attention layer should be [batch_size, 2, block_length]"
            ", but got %s", kv_cache_blocks.value().get().debugString().c_str());
    }

    // typically local_head_num * size_per_head
    const auto qkv_hidden_size = output_weight->kernel->shape()[0];
    // typically local_head_num * size_per_head + 2 * local_head_num_kv * size_per_head
    const auto qkv_merged_size = qkv_weight->kernel->shape()[1];

    if (qkv_weight->bias) {
        FT_LOG_WARNING("AttentionLayer bias is not supported yet, ignored");
    }
    const auto qkv = gemm({input, *(qkv_weight->kernel)});
    printBufferData(input, "qkv input");
    printBufferData(*(qkv_weight->kernel), "qkv kernel");
    printBufferData(*qkv, "qkv");

    const auto generate_batch_size = sequence_lengths.shape()[0];
    const auto context_batch_size = input_lengths.shape()[0] - generate_batch_size;

    const auto context_token_num = std::accumulate(
        input_lengths.data<int32_t>(), input_lengths.data<int32_t>() + context_batch_size, 0);
    const auto h_token_num = context_token_num + generate_batch_size;

    const auto qkv_output = allocateBuffer({input.type(), {h_token_num, qkv_hidden_size}});

    auto generate_qkv = qkv->view(0, generate_batch_size);
    auto generate_output = qkv_output->view(0, generate_batch_size);
    auto generate_kv_blocks = kv_cache_blocks
        ? kv_cache_blocks.value().get().view(0, generate_batch_size)
        : Buffer::emptyBuffer();
    auto context_qkv = qkv->view(generate_batch_size, context_token_num);
    auto context_output = qkv_output->view(generate_batch_size, context_token_num);
    auto context_kv_blocks = kv_cache_blocks
        ? kv_cache_blocks.value().get().view(generate_batch_size, context_batch_size)
        : Buffer::emptyBuffer();

    if (generate_batch_size) {
        if (kv_cache_blocks) {
            params.common.kv_cache_blocks = generate_kv_blocks;
        }
        decoderSelfAttention({generate_qkv, generate_output, params.common, params.weights, params.configs});
    }
    if (context_batch_size) {
        if (kv_cache_blocks) {
            params.common.kv_cache_blocks = context_kv_blocks;
        }
        contextAttention({context_qkv, context_output, params.common, params.weights, params.configs});
    }
    printBufferData(*qkv_output, "qkv_output");

    auto output = gemm({*qkv_output, *(output_weight->kernel)});

    return {move(output)};
}

}; // namespace fastertransformer
