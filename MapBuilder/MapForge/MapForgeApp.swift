import AppKit
import SwiftUI

/// TRACE ZERO MAP FORGE: a Mac front end for Forge/forge.sh.
@main
struct MapForgeApp: App {
    var body: some Scene {
        WindowGroup("TRACE ZERO MAP FORGE") {
            ContentView()
                .frame(minWidth: 820, minHeight: 560)
                .preferredColorScheme(.dark)
        }
        .windowStyle(.hiddenTitleBar)
    }
}

private let cyan = Color(red: 0, green: 0.8, blue: 1)
private let cyanDim = Color(red: 0, green: 0.8, blue: 1).opacity(0.5)

struct ContentView: View {
    @StateObject private var catalog = RegionCatalog()
    @StateObject private var runner = ForgeRunner()
    @State private var query = ""
    @State private var selection: Region?
    @State private var localFile: URL?
    @State private var name = ""
    @State private var walk = true
    @State private var bike = true
    @State private var car = true
    @State private var includeMap = true
    @State private var includeSearch = true
    @State private var freshDownload = false
    @AppStorage("outputFolder") private var outputFolder =
        FileManager.default.urls(for: .desktopDirectory, in: .userDomainMask)[0].path

    private var filtered: [Region] {
        let q = query.trimmingCharacters(in: .whitespaces).lowercased()
        guard !q.isEmpty else { return catalog.regions }
        return catalog.regions.filter { $0.path.lowercased().contains(q) || $0.id.contains(q) }
    }

    var body: some View {
        HStack(spacing: 0) {
            regionList
                .frame(width: 330)
            Rectangle().fill(cyanDim).frame(width: 1)
            options
                .padding(20)
        }
        .background(Color.black)
        .foregroundColor(cyan)
        .tint(cyan)
        .task { await catalog.load() }
    }

    // MARK: region list

