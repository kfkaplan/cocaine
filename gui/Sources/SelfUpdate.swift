// The launcher updating itself from its GitHub releases.
//
// Each time the app opens it asks GitHub for the repository's releases, and
// if one tagged gui-v<version> is newer than this app, offers it. Yes
// downloads the release's zip, unpacks it, and hands over to a small script
// that waits for this app to quit, puts the new one in its place and opens it.

import AppKit
import Foundation

enum LauncherUpdate {
  static let releases = URL(string: "https://api.github.com/repos/sawyerthemiller/cocaine/releases?per_page=30")!
  static let tagPrefix = "gui-v"

  struct Release {
    let version: [Int]
    let zip: URL
  }

  static var currentVersion: [Int] {
    parse(Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "0")
  }

  static func parse(_ text: String) -> [Int] {
    text.split(separator: ".").map { Int($0) ?? 0 }
  }

  static func isNewer(_ a: [Int], than b: [Int]) -> Bool {
    for k in 0..<max(a.count, b.count) {
      let x = k < a.count ? a[k] : 0, y = k < b.count ? b[k] : 0
      if x != y { return x > y }
    }
    return false
  }

  /// The newest launcher release on GitHub, or nil if there's none or GitHub
  /// can't be reached (then nothing is said).
  static func newest() async -> Release? {
    var request = URLRequest(url: releases, timeoutInterval: 15)
    request.setValue("Cocaine-launcher", forHTTPHeaderField: "User-Agent")
    request.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
    guard let (data, response) = try? await URLSession.shared.data(for: request),
          (response as? HTTPURLResponse)?.statusCode == 200,
          let list = try? JSONSerialization.jsonObject(with: data) as? [[String: Any]] else { return nil }
    var best: Release?
    for item in list {
      guard let tag = item["tag_name"] as? String, tag.hasPrefix(tagPrefix),
            item["draft"] as? Bool != true, item["prerelease"] as? Bool != true,
            let assets = item["assets"] as? [[String: Any]],
            let zip = assets.compactMap({ $0["browser_download_url"] as? String })
              .first(where: { $0.hasSuffix(".zip") }).flatMap(URL.init(string:)) else { continue }
      let version = parse(String(tag.dropFirst(tagPrefix.count)))
      if best == nil || isNewer(version, than: best!.version) { best = Release(version: version, zip: zip) }
    }
    return best
  }

  /// Looks, and if there's something newer, asks.
  @MainActor
  static func checkAndOffer() async {
    guard let release = await newest(), isNewer(release.version, than: currentVersion) else { return }
    let alert = NSAlert()
    alert.messageText = "An update to the Cocaine launcher is now available."
    alert.informativeText = "Do you want to do it?"
    alert.addButton(withTitle: "Yes")
    alert.addButton(withTitle: "No")
    NSApp.activate(ignoringOtherApps: true)
    guard alert.runModal() == .alertFirstButtonReturn else { return }
    do {
      try await install(release)
    } catch {
      let failed = NSAlert()
      failed.messageText = "Couldn't update the launcher"
      failed.informativeText = "\(error.localizedDescription) - you can download it from GitHub instead."
      failed.runModal()
    }
  }

  struct Failure: LocalizedError {
    let errorDescription: String?
    init(_ text: String) { errorDescription = text }
  }

  /// Downloads and unpacks the release, then quits so the script can swap it in.
  @MainActor
  static func install(_ release: Release) async throws {
    let here = Bundle.main.bundleURL
    let folder = here.deletingLastPathComponent()
    guard !here.path.contains("/AppTranslocation/"), FileManager.default.isWritableFile(atPath: folder.path) else {
      throw Failure("macOS won't let it change the folder it's in (\(folder.path))")
    }
    let work = FileManager.default.temporaryDirectory.appendingPathComponent("cocaine-update-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: work, withIntermediateDirectories: true)

    var request = URLRequest(url: release.zip, timeoutInterval: 120)
    request.setValue("Cocaine-launcher", forHTTPHeaderField: "User-Agent")
    let (download, response) = try await URLSession.shared.download(for: request)
    guard (response as? HTTPURLResponse)?.statusCode == 200 else { throw Failure("the download from GitHub failed") }
    let zip = work.appendingPathComponent("Cocaine.zip")
    try FileManager.default.moveItem(at: download, to: zip)

    let unpacked = work.appendingPathComponent("new")
    guard Shell.capture("/usr/bin/ditto", ["-x", "-k", zip.path, unpacked.path]).status == 0 else {
      throw Failure("the download couldn't be unpacked")
    }
    guard let app = (try? FileManager.default.contentsOfDirectory(at: unpacked, includingPropertiesForKeys: nil))?
            .first(where: { $0.pathExtension == "app" }),
          Bundle(url: app)?.bundleIdentifier == Bundle.main.bundleIdentifier else {
      throw Failure("the download doesn't hold the launcher")
    }

    // Waits for this app to quit, swaps the new one in (putting the old one
    // back if that fails), and opens it.
    let script = """
      while kill -0 "$1" 2>/dev/null; do sleep 0.2; done
      if mv "$2" "$4"; then
        mv "$3" "$2" || mv "$4" "$2"
      fi
      xattr -dr com.apple.quarantine "$2" 2>/dev/null
      open -n "$2"
      """
    let swap = Process()
    swap.executableURL = URL(fileURLWithPath: "/bin/sh")
    swap.arguments = ["-c", script, "sh", String(getpid()), here.path, app.path,
                      work.appendingPathComponent("old.app").path]
    swap.standardInput = FileHandle.nullDevice
    swap.standardOutput = FileHandle.nullDevice
    swap.standardError = FileHandle.nullDevice
    try swap.run()
    NSApp.terminate(nil)
  }
}
