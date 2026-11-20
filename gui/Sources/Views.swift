// The window: three tabs, Install, Keys and Run.

import AppKit
import SwiftUI
import UniformTypeIdentifiers

@main
struct CocaineApp: App {
  @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate
  @StateObject private var model = AppModel()

  var body: some Scene {
    WindowGroup("Cocaine") {
      ContentView()
        .environmentObject(model)
        .frame(minWidth: 680, minHeight: 760)
    }
    .windowResizability(.contentMinSize)
  }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
  func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
  func applicationWillTerminate(_ notification: Notification) { RunningCommand.stopAll() }
}

enum Tab: Hashable { case install, keys, run }

struct ContentView: View {
  @EnvironmentObject var model: AppModel
  @State private var tab: Tab = .install
  @State private var chosen = false

  var body: some View {
    TabView(selection: $tab) {
      InstallView().tabItem { Label("Install", systemImage: "hammer") }.tag(Tab.install)
      KeysView().tabItem { Label("Keys", systemImage: "key") }.tag(Tab.keys)
      RunView().tabItem { Label("Run", systemImage: "play") }.tag(Tab.run)
    }
    .padding()
    .onAppear {
      // Straight to running things once everything's set up.
      if !chosen && model.isReady { tab = .run }
      chosen = true
    }
  }
}

// MARK: - Pieces

/// A line of status: a coloured mark, what it's about, and what's the case.
struct StatusRow: View {
  enum State { case good, bad, warn, unknown }
  let title: String
  let detail: String
  let state: State

  var body: some View {
    LabeledContent {
      HStack(spacing: 6) {
        Text(detail).foregroundStyle(.secondary).textSelection(.enabled)
        Image(systemName: symbol).foregroundStyle(color)
      }
    } label: {
      Text(title)
    }
  }
  private var symbol: String {
    switch state {
    case .good: return "checkmark.circle.fill"
    case .bad: return "xmark.circle.fill"
    case .warn: return "exclamationmark.triangle.fill"
    case .unknown: return "questionmark.circle"
    }
  }
  private var color: Color {
    switch state {
    case .good: return .green
    case .bad: return .red
    case .warn: return .orange
    case .unknown: return .secondary
    }
  }
}

/// Command output, following along as it comes.
struct LogView: View {
  @ObservedObject var log: LogBuffer

  var body: some View {
    ScrollViewReader { proxy in
      ScrollView {
        LazyVStack(alignment: .leading, spacing: 1) {
          ForEach(Array(log.lines.enumerated()), id: \.offset) { item in
            Text(item.element.isEmpty ? " " : item.element)
              .font(.system(.caption, design: .monospaced))
              .frame(maxWidth: .infinity, alignment: .leading)
              .textSelection(.enabled)
              .id(item.offset)
          }
        }
        .padding(8)
      }
      .background(Color(nsColor: .textBackgroundColor))
      .clipShape(RoundedRectangle(cornerRadius: 6))
      .overlay(RoundedRectangle(cornerRadius: 6).stroke(.separator))
      .onChange(of: log.lines.count) { _, count in
        if count > 0 { proxy.scrollTo(count - 1, anchor: .bottom) }
      }
    }
  }
}

func short(_ commit: String?) -> String { commit.map { String($0.prefix(7)) } ?? "?" }

func choosePath(directory: Bool, types: [UTType] = [], message: String) -> URL? {
  let panel = NSOpenPanel()
  panel.canChooseDirectories = directory
  panel.canChooseFiles = !directory || !types.isEmpty
  panel.canCreateDirectories = directory
  panel.allowsMultipleSelection = false
  panel.message = message
  if !types.isEmpty { panel.allowedContentTypes = types }
  return panel.runModal() == .OK ? panel.url : nil
}

// MARK: - Install

struct InstallView: View {
  @EnvironmentObject var model: AppModel