    private var regionList: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("TRACE//ZERO").font(.system(size: 22, weight: .heavy, design: .monospaced))
            Text("MAP FORGE").font(.system(size: 13, weight: .bold, design: .monospaced)).kerning(3)
            TextField("Search regions (country, state, city)", text: $query)
                .textFieldStyle(.roundedBorder)
                .padding(.top, 8)
            if catalog.loading { ProgressView().frame(maxWidth: .infinity) }
            if let e = catalog.error { Text(e).font(.caption).foregroundColor(.red) }
            List(filtered, id: \.self, selection: Binding(get: { selection }, set: { pick($0) })) { r in
                VStack(alignment: .leading, spacing: 2) {
                    Text(r.name).font(.system(size: 13, weight: .semibold))
                    Text(r.path).font(.system(size: 10)).foregroundColor(cyanDim)
                }
                .tag(r)
            }
            .listStyle(.plain)
            Button("Use a local .osm.pbf file…") { chooseFile() }
                .buttonStyle(.link)
            if let f = localFile {
                Text(f.lastPathComponent).font(.caption)
            }
        }
        .padding(16)
    }

    private func pick(_ r: Region?) {
        selection = r
        if let r = r {
            localFile = nil
            name = r.name
        }
    }

    private func chooseFile() {
        let panel = NSOpenPanel()
        panel.allowsMultipleSelection = false
        panel.canChooseDirectories = false
        panel.message = "Choose an OpenStreetMap extract (.osm.pbf)"
        if panel.runModal() == .OK, let u = panel.url {
            localFile = u
            selection = nil
            name = u.lastPathComponent.replacingOccurrences(of: ".osm.pbf", with: "")
                .replacingOccurrences(of: "-latest", with: "").capitalized
        }
    }

    private func chooseFolder() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.canCreateDirectories = true
        panel.directoryURL = URL(fileURLWithPath: outputFolder)
        if panel.runModal() == .OK, let u = panel.url { outputFolder = u.path }
    }

    // MARK: options and progress

    private var options: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text(selection?.path ?? localFile?.lastPathComponent ?? "Choose a region on the left")
                .font(.system(size: 18, weight: .bold))
            Group {
                HStack {
                    Text("Name in the app").frame(width: 140, alignment: .leading)
                    TextField("Name", text: $name).textFieldStyle(.roundedBorder)
                }
                HStack {
                    Text("Routing").frame(width: 140, alignment: .leading)
                    Toggle("Walk", isOn: $walk)
                    Toggle("Bike", isOn: $bike)
                    Toggle("Car", isOn: $car)
                }
                HStack {
                    Text("Contents").frame(width: 140, alignment: .leading)
                    Toggle("Map", isOn: $includeMap)
                    Toggle("Search", isOn: $includeSearch)
                    Toggle("Download again", isOn: $freshDownload)
                }
                HStack {
                    Text("Save to").frame(width: 140, alignment: .leading)
                    Text(outputFolder).lineLimit(1).truncationMode(.middle).foregroundColor(cyanDim)
                    Spacer()
                    Button("Change…") { chooseFolder() }
                }
            }
            .disabled(runner.isRunning)

            HStack {
                Button(action: start) {
                    Text(runner.isRunning ? "FORGING…" : "FORGE")
                        .font(.system(size: 15, weight: .heavy, design: .monospaced)).kerning(3)
                        .frame(width: 160, height: 30)
                }
                .disabled(runner.isRunning || (selection == nil && localFile == nil) || !(walk || bike || car))
                .keyboardShortcut(.defaultAction)
                if runner.isRunning {
                    Button("Cancel") { runner.cancel() }
                }
            }

            statusView

            ScrollViewReader { proxy in
                ScrollView {
                    VStack(alignment: .leading, spacing: 1) {
                        ForEach(Array(runner.log.enumerated()), id: \.offset) { i, line in
                            Text(line).font(.system(size: 10, design: .monospaced)).foregroundColor(cyanDim)
                                .frame(maxWidth: .infinity, alignment: .leading).id(i)
                        }
                    }
                }
                .onChange(of: runner.log.count) { n in proxy.scrollTo(n - 1, anchor: .bottom) }
            }
            .padding(8)
            .overlay(RoundedRectangle(cornerRadius: 4).stroke(cyanDim))

            Text("First run downloads Planetiler, a Java runtime if needed and ~1.5 GB of shared ocean and " +
                 "landscape data into ~/Library/Caches/TraceZeroForge. Map data © OpenStreetMap contributors (ODbL).")
                .font(.system(size: 10)).foregroundColor(cyanDim)
        }
    }

    @ViewBuilder private var statusView: some View {
        switch runner.state {
        case .idle:
            EmptyView()
        case .running:
            VStack(alignment: .leading, spacing: 4) {
                if let f = runner.fraction { ProgressView(value: f) } else { ProgressView() }
                Text(runner.stepTitle + (runner.detail.isEmpty ? "" : " · " + runner.detail)).font(.caption)
            }
        case .finished(let url):
            HStack {
                Text("✓ \(url.lastPathComponent) is ready. In Finder, select your iPhone → Files and drag it onto TRACE ZERO.")
                    .font(.system(size: 12, weight: .semibold))
                Button("Show in Finder") { NSWorkspace.shared.activateFileViewerSelecting([url]) }
            }
        case .failed(let message):
            Text("✗ " + message).foregroundColor(.red).font(.system(size: 12, weight: .semibold))
        case .cancelled:
            Text("Cancelled").font(.caption)
        }
    }

    private func start() {
        var args: [String] = []
        if let f = localFile {
            args += ["--file", f.path]
        } else if let r = selection {
            args.append(r.id)
        } else {
            return
        }
        let profiles = [car ? "car" : nil, bike ? "bike" : nil, walk ? "foot" : nil].compactMap { $0 }
        args += ["--out", outputFolder, "--profiles", profiles.joined(separator: ",")]
        if !name.trimmingCharacters(in: .whitespaces).isEmpty { args += ["--name", name] }
        if !includeMap { args.append("--no-tiles") }
        if !includeSearch { args.append("--no-search") }
        if freshDownload { args.append("--fresh") }
        runner.run(arguments: args)
    }
}
