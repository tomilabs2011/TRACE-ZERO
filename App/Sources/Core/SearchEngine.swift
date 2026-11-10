import CoreLocation
import Foundation

struct SearchResult {
    enum Kind { case place, street, address, poi, coordinate }

    let title: String
    let subtitle: String
    let coordinate: CLLocationCoordinate2D
    let kind: Kind
    /// Place type or POI group from tz_core.h (TZ_PLACE_*, TZ_POI_*).
    let sub: Int
    let distance: Double?
    let score: Double

    /// Short tag shown in front of the result.
    var tag: String {
        switch kind {
        case .place: return "PLACE"
        case .street: return "STREET"
        case .address: return "ADDR"
        case .poi: return "POI"
        case .coordinate: return "GPS"
        }
    }
}

/// Offline search over all installed packs. Typing is debounced by the
/// caller; queries run on a background queue and a newer query always wins.
final class SearchEngine {
    static let shared = SearchEngine()
    private let queue = DispatchQueue(label: "tracezero.search", qos: .userInitiated)
    private var generation = 0   // main thread only

    /// Results arrive on the main queue; stale answers are dropped.
    func search(_ text: String, near: CLLocationCoordinate2D?, limit: Int = 40,
                completion: @escaping ([SearchResult]) -> Void) {
        generation += 1
        let gen = generation
        let packs = PackStore.shared.packs
        queue.async {
            let results = SearchEngine.run(text, near: near, packs: packs, limit: limit)
            DispatchQueue.main.async {
                if gen == self.generation { completion(results) }
            }
        }
    }

    static func run(_ raw: String, near: CLLocationCoordinate2D?, packs: [MapPack], limit: Int) -> [SearchResult] {
        let text = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !text.isEmpty else { return [] }
        var out: [SearchResult] = []

        // Coordinates first: "47.4979, 19.0402", "N47 29.873 E19 2.412", ...
        var lat = 0.0, lon = 0.0
        if tz_parse_coordinate(text, &lat, &lon) == 1 {
            let c = CLLocationCoordinate2D(latitude: lat, longitude: lon)
            let dist = near.map { tz_distance_m($0.latitude, $0.longitude, lat, lon) }
            out.append(SearchResult(title: String(format: "%.6f, %.6f", lat, lon), subtitle: "Coordinate",
                                    coordinate: c, kind: .coordinate, sub: 0, distance: dist, score: 1e9))
        }

        var hits = [tz_hit](repeating: tz_hit(), count: limit)
        for pack in packs {
            guard let s = pack.searchHandle() else { continue }
            let n = Int(tz_search_query(s, text, near?.latitude ?? 0, near?.longitude ?? 0,
                                        near == nil ? 0 : 1, &hits, Int32(limit)))
            for i in 0..<n {
                let h = hits[i]
                let kind: SearchResult.Kind
                switch Int(h.kind) {
                case Int(TZ_HIT_PLACE): kind = .place
                case Int(TZ_HIT_STREET): kind = .street
                case Int(TZ_HIT_ADDRESS): kind = .address
                default: kind = .poi
                }
                out.append(SearchResult(title: h.name.map { String(cString: $0) } ?? "",
                                        subtitle: h.context.map { String(cString: $0) } ?? "",
                                        coordinate: CLLocationCoordinate2D(latitude: h.lat, longitude: h.lon),
                                        kind: kind, sub: Int(h.sub),
                                        distance: h.dist_m >= 0 ? h.dist_m : nil, score: h.score))
            }
        }
        // Several packs may overlap (a city inside a country): drop duplicates.
        var seen = Set<String>()
        out = out.sorted { $0.score > $1.score }.filter { r in
            let key = "\(r.title)|\(r.subtitle)|\(Int(r.coordinate.latitude * 2000))|\(Int(r.coordinate.longitude * 2000))"
            return seen.insert(key).inserted
        }
        return Array(out.prefix(limit))
    }

    /// Best description of a point: an address or POI nearby, else the
    /// nearest named road, else the coordinates. Synchronous and fast.
    static func describe(_ c: CLLocationCoordinate2D) -> (title: String, subtitle: String) {
        for pack in PackStore.shared.packs(containing: c) {
            var hit = tz_hit()
            if let s = pack.searchHandle(), tz_search_nearest(s, c.latitude, c.longitude, 60, &hit) == 1 {
                let name = hit.name.map { String(cString: $0) } ?? ""
                let ctx = hit.context.map { String(cString: $0) } ?? ""
                if !name.isEmpty { return (name, ctx) }
            }
            if let r = try? pack.roadsHandle() {
                var d = 0.0
                if let name = tz_roads_nearest_name(r, c.latitude, c.longitude, 80, &d) {
                    let s = String(cString: name)
                    if !s.isEmpty { return (s, pack.name) }
                }
            }
        }
        return (String(format: "%.5f, %.5f", c.latitude, c.longitude), "Dropped pin")
    }
}
