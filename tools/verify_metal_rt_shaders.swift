#!/usr/bin/env swift
// Verify converted OoT ray tracing functions on a real Metal device, without a game window.
import Foundation
import Metal

enum VerificationError: Error {
    case missing(String)
}

guard CommandLine.arguments.count == 2 else {
    throw VerificationError.missing("Usage: verify_metal_rt_shaders.swift BUILD_DIR")
}
guard let device = MTLCreateSystemDefaultDevice(), device.supportsRaytracing else {
    throw VerificationError.missing("A Metal ray tracing device is required")
}

let directory = URL(fileURLWithPath: CommandLine.arguments[1], isDirectory: true)
let raygen = ["PrimaryRayGen", "DirectRayGen", "IndirectRayGen", "ReflectionRayGen",
              "RefractionRayGen", "OcclusionRayGen", "LightsRayGen", "ScatterRayGen"]
let visible = ["SurfaceMiss", "ShadowMiss", "SurfaceClosestHit", "SurfaceAnyHit", "ShadowAnyHit"]
var functions: [String: MTLFunction] = [:]
for name in raygen + visible {
    let library = try device.makeLibrary(URL: directory.appendingPathComponent("\(name).metallib"))
    guard let function = library.makeFunction(name: name) else {
        throw VerificationError.missing("\(name) is absent from its Metal library")
    }
    if raygen.contains(name) {
        _ = try device.makeComputePipelineState(function: function)
    }
    functions[name] = function
    let reflection = try Data(contentsOf: directory.appendingPathComponent("\(name).json"))
    guard let json = try JSONSerialization.jsonObject(with: reflection) as? [String: Any],
          json["EntryPoint"] as? String == name,
          (json["TopLevelArgumentBuffer"] as? [[String: Any]])?.count == 5 else {
        throw VerificationError.missing("\(name) has incomplete reflection")
    }
}
let wrapper = try device.makeLibrary(
    URL: directory.appendingPathComponent("IndirectTriangleIntersection.metallib"))
guard let wrapperFunction = wrapper.makeFunction(name: "irconverter.wrapper.intersection.function.triangle") else {
    throw VerificationError.missing("Triangle intersection wrapper is absent")
}
let linked = MTLLinkedFunctions()
linked.functions = visible.compactMap { functions[$0] } + [wrapperFunction]
for name in raygen {
    let descriptor = MTLComputePipelineDescriptor()
    descriptor.computeFunction = functions[name]
    descriptor.linkedFunctions = linked
    let pipeline = try device.makeComputePipelineState(descriptor: descriptor, options: [], reflection: nil)
    if name == raygen.first {
        let visibleDescriptor = MTLVisibleFunctionTableDescriptor()
        visibleDescriptor.functionCount = visible.count + 1 // Handle zero is reserved.
        guard let visibleTable = pipeline.makeVisibleFunctionTable(descriptor: visibleDescriptor) else {
            throw VerificationError.missing("Could not allocate visible function table")
        }
        for (index, symbol) in visible.enumerated() {
            guard let function = functions[symbol],
                  let handle = pipeline.functionHandle(function: function) else {
                throw VerificationError.missing("Linked function handle missing: \(symbol)")
            }
            visibleTable.setFunction(handle, index: index + 1)
        }
        let intersectionDescriptor = MTLIntersectionFunctionTableDescriptor()
        intersectionDescriptor.functionCount = 1
        guard let intersectionTable = pipeline.makeIntersectionFunctionTable(descriptor: intersectionDescriptor),
              let handle = pipeline.functionHandle(function: wrapperFunction) else {
            throw VerificationError.missing("Indirect triangle intersection handle missing")
        }
        intersectionTable.setFunction(handle, index: 0)
    }
}
print("METAL_RAYTRACING_LIBRARIES_OK device=\(device.name) raygen=\(raygen.count) visible=\(visible.count) linked=\(raygen.count) tables=2")
