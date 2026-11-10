// Independent CPU oracle and GPU timing for both production light-filter steps.
// Inputs are synthetic; no game or ROM data is used.
import Foundation
import Metal
import simd

enum Failure: Error { case invalid(String) }
func require(_ value: Bool, _ message: String) throws {
    if !value { throw Failure.invalid(message) }
}
guard CommandLine.arguments.count >= 2,
      let device = MTLCreateSystemDefaultDevice(),
      let queue = device.makeCommandQueue() else {
    throw Failure.invalid("Usage: light_filter_macos_test SHADER_DIRECTORY [--benchmark]")
}
let directory = URL(fileURLWithPath: CommandLine.arguments[1])
let benchmark = CommandLine.arguments.contains("--benchmark")
let pipelines = try (1...2).map { step -> (MTLFunction, MTLComputePipelineState) in
    let lib = try device.makeLibrary(URL: directory.appendingPathComponent("RtFilterCS\(step).hlsl.metallib"))
    guard let function = lib.makeFunction(name: "CSMain") else { throw Failure.invalid("Missing CSMain") }
    return (function, try device.makeComputePipelineState(function: function))
}

struct Fixture {
    let width: Int, height: Int
    var input: [SIMD4<Float>], position: [SIMD4<Float>], normal: [SIMD4<Float>], instance: [Int32]
    init(_ width: Int, _ height: Int, background: Bool = false) {
        self.width = width; self.height = height
        input = []; position = []; normal = []; instance = []
        for y in 0..<height { for x in 0..<width {
            let noise = Float((x * 17 + y * 31) % 101) / 100
            input.append(SIMD4(noise, Float(x % 5) / 4, Float(y % 7) / 6, Float((x+y) % 9) / 8))
            position.append(SIMD4(Float(x), Float(y), x < width/2 ? 0 : 3, 1))
            normal.append(x < width/2 ? SIMD4(0, 0, 1, 0) : SIMD4(0.6, 0, 0.8, 0))
            instance.append(background || (x+3*y) % 13 == 0 ? -1 : Int32(x % 3))
        }}
    }
}
func reference(_ f: Fixture, step: Int) -> [SIMD4<Float>] {
    let kernel: [Float] = [1/16, 1/4, 3/8, 1/4, 1/16]
    func rgb(_ v: SIMD4<Float>) -> SIMD3<Float> { SIMD3(v.x, v.y, v.z) }
    func luma(_ v: SIMD4<Float>) -> Float { simd_dot(rgb(v), SIMD3(0.299, 0.587, 0.114)) }
    var output = f.input
    for y in 0..<f.height { for x in 0..<f.width {
        let p = y*f.width+x
        if f.instance[p] < 0 { continue }
        var sum = SIMD3<Float>(repeating: 0), total: Float = 0
        for j in -2...2 { for i in -2...2 {
            let tx = x+i*step, ty = y+j*step
            if tx < 0 || ty < 0 || tx >= f.width || ty >= f.height { continue }
            let t = ty*f.width+tx
            if f.instance[t] < 0 { continue }
            var weight = kernel[i+2]*kernel[j+2]
            weight *= pow(max(0, min(1, simd_dot(rgb(f.normal[p]), rgb(f.normal[t])))), 64)
            let offset = rgb(f.position[t])-rgb(f.position[p])
            let distance = simd_length(offset)
            if distance > 0 { weight *= max(0, min(1, 1-4*abs(simd_dot(rgb(f.normal[p]), offset))/distance)) }
            weight *= exp(-abs(luma(f.input[t])-luma(f.input[p]))/0.25)
            sum += rgb(f.input[t])*weight; total += weight
        }}
        if total > 0 { output[p] = SIMD4(sum/total, f.input[p].w) }
    }}
    return output
}
func texture<T>(_ f: Fixture, _ values: [T], _ format: MTLPixelFormat) throws -> MTLTexture {
    let desc = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: format, width: f.width, height: f.height, mipmapped: false)
    desc.storageMode = .shared; desc.usage = [.shaderRead, .shaderWrite]
    guard let texture = device.makeTexture(descriptor: desc) else { throw Failure.invalid("Texture allocation") }
    values.withUnsafeBytes { bytes in
        texture.replace(region: MTLRegionMake2D(0, 0, f.width, f.height), mipmapLevel: 0,
                        withBytes: bytes.baseAddress!, bytesPerRow: f.width*MemoryLayout<T>.stride)
    }
    return texture
}
func run(_ f: Fixture, step: Int, repeats: Int = 1) throws -> ([SIMD4<Float>], Double) {
    let resources = try [texture(f, f.input, .rgba32Float), texture(f, f.position, .rgba32Float),
                         texture(f, f.normal, .rgba32Float), texture(f, f.instance, .r32Sint),
                         texture(f, f.input, .rgba32Float)]
    let (function, pipeline) = pipelines[step-1]
    try require(pipeline.staticThreadgroupMemoryLength <= device.maxThreadgroupMemoryLength,
                "Filter exceeds the device's threadgroup memory limit")
    let args = function.makeArgumentEncoder(bufferIndex: 0)
    guard let table = device.makeBuffer(length: args.encodedLength, options: .storageModeShared) else {
        throw Failure.invalid("Argument allocation")
    }
    args.setArgumentBuffer(table, offset: 0)
    for (index, resource) in resources.enumerated() { args.setTexture(resource, index: index+1) }
    var timings: [Double] = []
    for iteration in 0..<(repeats == 1 ? 1 : repeats+3) {
        guard let command = queue.makeCommandBuffer(), let encoder = command.makeComputeCommandEncoder() else {
            throw Failure.invalid("Command allocation")
        }
        encoder.setComputePipelineState(pipeline); encoder.setBuffer(table, offset: 0, index: 0)
        for (index, resource) in resources.enumerated() { encoder.useResource(resource, usage: index == 4 ? .write : .read) }
        encoder.dispatchThreadgroups(MTLSize(width: (f.width+7)/8, height: (f.height+7)/8, depth: 1),
                                     threadsPerThreadgroup: MTLSize(width: 8, height: 8, depth: 1))
        encoder.endEncoding(); command.commit()
        let deadline = Date().addingTimeInterval(10)
        while command.status != .completed && command.status != .error && Date() < deadline {
            Thread.sleep(forTimeInterval: 0.001)
        }
        try require(command.status == .completed, "Filter GPU failure/timeout: \(String(describing: command.error))")
        if repeats == 1 || iteration >= 3 { timings.append((command.gpuEndTime-command.gpuStartTime)*1000) }
    }
    var output = f.input
    output.withUnsafeMutableBytes { bytes in
        resources[4].getBytes(bytes.baseAddress!, bytesPerRow: f.width*16,
                              from: MTLRegionMake2D(0, 0, f.width, f.height), mipmapLevel: 0)
    }
    return (output, timings.sorted()[timings.count/2])
}

