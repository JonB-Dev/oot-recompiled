// Unmatched frames still need complete transforms for tracing at the original rate.
#include "hle/rt64_workload_queue.h"
#include "hle/rt64_game_frame.h"
#include "render/rt64_transform_processor.h"
#include <cstdio>
#include <cstring>
#include <memory>

int main() {
    auto queue = std::make_unique<RT64::WorkloadQueue>();
    auto &data = queue->workloads[0].drawData;
    data.worldTransforms.assign(2, interop::float4x4::identity());
    RT64::GameFrame current, previous;
    current.workloads.push_back(0);
    current.frameMap.workloads.resize(1);
    current.frameMap.workloads[0].transforms.resize(2);
    RT64::TransformProcessor processor;
    RT64::TransformProcessor::ProcessParams params;
    params.workloadQueue = queue.get();
    params.curFrame = &current;
    for (int mode : {0, 1, 2}) {
        params.prevFrame = mode ? &previous : nullptr;
        current.frameMap.workloads[0].mapped = (mode == 2);
        processor.process(params);
        if (data.lerpWorldTransforms.size() != 2 || data.prevWorldTransforms.size() != 2 ||
            data.invTWorldTransforms.size() != 2) {
            std::fprintf(stderr, "Unmatched traced frame has missing transform buffers (mode=%d)\n", mode);
            return 1;
        }
        const size_t bytes = 2 * sizeof(interop::float4x4);
        if (std::memcmp(data.lerpWorldTransforms.data(), data.worldTransforms.data(), bytes) ||
            std::memcmp(data.prevWorldTransforms.data(), data.worldTransforms.data(), bytes)) {
            std::fprintf(stderr, "Unmatched transform fallback changed the world matrices\n");
            return 1;
        }
    }
    std::puts("UNMATCHED_TRACING_TRANSFORMS_OK (CPU only)");
}
