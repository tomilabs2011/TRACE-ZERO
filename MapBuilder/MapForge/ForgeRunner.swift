import Foundation

/// Runs the bundled forge.sh and turns its progress lines into state:
///   FORGE <step> <steps> <title>   pipeline step
///   STAGE <i> <n> <title>          tzforge stage
///   PROGRESS <percent>             tzforge progress
///   PACK <path>                    finished pack
///   ERROR <message>                failure
@MainActor
final class ForgeRunner: ObservableObject {
    enum State: Equatable { case idle, running, finished(URL), failed(String), cancelled }

    @Published private(set) var state: State = .idle
    @Published private(set) var stepTitle = ""
    @Published private(set) var detail = ""
    @Published private(set) var fraction: Double?   // nil = indeterminate
    @Published private(set) var log: [String] = []

    private var process: Process?
    private var step = 0, steps = 1
    private var buffers: [ObjectIdentifier: String] = [:]

    var isRunning: Bool { state == .running }

    /// forge.sh and tzforge are copied into the app's Resources at build
    /// time; running from the repository also works.
    static var scriptURL: URL? {
        if let u = Bundle.main.url(forResource: "forge", withExtension: "sh") { return u }
        let repo = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent().appendingPathComponent("Forge/forge.sh")
        return FileManager.default.fileExists(atPath: repo.path) ? repo : nil
    }

    func run(arguments: [String]) {
        guard !isRunning else { return }
        guard let script = Self.scriptURL else {
            state = .failed("forge.sh is missing from the app")
            return
        }
        log = []
        stepTitle = "Starting"
        detail = ""
        fraction = 0
        step = 0
        steps = 1
        state = .running

        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/bin/bash")
        p.arguments = [script.path] + arguments + ["--progress"]
        var env = ProcessInfo.processInfo.environment
        env["PATH"] = "/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin:" + (env["PATH"] ?? "")
        p.environment = env
        p.currentDirectoryURL = FileManager.default.homeDirectoryForCurrentUser
        let out = Pipe(), err = Pipe()
        p.standardOutput = out
        p.standardError = err
        for (pipe, isErr) in [(out, false), (err, true)] {
            pipe.fileHandleForReading.readabilityHandler = { [weak self] h in
                let data = h.availableData
                guard !data.isEmpty, let s = String(data: data, encoding: .utf8) else { return }
                Task { @MainActor in self?.consume(s, from: pipe, isError: isErr) }
            }
        }
        p.terminationHandler = { [weak self] proc in
            out.fileHandleForReading.readabilityHandler = nil
            err.fileHandleForReading.readabilityHandler = nil
            Task { @MainActor in self?.finished(status: proc.terminationStatus, reason: proc.terminationReason) }
        }
        do {
            try p.run()
            process = p
        } catch {
            state = .failed("Could not start forge.sh: \(error.localizedDescription)")
        }
    }

    func cancel() {
        guard let p = process, p.isRunning else { return }
        state = .cancelled
        p.terminate()
    }

    private func consume(_ chunk: String, from pipe: Pipe, isError: Bool) {
        let key = ObjectIdentifier(pipe)
        var text = (buffers[key] ?? "") + chunk
        // Planetiler and curl redraw progress with carriage returns.
        text = text.replacingOccurrences(of: "\r", with: "\n")
        var lines = text.components(separatedBy: "\n")
        buffers[key] = lines.removeLast()
        for line in lines where !line.isEmpty { handle(line, isError: isError) }
    }

    private func handle(_ line: String, isError: Bool) {
        let parts = line.split(separator: " ", maxSplits: 3).map(String.init)
        switch parts.first {
        case "FORGE" where parts.count >= 4 && !isError:
            step = Int(parts[1]) ?? step
            steps = max(1, Int(parts[2]) ?? steps)
            stepTitle = parts[3]
            detail = ""
            fraction = Double(step - 1) / Double(steps)
            append("▶ " + parts[3])
        case "STAGE" where parts.count >= 4 && !isError:
            detail = parts[3]
        case "PROGRESS" where parts.count >= 2 && !isError:
            if let pct = Double(parts[1]) {
                fraction = (Double(step - 1) + min(1, pct / 100)) / Double(steps)
            }
        case "PACK" where !isError:
            let path = String(line.dropFirst(5))
            state = .finished(URL(fileURLWithPath: path))
        case "ERROR" where !isError:
            state = .failed(String(line.dropFirst(6)))
        case "DONE" where !isError:
            break
        default:
            append(line)
        }
    }

    private func append(_ line: String) {
        log.append(line)
        if log.count > 400 { log.removeFirst(log.count - 400) }
    }

    private func finished(status: Int32, reason: Process.TerminationReason) {
        process = nil
        switch state {
        case .running:
            state = status == 0 ? .failed("forge.sh ended without a result") : .failed("forge.sh failed (exit \(status))")
        case .finished:
            fraction = 1
        default:
            break
        }
    }
}
