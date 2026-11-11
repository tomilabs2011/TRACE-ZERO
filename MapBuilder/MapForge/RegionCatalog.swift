import Foundation

/// One downloadable extract from Geofabrik.
struct Region: Identifiable, Hashable {
    let id: String          // e.g. "hungary", "berlin"
    let name: String
    let path: String        // "Europe › Germany › Berlin"
    let pbfURL: URL
}

/// Loads Geofabrik's list of regions (shared cache with forge.sh).
@MainActor
final class RegionCatalog: ObservableObject {
    @Published private(set) var regions: [Region] = []
    @Published private(set) var error: String?
    @Published private(set) var loading = false

    static let indexURL = URL(string: "https://download.geofabrik.de/index-v1-nogeom.json")!
    static var cacheFile: URL {
        let dir = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("TraceZeroForge", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir.appendingPathComponent("geofabrik-index.json")
    }

    private struct Index: Decodable {
        struct Feature: Decodable { let properties: Props }
        struct Props: Decodable {
            let id: String
            let name: String
            let parent: String?
            let urls: [String: String]
        }
        let features: [Feature]
    }

    func load() async {
        loading = true
        error = nil
        defer { loading = false }
        let file = Self.cacheFile
        var data: Data?
        let fresh = (try? file.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate)
            .map { Date().timeIntervalSince($0) < 86400 } ?? false
        if fresh { data = try? Data(contentsOf: file) }
        if data == nil {
            do {
                let (d, _) = try await URLSession.shared.data(from: Self.indexURL)
                try? d.write(to: file, options: .atomic)
                data = d
            } catch {
                data = try? Data(contentsOf: file)   // stale copy is better than nothing
                if data == nil { self.error = "Could not load the region list: \(error.localizedDescription)" }
            }
        }
        guard let d = data, let index = try? JSONDecoder().decode(Index.self, from: d) else {
            if error == nil { error = "The region list is unreadable." }
            return
        }
        var byID: [String: Index.Props] = [:]
        for f in index.features { byID[f.properties.id] = f.properties }
        func path(_ p: Index.Props) -> String {
            var parts = [p.name]
            var cur = p.parent.flatMap { byID[$0] }
            var guardCount = 0
            while let c = cur, guardCount < 10 {
                parts.insert(c.name, at: 0)
                cur = c.parent.flatMap { byID[$0] }
                guardCount += 1
            }
            return parts.joined(separator: " › ")
        }
        regions = index.features.compactMap { f in
            guard let s = f.properties.urls["pbf"], let u = URL(string: s) else { return nil }
            return Region(id: f.properties.id, name: f.properties.name, path: path(f.properties), pbfURL: u)
        }.sorted { $0.path < $1.path }
    }
}
