import CoreLocation
import Foundation

/// manifest.json written by tzforge (see docs/FILE-FORMATS.md).
struct PackManifest: Decodable {
    struct FileInfo: Decodable {
        let path: String
        let bytes: Int64
    }

    let format: Int
    let id: String
    let name: String
    let created: String?
    let osm_timestamp: String?
    let source: String?
    let bbox: [Double]          // min lon, min lat, max lon, max lat
    let files: [String: FileInfo]
}

enum PackError: Error, CustomStringConvertible {
    case missingManifest
    case badManifest(String)
    case unsupportedFormat(Int)
    case core(String)

    var description: String {
        switch self {
        case .missingManifest: return "manifest.json is missing"
        case .badManifest(let why): return "manifest.json is unreadable (\(why))"
        case .unsupportedFormat(let v):
            return "Built for map format \(v); this app reads format \(tzFormatVersion). Rebuild the pack."
        case .core(let why): return why
        }
    }
}

/// Map format this build understands (TZ_FORMAT_VERSION in Core/tz_format.h).
let tzFormatVersion = 1

/// One installed `.vmap` folder. Routing and search files are memory-mapped
/// on first use and stay mapped until the pack is released; the C handles are
/// read-only and safe to use from any thread.
final class MapPack {
    let url: URL
    let manifest: PackManifest
    let minLat, minLon, maxLat, maxLon: Double

    private let lock = NSLock()
    private var roads: OpaquePointer?
    private var graphs: [TravelMode: OpaquePointer] = [:]
    private var search: OpaquePointer?
    private var searchFailed = false

    var id: String { return manifest.id }
    var name: String { return manifest.name }
    var tilesURL: URL? {
        guard let f = manifest.files["tiles"] else { return nil }
        let u = url.appendingPathComponent(f.path)
        return FileManager.default.fileExists(atPath: u.path) ? u : nil
    }
    var totalBytes: Int64 { return manifest.files.values.reduce(0) { $0 + $1.bytes } }

    init(url: URL) throws {
        self.url = url
        let murl = url.appendingPathComponent("manifest.json")
        guard let data = try? Data(contentsOf: murl) else { throw PackError.missingManifest }
        do {
            manifest = try JSONDecoder().decode(PackManifest.self, from: data)
        } catch {
            throw PackError.badManifest(error.localizedDescription)
        }
        guard manifest.format == tzFormatVersion else { throw PackError.unsupportedFormat(manifest.format) }
        guard manifest.bbox.count == 4 else { throw PackError.badManifest("bbox") }
        minLon = manifest.bbox[0]
        minLat = manifest.bbox[1]
        maxLon = manifest.bbox[2]
        maxLat = manifest.bbox[3]
    }

    deinit {
        for (_, g) in graphs { tz_graph_close(g) }
        if let r = roads { tz_roads_close(r) }
        if let s = search { tz_search_close(s) }
    }

    func contains(_ c: CLLocationCoordinate2D, margin: Double = 0.02) -> Bool {
        return c.latitude >= minLat - margin && c.latitude <= maxLat + margin &&
            c.longitude >= minLon - margin && c.longitude <= maxLon + margin
    }

    func intersects(minLat a: Double, minLon b: Double, maxLat c: Double, maxLon d: Double) -> Bool {
        return !(c < minLat || a > maxLat || d < minLon || b > maxLon)
    }

    var center: CLLocationCoordinate2D {
        return CLLocationCoordinate2D(latitude: (minLat + maxLat) / 2, longitude: (minLon + maxLon) / 2)
    }

    func has(_ mode: TravelMode) -> Bool {
        return manifest.files[fileKey(mode)] != nil
    }

    private func fileKey(_ mode: TravelMode) -> String {
        switch mode {
        case .car: return "car"
        case .bike: return "bike"
        case .foot: return "foot"
        }
    }

    // MARK: handles (opened lazily, under the lock)

    func roadsHandle() throws -> OpaquePointer {
        lock.lock()
        defer { lock.unlock() }
        return try openRoadsLocked()
    }

    private func openRoadsLocked() throws -> OpaquePointer {
        if let r = roads { return r }
        var status: Int32 = 0
        let path = url.appendingPathComponent("roads.tzr").path
        guard let r = tz_roads_open(path, &status) else { throw PackError.core(Self.message(status)) }
        roads = r
        return r
    }

    func graphHandle(_ mode: TravelMode) throws -> OpaquePointer {
        lock.lock()
        defer { lock.unlock() }
        if let g = graphs[mode] { return g }
        let r = try openRoadsLocked()
        var status: Int32 = 0
        let path = url.appendingPathComponent(mode.graphFile).path
        guard let g = tz_graph_open(r, path, &status) else { throw PackError.core(Self.message(status)) }
        graphs[mode] = g
        return g
    }

    func searchHandle() -> OpaquePointer? {
        lock.lock()
        defer { lock.unlock() }
        if let s = search { return s }
        if searchFailed { return nil }
        var status: Int32 = 0
        let path = url.appendingPathComponent("search.tzs").path
        search = tz_search_open(path, &status)
        if search == nil { searchFailed = true }
        return search
    }

    static func message(_ status: Int32) -> String {
        guard let c = tz_status_message(status) else { return "Error \(status)" }
        return String(cString: c)
    }
}
