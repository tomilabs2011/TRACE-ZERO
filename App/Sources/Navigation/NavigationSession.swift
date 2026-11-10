import CoreLocation
import Foundation

/// Where the traveller is relative to the route.
struct NavProgress {
    /// Position on the route closest to the GPS fix.
    let snapped: CLLocationCoordinate2D
    /// Direction of the route at that point (degrees).
    let routeBearing: Double
    /// Polyline segment index the snapped point lies on.
    let segment: Int
    let distanceAlong: Double
    let remainingDistance: Double
    let remainingTime: Double
    /// Index into route.maneuvers of the next instruction (never `depart`).
    let nextManeuver: Int
    let distanceToNextManeuver: Double
    /// Distance of the GPS fix from the route line.
    let offset: Double
    let offRoute: Bool
    let arrived: Bool
}

/// Follows a route: snaps GPS fixes onto it, measures progress, detects
/// leaving the route and arriving. Pure logic, no UI and no timers, so the
/// same class drives the phone screen and CarPlay (and can be unit tested).
final class NavigationSession {
    private(set) var route: Route
    private(set) var progress: NavProgress?

    private var lastSegment = 0
    private var offSince: Date?
    private var offCount = 0
    /// Projected (metres) copy of the route around a reference point.
    private var proj: Projection

    init(route: Route) {
        self.route = route
        self.proj = Projection(route.coordinates)
    }

    /// Replace the route after a reroute.
    func replace(_ r: Route) {
        route = r
        proj = Projection(r.coordinates)
        lastSegment = 0
        offSince = nil
        offCount = 0
        progress = nil
    }

    /// Off-route threshold in metres for a fix of the given accuracy.
    private func threshold(_ accuracy: Double) -> Double {
        let base: Double
        switch route.mode {
        case .car: base = 35
        case .bike: base = 30
        case .foot: base = 30
        }
        return base + min(max(accuracy, 0), 40) * 0.6
    }

    @discardableResult
    func update(_ loc: CLLocation) -> NavProgress {
        let n = route.coordinates.count
        let p = proj.xy(loc.coordinate)

        // Search a window ahead of the last position first (routes can cross
        // themselves; looking near the last match avoids jumping), then the
        // whole route if nothing close is found.
        var best = proj.nearest(p, from: max(0, lastSegment - 3), to: min(n - 1, lastSegment + 120))
        if best.distance > threshold(loc.horizontalAccuracy) {
            let all = proj.nearest(p, from: 0, to: n - 1)
            if all.distance < best.distance - 5 { best = all }
        }
        lastSegment = best.segment

        let seg = best.segment
        let segLen = route.cumulativeDistance[seg + 1] - route.cumulativeDistance[seg]
        let along = route.cumulativeDistance[seg] + segLen * best.t
        let timeAt = route.cumulativeTime[seg] + (route.cumulativeTime[seg + 1] - route.cumulativeTime[seg]) * best.t
        let a = route.coordinates[seg], b = route.coordinates[seg + 1]
        let snapped = CLLocationCoordinate2D(latitude: a.latitude + (b.latitude - a.latitude) * best.t,
                                             longitude: a.longitude + (b.longitude - a.longitude) * best.t)
        let bearing = tz_bearing_deg(a.latitude, a.longitude, b.latitude, b.longitude)

        // next maneuver strictly ahead of us
        var next = route.maneuvers.count - 1
        for (i, m) in route.maneuvers.enumerated() where m.kind != .depart {
            if m.distanceFromStart > along + 1 { next = i; break }
        }
        let toNext = max(0, route.maneuvers[next].distanceFromStart - along)

        // off route: far from the line for two fixes and three seconds
        let off = best.distance > threshold(loc.horizontalAccuracy)
        if off {
            offCount += 1
            if offSince == nil { offSince = loc.timestamp }
        } else {
            offCount = 0
            offSince = nil
        }
        let offRoute = off && offCount >= 2 && (offSince.map { loc.timestamp.timeIntervalSince($0) >= 3 } ?? false)

        let remaining = max(0, route.distance - along)
        let toDest = tz_distance_m(loc.coordinate.latitude, loc.coordinate.longitude,
                                   route.destination.latitude, route.destination.longitude)
        let arriveRadius = route.mode == .car ? 30.0 : 15.0
        let arrived = remaining < arriveRadius || (toDest < arriveRadius && remaining < 150)

        let prog = NavProgress(snapped: snapped, routeBearing: bearing, segment: seg, distanceAlong: along,
                               remainingDistance: remaining, remainingTime: max(0, route.duration - timeAt),
                               nextManeuver: next, distanceToNextManeuver: toNext, offset: best.distance,
                               offRoute: offRoute, arrived: arrived)
        progress = prog
        return prog
    }
}

/// Route polyline projected to local metres for fast point-to-line distances.
private struct Projection {
    private let lat0: Double, lon0: Double, kx: Double, ky: Double
    private let xs: [Double], ys: [Double]

    init(_ coords: [CLLocationCoordinate2D]) {
        let mid = coords.isEmpty ? CLLocationCoordinate2D(latitude: 0, longitude: 0) : coords[coords.count / 2]
        lat0 = mid.latitude
        lon0 = mid.longitude
        ky = 6371008.8 * Double.pi / 180
        kx = ky * cos(lat0 * Double.pi / 180)
        var x = [Double](), y = [Double]()
        x.reserveCapacity(coords.count)
        y.reserveCapacity(coords.count)
        for c in coords {
            x.append((c.longitude - mid.longitude) * kx)
            y.append((c.latitude - mid.latitude) * ky)
        }
        xs = x
        ys = y
    }

    func xy(_ c: CLLocationCoordinate2D) -> (Double, Double) {
        return ((c.longitude - lon0) * kx, (c.latitude - lat0) * ky)
    }

    /// Closest segment in [from, to) to point p.
    func nearest(_ p: (Double, Double), from: Int, to: Int) -> (segment: Int, t: Double, distance: Double) {
        var best = (segment: max(0, min(from, xs.count - 2)), t: 0.0, distance: Double.greatestFiniteMagnitude)
        guard xs.count >= 2 else { return best }
        let hi = min(to, xs.count - 1)
        var i = max(0, from)
        while i < hi {
            let ax = xs[i], ay = ys[i], bx = xs[i + 1], by = ys[i + 1]
            let dx = bx - ax, dy = by - ay
            let l2 = dx * dx + dy * dy
            var t = 0.0
            if l2 > 0 { t = max(0, min(1, ((p.0 - ax) * dx + (p.1 - ay) * dy) / l2)) }
            let cx = ax + t * dx - p.0, cy = ay + t * dy - p.1
            let d = (cx * cx + cy * cy).squareRoot()
            // Prefer the later segment on ties so progress never goes backwards at joints.
            if d < best.distance - 0.01 || (abs(d - best.distance) <= 0.01 && i > best.segment) {
                best = (i, t, d)
            }
            i += 1
        }
        return best
    }
}
