#pragma once
#include <assert.h>
#include <cstdint>
#include <mutex>
#include <string>
#include <optional>
#include <queue>
#include <condition_variable>
#include "src/fastertransformer/core/Buffer.h"
#include "maga_transformer/cpp/utils/TimeUtility.h"
#include "maga_transformer/cpp/embedding_engine/EmbeddingQuery.h"
#include "maga_transformer/cpp/dataclass/StreamCacheResource.h"
#include "maga_transformer/cpp/cache/CacheManager.h"
#include "maga_transformer/cpp/utils/SynchronizedQueue.h"
#include "maga_transformer/cpp/system_prompt/SystemPrompt.h"
#include "src/fastertransformer/devices/utils/BufferUtils.h"
#include "absl/status/statusor.h"

namespace ft = fastertransformer;

namespace rtp_llm {

class EmbeddingStream {
public:
    EmbeddingStream(const std::shared_ptr<EmbeddingInput>& query);
    ~EmbeddingStream() {        }

public:
    // Exported to python world.
    std::shared_ptr<EmbeddingInput> embeddingInput() const;
    std::shared_ptr<EmbeddingOutput> embeddingOutput() const;

    int64_t inputLength() const;

    int64_t streamId() const;

    int64_t batchSize() const;

    void updateOutput(ft::BufferPtr& output);

    void waitFinish();

    void setError(const std::string& error_info);

    std::string debugString() const {
        std::stringstream debug_string;
        debug_string << "EmbeddingStream {"
                    //  << "generate_input:" << generate_input_->debugString()
                    //  << ", max_seq_len:" << max_seq_len_
                    //  << ", input_length:" << inputLength()
                    //  << ", seq_length:" << seq_length_
                    //  << ", reuse_length:" << reuse_length_
                    //  << ", batch_size:" << batch_size_
                     << "}";
        return debug_string.str();
    }

protected:
    ft::DeviceBase* device_;
    std::shared_ptr<EmbeddingInput>      embedding_input_;
    std::shared_ptr<EmbeddingOutput>     embedding_output_;
    int64_t                              begin_time_;
    std::condition_variable              cond_;
    std::mutex                           lock_;
    GenerateState                        generate_state_;
};

typedef std::shared_ptr<EmbeddingStream> EmbeddingStreamPtr;
} // namespace rtp_llm