for (w, h) in [(1, 1), (3, 5), (8, 8), (9, 7), (17, 19), (32, 24)] {
    for background in [false, true] {
        var fixture = Fixture(w, h, background: background)
        // Include coincident positions and zero normals to cover the zero-weight fallback.
        if fixture.input.count > 3 {
            fixture.position[1] = fixture.position[2]
            fixture.normal[3] = .zero
        }
        for p in fixture.instance.indices where fixture.instance[p] < 0 {
            fixture.position[p] = SIMD4(repeating: .nan)
            fixture.normal[p] = SIMD4(repeating: .nan)
        }
        for step in 1...2 {
            let expected = reference(fixture, step: step)
            let (actual, _) = try run(fixture, step: step)
            var error: Float = 0
            for p in actual.indices {
                for c in 0..<4 {
                    try require(actual[p][c].isFinite, "Nonfinite output at \(p), channel \(c)")
                    error = max(error, abs(actual[p][c]-expected[p][c]))
                }
                try require(actual[p].w == fixture.input[p].w, "Alpha changed")
                if fixture.instance[p] < 0 { try require(actual[p] == fixture.input[p], "Background changed") }
            }
            try require(error < 0.0001, "Filter mismatch step=\(step) size=\(w)x\(h) error=\(error)")
            print("FILTER_EQUIVALENCE_OK step=\(step) size=\(w)x\(h) background=\(background) max_error=\(error)")
            fixture.input = actual // Also test the second pass on first-pass output.
        }
    }
}
if benchmark {
    let fixture = Fixture(3060, 1920)
    for step in 1...2 {
        let (_, milliseconds) = try run(fixture, step: step, repeats: 9)
        print("FILTER_GPU_TIME step=\(step) size=3060x1920 median_ms=\(milliseconds) device=\(device.name)")
    }
}
