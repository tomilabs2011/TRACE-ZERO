import CoreLocation
import Foundation

enum ManeuverKind {
    case depart, turn, continueOn, fork, ramp, roundabout, ferry, arrive
}

/// One instruction along a route.
struct Maneuver {
    let kind: ManeuverKind
    /// -3 sharp left ... 0 straight ... +3 sharp right, 4 = U-turn
    let modifier: Int
    /// Roundabout exit number (1-based); 0 = destination is inside the roundabout.
    let exit: Int
    let pointIndex: Int
    let distanceFromStart: Double
    let timeFromStart: Double
    let bearingBefore: Double
    let bearingAfter: Double
    let name: String
    let ref: String
    let coordinate: CLLocationCoordinate2D

    /// Road name, or route number, or nothing.
    var road: String { return !name.isEmpty ? name : ref }

    /// Turn angle used to draw the arrow glyph.
    var glyphAngle: Double {
        switch kind {
        case .roundabout: return Maneuver.normalize(bearingAfter - bearingBefore)
        default:
            switch modifier {
            case -3: return -140
            case -2: return -90
            case -1: return -45
            case 1: return 45
            case 2: return 90
            case 3: return 140
            default: return 0
            }
        }
    }

    static func normalize(_ a: Double) -> Double {
        var x = a.truncatingRemainder(dividingBy: 360)
        if x > 180 { x -= 360 }
        if x <= -180 { x += 360 }
        return x
    }
}

/// A computed route. Immutable; safe to pass between threads.
final class Route {
    let mode: TravelMode
    let packID: String
    let coordinates: [CLLocationCoordinate2D]
    let cumulativeDistance: [Double]   // metres from start, per point
    let cumulativeTime: [Double]       // seconds from start, per point
    let maneuvers: [Maneuver]
    let distance: Double
    let duration: Double
    let computeMilliseconds: Double
    let destination: CLLocationCoordinate2D

    init(mode: TravelMode, packID: String, coordinates: [CLLocationCoordinate2D], cumulativeDistance: [Double],
         cumulativeTime: [Double], maneuvers: [Maneuver], computeMilliseconds: Double,
         destination: CLLocationCoordinate2D) {
        self.mode = mode
        self.packID = packID
        self.coordinates = coordinates
        self.cumulativeDistance = cumulativeDistance
        self.cumulativeTime = cumulativeTime
        self.maneuvers = maneuvers
        self.distance = cumulativeDistance.last ?? 0
        self.duration = cumulativeTime.last ?? 0
        self.computeMilliseconds = computeMilliseconds
        self.destination = destination
    }
}

enum RouteError: Error, CustomStringConvertible {
    case noPack
    case profileMissing(TravelMode)
    case noStart, noEnd, noRoute
    case failed(String)

    var description: String {
        switch self {
        case .noPack: return "No installed map covers both the start and the destination."
        case .profileMissing(let m): return "This map pack has no \(m.title.lowercased()) routing."
        case .noStart: return "No road near the start."
        case .noEnd: return "No road near the destination."
        case .noRoute: return "No route between these points."
        case .failed(let s): return s
        }
    }
}

/// Computes routes on a background queue. Queries take milliseconds even on
/// an iPhone 5s thanks to the contraction hierarchy (see docs/ROUTING.md).
final class Router {
    static let shared = Router()
    private let queue = DispatchQueue(label: "tracezero.routing", qos: .userInitiated)

    /// Calls completion on the main queue.
    func route(from: CLLocationCoordinate2D, heading: CLLocationDirection?, to: CLLocationCoordinate2D,
               mode: TravelMode, completion: @escaping (Result<Route, RouteError>) -> Void) {
        guard let pack = PackStore.shared.pack(for: from, and: to) else {
            completion(.failure(.noPack))
            return
        }
        queue.async {
            let result = Router.compute(pack: pack, from: from, heading: heading, to: to, mode: mode)
            DispatchQueue.main.async { completion(result) }
        }
    }

    /// Synchronous version (call off the main thread).
    static func compute(pack: MapPack, from: CLLocationCoordinate2D, heading: CLLocationDirection?,
                        to: CLLocationCoordinate2D, mode: TravelMode) -> Result<Route, RouteError> {
        guard pack.has(mode) else { return .failure(.profileMissing(mode)) }
        let graph: OpaquePointer
        do {
            graph = try pack.graphHandle(mode)
        } catch {
            return .failure(.failed("\(error)"))
        }
        var a = tz_waypoint(lat: from.latitude, lon: from.longitude, heading: heading ?? -1, radius_m: 0)
        var b = tz_waypoint(lat: to.latitude, lon: to.longitude, heading: -1, radius_m: 0)
        var out: UnsafeMutablePointer<tz_route>? = nil
        let status = tz_route_compute(graph, &a, &b, &out)
        guard status == Int32(TZ_OK.rawValue), let r = out else {
            switch status {
            case Int32(TZ_ERR_NO_START.rawValue): return .failure(.noStart)
            case Int32(TZ_ERR_NO_END.rawValue): return .failure(.noEnd)
            case Int32(TZ_ERR_NO_ROUTE.rawValue): return .failure(.noRoute)
            default: return .failure(.failed(MapPack.message(status)))
            }
        }
        defer { tz_route_free(r) }
        let rt = r.pointee
        let n = Int(rt.npts)
        var coords = [CLLocationCoordinate2D]()
        coords.reserveCapacity(n)
        var dist = [Double](repeating: 0, count: n)
        var time = [Double](repeating: 0, count: n)
        for i in 0..<n {
            coords.append(CLLocationCoordinate2D(latitude: rt.lat[i], longitude: rt.lon[i]))
            dist[i] = rt.cum_dist[i]
            time[i] = rt.cum_time[i]
        }
        var mans = [Maneuver]()
        for i in 0..<Int(rt.nman) {
            let m = rt.man[i]
            let idx = min(max(0, Int(m.point)), n - 1)
            mans.append(Maneuver(kind: kind(m.type), modifier: Int(m.modifier), exit: Int(m.exit),
                                 pointIndex: idx, distanceFromStart: m.dist_m, timeFromStart: m.time_s,
                                 bearingBefore: m.bearing_before, bearingAfter: m.bearing_after,
                                 name: m.name.map { String(cString: $0) } ?? "",
                                 ref: m.ref.map { String(cString: $0) } ?? "",
                                 coordinate: coords[idx]))
        }
        return .success(Route(mode: mode, packID: pack.id, coordinates: coords, cumulativeDistance: dist,
                              cumulativeTime: time, maneuvers: mans, computeMilliseconds: rt.compute_ms,
                              destination: to))
    }

    private static func kind(_ t: UInt8) -> ManeuverKind {
        switch UInt32(t) {
        case TZ_MAN_DEPART.rawValue: return .depart
        case TZ_MAN_TURN.rawValue: return .turn
        case TZ_MAN_CONTINUE.rawValue: return .continueOn
        case TZ_MAN_FORK.rawValue: return .fork
        case TZ_MAN_RAMP.rawValue: return .ramp
        case TZ_MAN_ROUNDABOUT.rawValue: return .roundabout
        case TZ_MAN_FERRY.rawValue: return .ferry
        case TZ_MAN_ARRIVE.rawValue: return .arrive
        default: return .turn
        }
    }
}
