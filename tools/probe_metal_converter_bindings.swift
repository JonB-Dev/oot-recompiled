#!/usr/bin/env swift
// Execute a converted HLSL compute shader against a real 24-byte descriptor table.
import Foundation
import Metal

enum ProbeError: Error {
    case invalid(String)
}

guard CommandLine.arguments.count == 2,
      let device = MTLCreateSystemDefaultDevice(),
      let queue = device.makeCommandQueue() else {
    throw ProbeError.invalid("Usage: probe_metal_converter_bindings.swift PROBE.metallib")
}
let library = try device.makeLibrary(URL: URL(fileURLWithPath: CommandLine.arguments[1]))
guard let function = library.makeFunction(name: "CSMain") else {
    throw ProbeError.invalid("CSMain was not converted")
}
let pipeline = try device.makeComputePipelineState(function: function)

// Apple Metal Shader Converter's IRDescriptorTableEntry is 3 x UInt64.
guard let input = device.makeBuffer(length: 4, options: .storageModeShared),
      let output = device.makeBuffer(length: 4, options: .storageModeShared),
      let table = device.makeBuffer(length: 2 * 24, options: .storageModeShared),
      let root = device.makeBuffer(length: 8, options: .storageModeShared),
      let command = queue.makeCommandBuffer(),
      let encoder = command.makeComputeCommandEncoder() else {
    throw ProbeError.invalid("Could not allocate probe resources")
}
input.contents().storeBytes(of: UInt32(35), as: UInt32.self)
output.contents().storeBytes(of: UInt32(0), as: UInt32.self)
let slots = table.contents().bindMemory(to: UInt64.self, capacity: 6)
for index in 0..<6 { slots[index] = 0 }
slots[0] = output.gpuAddress
slots[2] = 4 // Output buffer size, in the low 32 bits of metadata.
slots[3] = input.gpuAddress
slots[5] = 4 // Input buffer size.
root.contents().storeBytes(of: table.gpuAddress, as: UInt64.self)

encoder.setComputePipelineState(pipeline)
encoder.setBuffer(root, offset: 0, index: 2) // kIRArgumentBufferBindPoint
encoder.useResource(table, usage: .read)
encoder.useResource(input, usage: .read)
encoder.useResource(output, usage: .write)
encoder.dispatchThreadgroups(MTLSize(width: 1, height: 1, depth: 1),
                             threadsPerThreadgroup: MTLSize(width: 1, height: 1, depth: 1))
encoder.endEncoding()
command.commit()
command.waitUntilCompleted()
guard command.status == .completed else {
    throw ProbeError.invalid("Metal command failed: \(String(describing: command.error))")
}
let result = output.contents().load(as: UInt32.self)
guard result == 42 else {
    throw ProbeError.invalid("Expected converted compute output 42, got \(result)")
}
print("METAL_CONVERTER_BINDINGS_OK device=\(device.name) result=\(result)")
