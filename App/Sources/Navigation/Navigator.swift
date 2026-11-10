import CoreLocation
import UIKit

/// The active navigation, shared by the phone UI and CarPlay. Owns the
/// session, listens to GPS, reroutes, speaks and detects arrival; screens
/// only observe `Navigator.didUpdate` and read the state.
final class Navigator {
    static let shared = Navigator()
    static let didUpdate = Notification.Name("TraceZeroNavigatorDidUpdate")
    static let didStart = Notification.Name("TraceZeroNavigatorDidStart")
    static let didEnd = Notification.Name("TraceZeroNavigatorDidEnd")

    private(set) var session: NavigationSession?
    private(set) var destinationName = ""
    private(set) var lastLocation: CLLocation?
    /// Location to show for the user: snapped onto the route when on it.
    private(set) var displayLocation: CLLocation?
    private(set) var isRerouting = false
    private(set) var rerouteError: String?
    private(set) var arrived = false

    private var observer: UUID?
    private var lastReroute = Date.distantPast
    private var arrivalTimer: Timer?

    var isActive: Bool { return session != nil }
    var route: Route? { return session?.route }
    var progress: NavProgress? { return session?.progress }

    func start(route: Route, destinationName: String) {
        stop(notify: false)
        session = NavigationSession(route: route)
        self.destinationName = destinationName
        arrived = false
        rerouteError = nil
        LocationService.shared.travelMode = route.mode
        LocationService.shared.set(.navigate)
        observer = LocationService.shared.observe { [weak self] loc in self?.handle(loc) }
        VoiceGuide.shared.reset()
        if let first = route.maneuvers.first {
            VoiceGuide.shared.say(Instructions.sentence(first))
        }
        updateIdleTimer()
        if let loc = LocationService.shared.last { handle(loc) }
        NotificationCenter.default.post(name: Navigator.didStart, object: self)
        NotificationCenter.default.post(name: Navigator.didUpdate, object: self)
    }

    func stop(notify: Bool = true) {
        let wasActive = session != nil
        if let o = observer { LocationService.shared.remove(o) }
        observer = nil
        arrivalTimer?.invalidate()
        arrivalTimer = nil
        session = nil
        isRerouting = false
        displayLocation = nil
        VoiceGuide.shared.stop()
        LocationService.shared.set(LocationService.shared.authorized ? .browse : .off)
        UIApplication.shared.isIdleTimerDisabled = false
        if notify && wasActive { NotificationCenter.default.post(name: Navigator.didEnd, object: self) }
    }

    func updateIdleTimer() {
        UIApplication.shared.isIdleTimerDisabled = isActive && Settings.shared.keepAwake
    }

    private func handle(_ loc: CLLocation) {
        guard let s = session else { return }
        lastLocation = loc
        let p = s.update(loc)

        if p.offRoute {
            displayLocation = loc
        } else {
            // Draw the arrow on the line, pointing along the route.
            displayLocation = CLLocation(coordinate: p.snapped, altitude: loc.altitude,
                                         horizontalAccuracy: loc.horizontalAccuracy,
                                         verticalAccuracy: loc.verticalAccuracy,
                                         course: p.routeBearing, speed: max(0, loc.speed),
                                         timestamp: loc.timestamp)
        }

        if p.arrived && !arrived {
            arrived = true
            VoiceGuide.shared.say("You have arrived at your destination")
            arrivalTimer = Timer.scheduledTimer(withTimeInterval: 20, repeats: false) { [weak self] _ in
                self?.stop()
            }
        } else if !arrived {
            VoiceGuide.shared.update(route: s.route, progress: p, speed: max(0, loc.speed))
            if p.offRoute && Settings.shared.autoReroute && !isRerouting && Date().timeIntervalSince(lastReroute) > 6 {
                reroute()
            }
        }
        NotificationCenter.default.post(name: Navigator.didUpdate, object: self)
    }

    /// Recalculate from the current position, keeping the direction of travel.
    func reroute() {
        guard let s = session, let loc = lastLocation, !isRerouting else { return }
        isRerouting = true
        rerouteError = nil
        lastReroute = Date()
        NotificationCenter.default.post(name: Navigator.didUpdate, object: self)
        let heading: CLLocationDirection? = (loc.course >= 0 && loc.speed > 2) ? loc.course : nil
        let target = s.route.destination
        Router.shared.route(from: loc.coordinate, heading: heading, to: target, mode: s.route.mode) { [weak self] result in
            guard let self = self, let cur = self.session, cur === s else { return }
            self.isRerouting = false
            switch result {
            case .success(let r):
                cur.replace(r)
                VoiceGuide.shared.reset()
                VoiceGuide.shared.say("Route recalculated")
                if let l = self.lastLocation { self.handle(l) }
            case .failure(let e):
                self.rerouteError = e.description
            }
            NotificationCenter.default.post(name: Navigator.didUpdate, object: self)
        }
    }
}
