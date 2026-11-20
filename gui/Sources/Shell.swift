// Running other programs: git, cmake, the system root importer, cocaine.
//
// An app started from the Finder gets a bare PATH, so the places Homebrew,
// MacPorts and Xcode put their tools are spelled out here.

import Foundation

enum Shell {
  static let searchPath = ["/opt/homebrew/bin", "/usr/local/bin", "/opt/local/bin", "/usr/bin", "/bin", "/usr/sbin", "/sbin"]

  static var environment: [String: String] {
    var env = ProcessInfo.processInfo.environment
    env["PATH"] = searchPath.joined(separator: ":")
    return env
  }

  /// Where a command line tool is, or nil.
  static func which(_ tool: String) -> String? {
    for dir in searchPath {
      let path = dir + "/" + tool
      if FileManager.default.isExecutableFile(atPath: path) { return path }
    }
    return nil
  }

  /// Runs a quick command to the end and returns its status and output.
  @discardableResult
  static func capture(_ tool: String, _ args: [String], in dir: String? = nil) -> (status: Int32, output: String) {
    guard let exe = tool.hasPrefix("/") ? tool : which(tool) else { return (-1, "") }
    let process = Process()
    process.executableURL = URL(fileURLWithPath: exe)
    process.arguments = args
    process.environment = environment
    if let dir { process.currentDirectoryURL = URL(fileURLWithPath: dir) }
    let pipe = Pipe()
    process.standardOutput = pipe
    process.standardError = Pipe()
    do { try process.run() } catch { return (-1, "") }
    let data = pipe.fileHandleForReading.readDataToEndOfFile()
    process.waitUntilExit()
    let text = String(decoding: data, as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines)
    return (process.terminationStatus, text)
  }

  /// Lines some Macs' system tweaks print from every program, which say
  /// nothing about the command itself.
  static func isNoise(_ line: String) -> Bool {
    line.contains("[AppleSharpener]") || line.contains("[BlurTweak]")
  }
}

/// The output of running commands, kept for showing: added to from any
/// thread, shown on the main one a few times a second.
final class LogBuffer: ObservableObject, @unchecked Sendable {
  @Published private(set) var lines: [String] = []
  private var pending: [String] = []
  private var scheduled = false
  private let lock = NSLock()
  private let limit = 5000

  func add(_ line: String) {
    lock.lock()
    pending.append(line)
    let schedule = !scheduled
    scheduled = true
    lock.unlock()
    if schedule {
      DispatchQueue.main.asyncAfter(deadline: .now() + 0.08) { self.flush() }
    }
  }

  private func flush() {
    lock.lock()
    let new = pending
    pending.removeAll()
    scheduled = false
    lock.unlock()
    lines.append(contentsOf: new)
    if lines.count > limit { lines.removeFirst(lines.count - limit) }
  }

  func clear() {
    lock.lock()
    pending.removeAll()
    lock.unlock()
    lines.removeAll()
  }
}

/// A command that may run for a while, its output passed on a line at a time.
final class RunningCommand: @unchecked Sendable {
  private let process = Process()
  private let lock = NSLock()
  private var partial = Data()

  private static let registryLock = NSLock()
  private static var registry: [ObjectIdentifier: RunningCommand] = [:]

  init(_ tool: String, _ args: [String], in dir: String? = nil, environment extra: [String: String] = [:]) {
    let exe = tool.hasPrefix("/") ? tool : (Shell.which(tool) ?? tool)
    process.executableURL = URL(fileURLWithPath: exe)
    process.arguments = args
    var env = Shell.environment
    for (k, v) in extra { env[k] = v }
    process.environment = env
    if let dir { process.currentDirectoryURL = URL(fileURLWithPath: dir) }
  }

  /// Runs it, handing each line of output (and errors) to `onLine` from a
  /// background thread, and returns its exit status.
  func run(onLine: @escaping @Sendable (String) -> Void) async -> Int32 {
    let pipe = Pipe()
    process.standardOutput = pipe
    process.standardError = pipe
    process.standardInput = FileHandle.nullDevice
    let reader = pipe.fileHandleForReading
    reader.readabilityHandler = { [weak self] handle in
      let data = handle.availableData
      guard let self, !data.isEmpty else { return }
      self.take(data, onLine)
    }
    Self.register(self, true)
    let status: Int32 = await withCheckedContinuation { done in
      process.terminationHandler = { [weak self] p in
        reader.readabilityHandler = nil
        let rest = reader.readDataToEndOfFile()
        if let self {
          self.take(rest, onLine)
          self.finish(onLine)
        }
        done.resume(returning: p.terminationStatus)
      }
      do {
        try process.run()
      } catch {
        onLine("Couldn't start \(process.executableURL?.lastPathComponent ?? "the command"): \(error.localizedDescription)")
        process.terminationHandler = nil
        done.resume(returning: -1)
      }
    }
    Self.register(self, false)
    return status
  }

  private func take(_ data: Data, _ onLine: (String) -> Void) {
    lock.lock()
    partial.append(data)
    var lines: [String] = []
    while let end = partial.firstIndex(of: 10) {
      lines.append(String(decoding: partial[partial.startIndex..<end], as: UTF8.self))
      partial.removeSubrange(partial.startIndex...end)
    }
    lock.unlock()
    for line in lines where !Shell.isNoise(line) { onLine(line) }
  }

  private func finish(_ onLine: (String) -> Void) {
    lock.lock()
    let last = String(decoding: partial, as: UTF8.self)
    partial.removeAll()
    lock.unlock()
    if !last.isEmpty && !Shell.isNoise(last) { onLine(last) }
  }

  private static func register(_ command: RunningCommand, _ running: Bool) {
    registryLock.lock()
    registry[ObjectIdentifier(command)] = running ? command : nil
    registryLock.unlock()
  }

  /// Asks it to stop (SIGTERM).
  func stop() {
    if process.isRunning { process.terminate() }
  }

  /// Stops everything still running: for when the app quits.
  static func stopAll() {
    registryLock.lock()
    let all = Array(registry.values)
    registryLock.unlock()
    for command in all { command.stop() }
  }
}
