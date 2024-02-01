#pragma once
#include "src/fastertransformer/core/allocator_cuda.h"
#include "torch/extension.h"
#include "src/fastertransformer/utils/logger.h"
#include <unordered_map>
#include <vector>

namespace fastertransformer {

template<>
class Allocator<AllocatorType::TH>: public ICudaAllocator, public TypedAllocator<AllocatorType::TH> {
    std::unordered_map<void*, torch::Tensor>* pointer_mapping_;

    bool isExist(void* address) const {
        return pointer_mapping_->count(address) > 0;
    }
    ReallocType isReMalloc(void* address, size_t size) const {
        FT_CHECK(isExist(address));
        size_t current_buffer_size = 1;
        for (int i = 0; i < pointer_mapping_->at(address).dim(); i++) {
            current_buffer_size *= pointer_mapping_->at(address).size(i);
        }
        FT_LOG_DEBUG(
            "current_buffer_size: %d, original buffer: %p, new buffer: %d", current_buffer_size, address, size);
        if (current_buffer_size < size) {
            return ReallocType::INCREASE;
        } else if (current_buffer_size == size) {
            return ReallocType::REUSE;
        } else {
            return ReallocType::DECREASE;
        }
    }

public:
    Allocator();

    void* malloc(size_t size, const bool is_set_zero = true, bool is_host = false);
    void free(void** ptr, bool is_host = false) const;

    virtual ~Allocator();

    void memSet(void* ptr, const int val, const size_t size) {
        check_cuda_error(cudaMemsetAsync(ptr, val, size, stream_));
    }
};

}
