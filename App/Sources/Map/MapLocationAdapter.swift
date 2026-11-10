import CoreLocation
import MapLibre

/// Lets MLNMapView draw the user arrow from *our* location stream instead of
/// running its own CLLocationManager (one GPS consumer, less battery).
/// While navigating we feed it the position snapped to the route with the
/// route's direction as course, so the arrow sits on the line and turns
/// with it instead of wobbling with GPS noise.
final class MapLocationAdapter: NSObject, MLNLocationManager {
    weak var delegate: MLNLocationManagerDelegate?
    var headingOrientation: CLDeviceOrientation = .portrait
    private var running = false

    var authorizationStatus: CLAuthorizationStatus {
        return CLLocationManager.authorizationStatus()
    }

    // Authorization and the radio are handled by LocationService.
    func requestAlwaysAuthorization() {}
    func requestWhenInUseAuthorization() {}
    func startUpdatingLocation() { running = true }
    func stopUpdatingLocation() { running = false }
    func startUpdatingHeading() {}
    func stopUpdatingHeading() {}
    func dismissHeadingCalibrationDisplay() {}

    func push(_ location: CLLocation) {
        guard running, let d = delegate as? NSObject else { return }
        // Called through the Objective-C selector so this compiles the same
        // whatever Swift name the SDK version gives the delegate method.
        let sel = NSSelectorFromString("locationManager:didUpdateLocations:")
        if d.responds(to: sel) { d.perform(sel, with: self, with: [location]) }
    }
}