  var body: some View {
    VStack(alignment: .leading, spacing: 12) {
      Form {
        Section("cocaine") {
          LabeledContent("Location") {
            HStack {
              Text(model.checkoutPath).lineLimit(1).truncationMode(.middle).foregroundStyle(.secondary)
              Button("Choose…") {
                if let url = choosePath(directory: true, message: "Choose a cocaine checkout, or a folder to download it into") {
                  model.checkoutPath = url.path
                }
              }
            }
          }
          StatusRow(title: "Build tools",
                    detail: model.missingTools.isEmpty ? "Xcode command line tools, git, cmake and ninja" :
                      "missing: " + model.missingTools.joined(separator: ", "),
                    state: model.missingTools.isEmpty ? .good : .bad)
          StatusRow(title: "Source",
                    detail: model.hasCheckout ? "\(model.branch ?? "?") at \(short(model.head))" : "not downloaded",
                    state: model.hasCheckout ? .good : .bad)
          StatusRow(title: "Updates", detail: updateText, state: updateState)
          StatusRow(title: "Build", detail: buildText, state: buildState)
          HStack {
            if !model.hasCheckout {
              Button("Download and Build") { Task { await model.install() } }
                .disabled(!model.missingTools.isEmpty)
            } else {
              Button("Check for Updates") { Task { await model.checkForUpdates() } }
              Button("Update and Rebuild") { Task { await model.update() } }
                .disabled((model.behind ?? 0) == 0 && model.buildIsCurrent)
              Button(model.isBuilt ? "Rebuild" : "Build") { Task { await model.build() } }
            }
          }
          .disabled(model.busy != nil)
        }

        Section("Leopard system root") {
          LabeledContent("Location") {
            HStack {
              Text(model.sysrootPath).lineLimit(1).truncationMode(.middle).foregroundStyle(.secondary)
              Button("Choose…") {
                if let url = choosePath(directory: true, message: "Choose a Leopard system root, or a folder to import one into") {
                  model.sysrootChoice = url.path
                }
              }
              if !model.sysrootChoice.isEmpty {
                Button("Default") { model.sysrootChoice = "" }
              }
            }
          }
          StatusRow(title: "System root",
                    detail: model.sysrootReady ? "ready" : "not imported yet",
                    state: model.sysrootReady ? .good : .bad)
          HStack {
            Button(model.sysrootReady ? "Import again from Disc Image…" : "Import from Disc Image…") {
              let types = ["iso", "dmg", "cdr", "toast", "img"].compactMap { UTType(filenameExtension: $0) } + [.diskImage]
              if let image = choosePath(directory: false, types: types,
                                        message: "Choose a Mac OS X 10.5 Leopard install DVD image") {
                Task { await model.importSysroot(from: image) }
              }
            }
            .disabled(!model.hasCheckout || model.busy != nil)
            Text("A retail Leopard install DVD image - universal, about 7 GB - unpacks about 3.7 GB...")
              .font(.caption).foregroundStyle(.secondary)
          }
        }
      }
      .formStyle(.grouped)
      .frame(minHeight: 520, maxHeight: 560)

      HStack {
        if let busy = model.busy {
          ProgressView().controlSize(.small)
          Text(busy + "…")
        } else {
          Text("Output").foregroundStyle(.secondary)
        }
        Spacer()
        Button("Clear") { model.installLog.clear() }.controlSize(.small)
      }
      LogView(log: model.installLog)
    }
    .onAppear { model.refresh() }
  }

  private var updateText: String {
    guard model.hasCheckout else { return "—" }
    if model.checkFailed { return "could not reach GitHub" }
    guard let behind = model.behind else { return "not checked yet" }
    return behind == 0 ? "up to date" : "\(behind) new commit\(behind == 1 ? "" : "s") on GitHub"
  }
  private var updateState: StatusRow.State {
    guard model.hasCheckout, !model.checkFailed, let behind = model.behind else { return model.checkFailed ? .warn : .unknown }
    return behind == 0 ? .good : .warn
  }
  private var buildText: String {
    guard model.isBuilt else { return "not built" }
    guard let built = model.builtCommit else {
      return model.buildIsCurrent ? "built, newer than the source" : "built, older than the source"
    }
    return model.buildIsCurrent ? "built from \(short(built))" : "built from \(short(built)) - older than the source"
  }
  private var buildState: StatusRow.State {
    guard model.isBuilt else { return .bad }
    return model.buildIsCurrent ? .good : .warn
  }
}

// MARK: - Keys

struct KeysView: View {
  @EnvironmentObject var model: AppModel
  @State private var first = ""
  @State private var second = ""
  @State private var message: String?
  @State private var failed = false

  var body: some View {
    Form {
      Section {
        StatusRow(title: "Keys",
                  detail: model.keysInstalled ? "in place" : "not set",
                  state: model.keysInstalled ? .good : .warn)
        Text("Intel apps with windows need the two keys from an Intel Mac's SMC chip - Apple shipped a few of Leopard's own Intel programs encrypted, and the kernel decrypted them with these. PowerPC apps don't need them. They're saved in the system root, readable only by you, and never shown again or sent anywhere.")
          .font(.callout).foregroundStyle(.secondary)
          .fixedSize(horizontal: false, vertical: true)
      }
      Section("Enter the keys") {
        SecureField("OSK0 (32 characters)", text: $first)
          .onChange(of: first) { _, value in
            // Both pasted at once: split them.
            if value.count == 64 && second.isEmpty {
              second = String(value.suffix(32))
              first = String(value.prefix(32))
            }
          }
        SecureField("OSK1 (32 characters)", text: $second)
        HStack {
          Text("\(first.count)/32 and \(second.count)/32").font(.caption).monospacedDigit().foregroundStyle(.secondary)
          Spacer()
          if model.keysInstalled {
            Button("Remove Keys", role: .destructive) {
              do {
                try model.removeKeys()
                show("The keys file is in the Trash.", failed: false)
              } catch {
                show(error.localizedDescription, failed: true)
              }
            }
          }
          Button("Save Keys") {
            do {
              try model.saveKeys(first, second)
              first = ""
              second = ""
              show("Saved.", failed: false)
            } catch {
              show(error.localizedDescription, failed: true)
            }
          }
          .keyboardShortcut(.defaultAction)
          .disabled(first.isEmpty || second.isEmpty)
        }
        if let message {
          Text(message).font(.callout).foregroundStyle(failed ? .red : .green)
        }
      }
    }
    .formStyle(.grouped)
    .onAppear { model.refresh() }
  }

