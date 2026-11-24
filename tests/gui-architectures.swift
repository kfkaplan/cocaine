// swiftc -parse-as-library gui/Sources/Shell.swift gui/Sources/Model.swift tests/gui-architectures.swift -o /tmp/gui-architectures && /tmp/gui-architectures [cocaine-path [game-path]]
import Foundation

@main
struct ArchitectureTests {
  static func main() throws {
    let dir = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
    try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    defer { try! FileManager.default.removeItem(at: dir) }

    func check(_ name: String, _ bytes: [UInt8], _ expected: Architectures) throws {
      let file = dir.appendingPathComponent(name)
      try Data(bytes).write(to: file)
      precondition(Architectures.of(file) == expected, "wrong architecture for \(name)")
    }

    var pef = Array("Joy!peffpwpc".utf8) + [0, 0, 0, 1] + Array(repeating: UInt8(0), count: 24)
    pef[33] = 3
    try check("pef", pef, [.powerpc, .classicPEF])
    let bundle = dir.appendingPathComponent("Extensionless Application")
    let contents = bundle.appendingPathComponent("Contents")
    let macOS = contents.appendingPathComponent("MacOS")
    try FileManager.default.createDirectory(at: macOS, withIntermediateDirectories: true)
    let plist: [String: String] = ["CFBundleExecutable": "Game", "CFBundlePackageType": "APPL",
                                   "CFBundleIdentifier": "test.cocaine.extensionless"]
    let data = try PropertyListSerialization.data(fromPropertyList: plist, format: .xml, options: 0)
    try data.write(to: contents.appendingPathComponent("Info.plist"))
    try Data(pef).write(to: macOS.appendingPathComponent("Game"))
    precondition(Architectures.of(bundle) == [.powerpc, .classicPEF],
                 "extensionless app bundle not recognized")
    try check("truncated", Array(pef.prefix(16)), [])
    var wrongCPU = pef
    wrongCPU.replaceSubrange(8..<12, with: Array("m68k".utf8))
    try check("wrong-cpu", wrongCPU, [])
    var badVersion = pef
    badVersion[15] = 2
    try check("bad-version", badVersion, [])
    var noSections = pef
    noSections[33] = 0
    try check("no-sections", noSections, [])
    try check("ppc-mach-o", [0xfe, 0xed, 0xfa, 0xce, 0, 0, 0, 18], [.powerpc])
    try check("intel-mach-o", [0xce, 0xfa, 0xed, 0xfe, 7, 0, 0, 0], [.intel])
    if CommandLine.arguments.count > 1 {
      func checkCLI(_ name: String, _ arch: String, _ message: String, _ status: Int32 = 126) throws {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: CommandLine.arguments[1])
        process.arguments = ["--info", "--arch", arch, dir.appendingPathComponent(name).path]
        let output = Pipe()
        process.standardError = output
        process.standardOutput = output
        try process.run()
        let error = String(decoding: output.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self)
        process.waitUntilExit()
        precondition(process.terminationStatus == status && error.contains(message), "wrong CLI result: \(error)")
      }
      try checkCLI("pef", "ppc", "PowerPC PEF/CFM", 0)
      try checkCLI("Extensionless Application", "ppc", "PowerPC PEF/CFM", 0)
      try checkCLI("pef", "i386", "is a PowerPC PEF/CFM file, not i386")
      try checkCLI("no-sections", "ppc", "not a Mach-O file")
    }
    if CommandLine.arguments.count > 2 {
      precondition(Architectures.of(URL(fileURLWithPath: CommandLine.arguments[2])) == [.powerpc, .classicPEF])
    }
    if CommandLine.arguments.count > 3 {
      precondition(Architectures.of(URL(fileURLWithPath: CommandLine.arguments[3])) == [.powerpc, .classicPEF])
    }
    print("GUI architecture tests passed")
  }
}
