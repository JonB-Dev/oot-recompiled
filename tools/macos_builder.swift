import AppKit

final class DropZone: NSBox {
    var receive: ((URL) -> Void)?
    override init(frame: NSRect) {
        super.init(frame: frame)
        registerForDraggedTypes([.fileURL])
        title = "Your ROM stays on this Mac"
    }
    required init?(coder: NSCoder) { fatalError() }
    func files(_ sender: NSDraggingInfo) -> [URL] {
        (sender.draggingPasteboard.readObjects(forClasses: [NSURL.self], options: [.urlReadingFileURLsOnly: true]) as? [URL]) ?? []
    }
    override func draggingEntered(_ sender: NSDraggingInfo) -> NSDragOperation {
        files(sender).count == 1 ? .copy : []
    }
    override func performDragOperation(_ sender: NSDraggingInfo) -> Bool {
        let urls = files(sender)
        guard urls.count == 1 else { return false }
        receive?(urls[0]); return true
    }
}

final class Builder: NSObject, NSApplicationDelegate, NSWindowDelegate {
    let root: URL
    let python: String
    var window: NSWindow!
    var rom: URL?
    var builtApp: URL?
    var process: Process?
    let romLabel = NSTextField(wrappingLabelWithString: "Drop your USA 1.0 .z64 ROM here, or choose a file.")
    let status = NSTextField(wrappingLabelWithString: "Choose your ROM. Setup can be checked before building.")
    let log = NSTextView()
    let tracing = NSButton(checkboxWithTitle: "Include fancy lighting (Apple shader converter required)", target: nil, action: nil)
    var controls: [NSControl] = []
    var openButton: NSButton!
    var cancelButton: NSButton!
    init(root: URL, python: String) { self.root = root; self.python = python }
    func button(_ title: String, _ action: Selector) -> NSButton {
        let b = NSButton(title: title, target: self, action: action)
        controls.append(b); return b
    }
    func applicationDidFinishLaunching(_ notification: Notification) {
        let menu = NSMenu(); let item = NSMenuItem(); menu.addItem(item)
        let appMenu = NSMenu(); appMenu.addItem(withTitle: "Quit builder", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        item.submenu = appMenu; NSApp.mainMenu = menu
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 780, height: 690), styleMask: [.titled, .closable, .miniaturizable], backing: .buffered, defer: false)
        window.title = "Build OoT for Mac"; window.delegate = self
        let content = window.contentView!
        let stack = NSStackView(); stack.orientation = .vertical; stack.alignment = .leading; stack.spacing = 16
        stack.translatesAutoresizingMaskIntoConstraints = false; content.addSubview(stack)
        NSLayoutConstraint.activate([stack.leadingAnchor.constraint(equalTo: content.leadingAnchor, constant: 24), stack.trailingAnchor.constraint(equalTo: content.trailingAnchor, constant: -24), stack.topAnchor.constraint(equalTo: content.topAnchor, constant: 24), stack.bottomAnchor.constraint(equalTo: content.bottomAnchor, constant: -24)])
        let heading = NSTextField(labelWithString: "Your ROM → your Mac app")
        heading.font = .boldSystemFont(ofSize: 28); stack.addArrangedSubview(heading)
        stack.addArrangedSubview(NSTextField(wrappingLabelWithString: "One local build. No Windows, no ROM upload. The first build downloads source and tools and may take a while."))
        let drop = DropZone(frame: .zero); drop.translatesAutoresizingMaskIntoConstraints = false
        drop.receive = { [weak self] url in self?.select(url) }
        let dropContent = NSStackView(views: [romLabel, button("Choose ROM…", #selector(chooseROM))]); dropContent.orientation = .vertical; dropContent.spacing = 12
        drop.contentView = dropContent; stack.addArrangedSubview(drop)
        drop.widthAnchor.constraint(equalTo: stack.widthAnchor).isActive = true
        drop.heightAnchor.constraint(equalToConstant: 110).isActive = true
        tracing.state = .on; controls.append(tracing); stack.addArrangedSubview(tracing)
        let setup = NSStackView(views: [button("Check setup", #selector(check)), button("Install build tools", #selector(install)), button("Select Apple converter…", #selector(converter))]); stack.addArrangedSubview(setup)
        let links = NSStackView(views: [button("Get Xcode", #selector(xcode)), button("Get Homebrew", #selector(homebrew)), button("Get Apple converter", #selector(appleConverter))]); stack.addArrangedSubview(links)
        openButton = button("Open app", #selector(openApp)); openButton.isEnabled = false
        let build = button("Build app", #selector(buildApp)); build.bezelStyle = .rounded; build.keyEquivalent = "\r"
        cancelButton = NSButton(title: "Cancel", target: self, action: #selector(cancelTask)); cancelButton.isEnabled = false
        stack.addArrangedSubview(NSStackView(views: [build, openButton, button("Show app in Finder", #selector(reveal)), cancelButton]))
        status.font = .systemFont(ofSize: 15, weight: .medium); stack.addArrangedSubview(status)
        let scroll = NSScrollView(); scroll.hasVerticalScroller = true; scroll.borderType = .bezelBorder
        log.isEditable = false; log.font = .monospacedSystemFont(ofSize: 12, weight: .regular)
        log.isVerticallyResizable = true; log.autoresizingMask = [.width]; log.textContainer?.widthTracksTextView = true
        scroll.documentView = log; stack.addArrangedSubview(scroll)
        scroll.widthAnchor.constraint(equalTo: stack.widthAnchor).isActive = true
        scroll.heightAnchor.constraint(greaterThanOrEqualToConstant: 150).isActive = true
        window.center(); window.makeKeyAndOrderFront(nil); NSApp.activate(ignoringOtherApps: true)
    }
    func select(_ url: URL) {
        guard process == nil else { return }
        rom = url; romLabel.stringValue = url.lastPathComponent
        status.stringValue = "ROM selected. Its exact version will be verified before building."
    }
    func pick(_ message: String) -> URL? {
        let panel = NSOpenPanel(); panel.message = message; panel.canChooseDirectories = false; panel.allowsMultipleSelection = false
        return panel.runModal() == .OK ? panel.url : nil
    }
    @objc func chooseROM() { if let url = pick("Choose your unmodified USA 1.0 ROM (.z64).") { select(url) } }
    @objc func check() { run(["check"] + (tracing.state == .on ? ["--tracing"] : [])) }
    @objc func install() {
        let alert = NSAlert(); alert.messageText = "Install Mac build tools?"
        alert.informativeText = "Homebrew will download and install CMake, Ninja, LLVM, lld, Make, Git, SDL2, FreeType and Python. This can take several minutes and uses disk space."
        alert.addButton(withTitle: "Install"); alert.addButton(withTitle: "Cancel")
        if alert.runModal() == .alertFirstButtonReturn { run(["install-tools"]) }
    }
    @objc func converter() {
        guard let url = pick("Select Apple's Metal Shader Converter 4.0 beta 3 .pkg downloaded from Apple.") else { return }
        let alert = NSAlert(); alert.messageText = "Apple converter license"
        alert.informativeText = "Continue only after reviewing and accepting Apple's license supplied with this download. The verified package will be extracted locally; it is not redistributed."
        alert.addButton(withTitle: "I reviewed and accept the license"); alert.addButton(withTitle: "Cancel")
        if alert.runModal() == .alertFirstButtonReturn { run(["converter", "--accept-license", "--package", url.path]) }
    }
    @objc func xcode() { NSWorkspace.shared.open(URL(string: "https://developer.apple.com/xcode/")!) }
    @objc func homebrew() { NSWorkspace.shared.open(URL(string: "https://brew.sh")!) }
    @objc func appleConverter() { NSWorkspace.shared.open(URL(string: "https://developer.apple.com/download/all/?q=Metal%20Shader%20Converter")!) }
    @objc func buildApp() {
        guard let rom = rom else { status.stringValue = "Drop a ROM above or click Choose ROM first."; return }
        let rt = tracing.state == .on
        builtApp = nil; openButton.isEnabled = false
        let source = FileManager.default.fileExists(atPath: root.appendingPathComponent(".git").path) ? root : root.appendingPathComponent("build/source")
        run(["build", "--rom", rom.path] + (rt ? ["--tracing"] : []), result: source.appendingPathComponent(rt ? "build/rt-compile-probe/OoTRecompiled.app" : "build-cmake/OoTRecompiled.app"))
    }
    @objc func cancelTask() { process?.terminate(); cancelButton.isEnabled = false; status.stringValue = "Stopping build processes…" }
    @objc func openApp() { if let app = builtApp { NSWorkspace.shared.open(app) } }
    @objc func reveal() { if let app = builtApp { NSWorkspace.shared.activateFileViewerSelecting([app]) } }
    func append(_ text: String) {
        log.textStorage?.append(NSAttributedString(string: text, attributes: [.font: NSFont.monospacedSystemFont(ofSize: 12, weight: .regular)]))
        if let storage = log.textStorage, storage.length > 100_000 { storage.deleteCharacters(in: NSRange(location: 0, length: storage.length - 80_000)) }
        log.scrollToEndOfDocument(nil)
    }
    func run(_ arguments: [String], result: URL? = nil) {
        guard process == nil else { return }
        let p = Process(); p.executableURL = URL(fileURLWithPath: python)
        p.arguments = ["-u", root.appendingPathComponent("tools/macos_builder.py").path] + arguments
        p.currentDirectoryURL = root
        var env = ProcessInfo.processInfo.environment
        env["PATH"] = "/opt/homebrew/bin:/opt/homebrew/opt/make/libexec/gnubin:" + (env["PATH"] ?? "/usr/bin:/bin:/usr/sbin:/sbin")
        p.environment = env
        let pipe = Pipe(); p.standardOutput = pipe; p.standardError = pipe
        log.string = ""; status.stringValue = "Working… Details are shown below. This window can be minimized."
        controls.forEach { $0.isEnabled = false }; cancelButton.isEnabled = true; process = p
        do { try p.run() } catch { finish(false, result: nil); append(error.localizedDescription); return }
        DispatchQueue.global(qos: .userInitiated).async {
            while true {
                let data = pipe.fileHandleForReading.availableData
                if data.isEmpty { break }
                let text = String(decoding: data, as: UTF8.self)
                DispatchQueue.main.async { self.append(text) }
            }
            p.waitUntilExit()
            DispatchQueue.main.async { self.finish(p.terminationStatus == 0, result: result) }
        }
    }
    func finish(_ success: Bool, result: URL?) {
        process = nil; cancelButton.isEnabled = false; controls.forEach { $0.isEnabled = true }
        if success, let result = result { builtApp = result }
        openButton.isEnabled = builtApp != nil
        status.stringValue = success ? (result == nil ? "Done. Details below." : "Your app is ready. Click Open app to play.") : "Needs attention. The reason is shown below; fix it and retry."
    }
    func windowShouldClose(_ sender: NSWindow) -> Bool {
        if process != nil { status.stringValue = "A task is still running. Minimize this window until it finishes."; return false }
        NSApp.terminate(nil); return true
    }
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        if process != nil { status.stringValue = "A task is still running. Minimize this window until it finishes."; return .terminateCancel }
        return .terminateNow
    }
}
let app = NSApplication.shared
app.setActivationPolicy(.regular)
guard CommandLine.arguments.count == 3 else { fatalError("Launch with Build macOS App.command from the source folder.") }
let delegate = Builder(root: URL(fileURLWithPath: CommandLine.arguments[1]), python: CommandLine.arguments[2])
app.delegate = delegate
app.run()