  private func show(_ text: String, failed: Bool) {
    message = text
    self.failed = failed
  }
}

// MARK: - Run

struct RunView: View {
  @EnvironmentObject var model: AppModel
  @State private var dropping = false

  var body: some View {
    VStack(alignment: .leading, spacing: 12) {
      Form {
        Section {
          HStack(spacing: 14) {
            if let target = model.target {
              Image(nsImage: NSWorkspace.shared.icon(forFile: target.path))
                .resizable().frame(width: 48, height: 48)
              VStack(alignment: .leading, spacing: 2) {
                Text(target.deletingPathExtension().lastPathComponent).font(.headline)
                Text("Has code for " + model.targetArchitectures.description)
                  .font(.caption).foregroundStyle(.secondary)
              }
            } else {
              Image(systemName: "square.dashed").font(.system(size: 36)).foregroundStyle(.secondary)
              Text("Choose an app, or drop one here").foregroundStyle(.secondary)
            }
            Spacer()
            Button("Choose…") {
              if let url = choosePath(directory: false, types: [.application, .unixExecutable, .item],
                                      message: "Choose an old PowerPC or Intel Mac app") {
                model.target = url
              }
            }
          }
          .padding(.vertical, 4)
          .onDrop(of: [.fileURL], isTargeted: $dropping) { providers in
            guard let provider = providers.first else { return false }
            _ = provider.loadObject(ofClass: URL.self) { url, _ in
              if let url { DispatchQueue.main.async { model.target = url } }
            }
            return true
          }
          .overlay(RoundedRectangle(cornerRadius: 8).stroke(dropping ? Color.accentColor : .clear, lineWidth: 2))

          Picker("Run as", selection: $model.mode) {
            ForEach(RunMode.allCases) { mode in
              Text(mode.title).tag(mode)
            }
          }
          .pickerStyle(.segmented)
          if let note = modeNote {
            Text(note).font(.caption).foregroundStyle(.orange)
          }
        }
      }
      .formStyle(.grouped)
      .frame(maxHeight: 220)

      HStack {
        if model.running {
          ProgressView().controlSize(.small)
          Text("Running. Closing its window ends it too.")
          Spacer()
          Button("Stop") { model.stop() }
        } else {
          Text(readiness).foregroundStyle(model.isReady ? .secondary : Color.orange)
          Spacer()
          Button("Run") { Task { await model.run() } }
            .keyboardShortcut(.defaultAction)
            .disabled(!canRun)
        }
      }
      LogView(log: model.runLog)
    }
    .onAppear { model.refresh() }
  }

  private var canRun: Bool {
    guard model.isReady, model.target != nil else { return false }
    let archs = model.targetArchitectures
    if archs.isEmpty || archs == .modern { return false }
    switch model.mode {
    case .auto: return true
    case .powerpc: return archs.contains(.powerpc)
    case .intel: return archs.contains(.intel)  // without the keys, command line programs still run
    }
  }

  private var readiness: String {
    if !model.isBuilt { return "Cocaine isn't built yet - see the install tab." }
    if !model.sysrootReady { return "Import Leopard's system root first - see the install tab." }
    return "Output"
  }

  private var modeNote: String? {
    let archs = model.targetArchitectures
    guard model.target != nil else { return nil }
    if archs.isEmpty || archs == .modern { return "That doesn't look like a PowerPC or 32-bit Intel program..." }
    switch model.mode {
    case .auto:
      if !archs.contains(.powerpc) && archs.contains(.intel) && !model.keysInstalled {
        return "Intel only - apps with windows need the keys (keys tab)."
      }
      return nil
    case .powerpc:
      return archs.contains(.powerpc) ? nil : "This program has no PowerPC code..."
    case .intel:
      if !archs.contains(.intel) { return "This program has no 32-bit Intel code..." }
      return model.keysInstalled ? nil : "Intel apps with windows need the keys (keys tab)."
    }
  }
}
