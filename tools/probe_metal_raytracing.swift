#!/usr/bin/env swift
// Small, headless proof that this Mac can build and query a Metal acceleration structure.
// It does not open a window or launch OoT.
import Foundation
import Metal
import simd

enum ProbeError: Error {
    case unavailable(String)
    case wrongResult(Float)
}

guard let device = MTLCreateSystemDefaultDevice() else {
    throw ProbeError.unavailable("No Metal device")
}
guard device.supportsRaytracing else {
    throw ProbeError.unavailable("\(device.name) does not support Metal ray tracing")
}
guard let queue = device.makeCommandQueue() else {
    throw ProbeError.unavailable("Could not create a command queue")
}

// A triangle in the z=0 plane; the test ray starts at z=1 and points toward it.
let vertices = [
    SIMD3<Float>(-1, -1, 0),
    SIMD3<Float>( 1, -1, 0),
    SIMD3<Float>( 0,  1, 0),
]
let vertexBuffer = vertices.withUnsafeBytes {
    device.makeBuffer(bytes: $0.baseAddress!, length: $0.count, options: .storageModeShared)
}
guard let vertexBuffer else {
    throw ProbeError.unavailable("Could not allocate triangle vertices")
}
let geometry = MTLAccelerationStructureTriangleGeometryDescriptor()
geometry.vertexBuffer = vertexBuffer
geometry.vertexStride = MemoryLayout<SIMD3<Float>>.stride
geometry.triangleCount = 1
let descriptor = MTLPrimitiveAccelerationStructureDescriptor()
descriptor.geometryDescriptors = [geometry]
let sizes = device.accelerationStructureSizes(descriptor: descriptor)
guard let structure = device.makeAccelerationStructure(size: sizes.accelerationStructureSize),
      let scratch = device.makeBuffer(length: sizes.buildScratchBufferSize, options: .storageModePrivate),
      let result = device.makeBuffer(length: MemoryLayout<Float>.size, options: .storageModeShared) else {
    throw ProbeError.unavailable("Could not allocate ray tracing resources")
}

let source = """
#include <metal_stdlib>
#include <metal_raytracing>
using namespace metal;
using namespace raytracing;
kernel void probe(primitive_acceleration_structure scene [[buffer(0)]],
                  device float *result [[buffer(1)]]) {
    ray r;
    r.origin = float3(0.0, 0.0, 1.0);
    r.direction = float3(0.0, 0.0, -1.0);
    r.min_distance = 0.0;
    r.max_distance = 10.0;
    intersector<triangle_data> query;
    result[0] = query.intersect(r, scene).distance;
}
"""
let library = try device.makeLibrary(source: source, options: nil)
guard let function = library.makeFunction(name: "probe") else {
    throw ProbeError.unavailable("Could not find ray query kernel")
}
let pipeline = try device.makeComputePipelineState(function: function)
guard let commands = queue.makeCommandBuffer(),
      let builder = commands.makeAccelerationStructureCommandEncoder() else {
    throw ProbeError.unavailable("Could not encode acceleration structure build")
}
builder.build(accelerationStructure: structure, descriptor: descriptor,
              scratchBuffer: scratch, scratchBufferOffset: 0)
builder.endEncoding()
guard let compute = commands.makeComputeCommandEncoder() else {
    throw ProbeError.unavailable("Could not encode ray query")
}
compute.setComputePipelineState(pipeline)
compute.setAccelerationStructure(structure, bufferIndex: 0)
compute.setBuffer(result, offset: 0, index: 1)
compute.dispatchThreads(MTLSize(width: 1, height: 1, depth: 1),
                        threadsPerThreadgroup: MTLSize(width: 1, height: 1, depth: 1))
compute.endEncoding()
commands.commit()
commands.waitUntilCompleted()
guard commands.status == .completed else {
    throw ProbeError.unavailable("Metal command failed: \(String(describing: commands.error))")
}
let distance = result.contents().load(as: Float.self)
guard abs(distance - 1.0) < 0.001 else {
    throw ProbeError.wrongResult(distance)
}
print("METAL_RAYTRACING_OK device=\(device.name) hit_distance=\(distance)")
