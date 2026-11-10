import CoreLocation
import UIKit

/// The only owner of GPS in the app. The map's user dot, navigation and
/// CarPlay all listen to this one CLLocationManager, so the radio is never
/// used twice and power use stays predictable.
final class LocationService: NSObject, CLLocationManagerDelegate {
    static let shared = LocationService()

    enum Mode { case off, browse, navigate }

    private let manager = CLLocationManager()
    private var observers: [UUID: (CLLocation) -> Void] = [:]
    private var authObservers: [UUID: () -> Void] = [:]
    private(set) var mode: Mode = .off
    private(set) var last: CLLocation?
    var travelMode: TravelMode = .foot { didSet { if mode == .navigate { configure() } } }
    /// Set while CarPlay shows our map: navigation must keep running then.
    var carPlayConnected = false
    private var inBackground = false

    override init() {
        super.init()
        manager.delegate = self
        NotificationCenter.default.addObserver(self, selector: #selector(didEnterBackground),
                                               name: UIApplication.didEnterBackgroundNotification, object: nil)
        NotificationCenter.default.addObserver(self, selector: #selector(willEnterForeground),
                                               name: UIApplication.willEnterForegroundNotification, object: nil)
    }

    var authorized: Bool {
        let s = CLLocationManager.authorizationStatus()
        return s == .authorizedWhenInUse || s == .authorizedAlways
    }

    var denied: Bool {
        let s = CLLocationManager.authorizationStatus()
        return s == .denied || s == .restricted
    }

    @discardableResult
    func observe(_ block: @escaping (CLLocation) -> Void) -> UUID {
        let id = UUID()
        observers[id] = block
        return id
    }

    @discardableResult
    func observeAuthorization(_ block: @escaping () -> Void) -> UUID {
        let id = UUID()
        authObservers[id] = block
        return id
    }

    func remove(_ id: UUID) {
        observers[id] = nil
        authObservers[id] = nil
    }

    func set(_ m: Mode) {
        mode = m
        configure()
    }

    private func configure() {
        switch mode {
        case .off:
            manager.stopUpdatingLocation()
            return
        case .browse:
            manager.desiredAccuracy = kCLLocationAccuracyNearestTenMeters
            manager.distanceFilter = 5
            manager.activityType = .other
            manager.pausesLocationUpdatesAutomatically = true
            setBackground(false)
        case .navigate:
            let charging = UIDevice.current.batteryState == .charging || UIDevice.current.batteryState == .full
            manager.desiredAccuracy = (travelMode == .car && charging) ? kCLLocationAccuracyBestForNavigation
                                                                      : kCLLocationAccuracyBest
            manager.distanceFilter = kCLDistanceFilterNone
            switch travelMode {
            case .car: manager.activityType = .automotiveNavigation
            case .bike: manager.activityType = .fitness
            case .foot: manager.activityType = .otherNavigation
            }
            manager.pausesLocationUpdatesAutomatically = false
            setBackground(Settings.shared.backgroundNavigation || carPlayConnected)
        }
        if CLLocationManager.authorizationStatus() == .notDetermined {
            manager.requestWhenInUseAuthorization()
            return
        }
        guard authorized else { return }
        if inBackground && !keepsRunningInBackground {
            manager.stopUpdatingLocation()
        } else {
            manager.startUpdatingLocation()
        }
    }

    /// Background updates need UIBackgroundModes=location in Info.plist (present).
    private func setBackground(_ on: Bool) {
        manager.allowsBackgroundLocationUpdates = on
        manager.showsBackgroundLocationIndicator = on
    }

    private var keepsRunningInBackground: Bool {
        return mode == .navigate && (Settings.shared.backgroundNavigation || carPlayConnected)
    }

    // Navigation pauses when the app or the screen goes to sleep, unless the
    // user enabled background navigation or CarPlay is in use.
    @objc private func didEnterBackground() {
        inBackground = true
        if !keepsRunningInBackground { manager.stopUpdatingLocation() }
    }

    @objc private func willEnterForeground() {
        inBackground = false
        configure()
    }

    // MARK: CLLocationManagerDelegate

    func locationManager(_ manager: CLLocationManager, didUpdateLocations locations: [CLLocation]) {
        for loc in locations {
            // Ignore stale cached fixes and invalid ones.
            guard loc.horizontalAccuracy >= 0, loc.timestamp.timeIntervalSinceNow > -30 else { continue }
            last = loc
            for (_, o) in observers { o(loc) }
        }
    }

    func locationManager(_ manager: CLLocationManager, didChangeAuthorization status: CLAuthorizationStatus) {
        configure()
        for (_, o) in authObservers { o() }
    }

    func locationManager(_ manager: CLLocationManager, didFailWithError error: Error) {
        // kCLErrorLocationUnknown is temporary; anything else is reported by
        // the authorization state. Nothing to do here.
    }
}
