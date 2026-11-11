import CoreLocation
import MapLibre
import UIKit

/// Owns one MLNMapView and everything drawn on it. The rest of the app talks
/// to this class only, never to MapLibre, so swapping or upgrading the map
/// renderer touches one file (see docs/PORTING.md).
///
/// One instance drives the phone screen; CarPlay creates a second one for
/// the car display.
final class MapController: NSObject, MLNMapViewDelegate {
    let mapView: MLNMapView
    let locationAdapter = MapLocationAdapter()

    /// Called when the user moves the map while it was following them.
    var onTrackingStopped: (() -> Void)?
    /// Called after the camera stops moving (center, zoom).
    var onRegionChanged: ((CLLocationCoordinate2D, Double) -> Void)?
    /// Long press on the map.
    var onLongPress: ((CLLocationCoordinate2D) -> Void)?

    private var styleLoaded = false
    private var composedPacks: [String] = []
    private var composedMode: RenderMode?

    // Overlay state, re-applied whenever a style (re)loads.
    private var routeCoords: [CLLocationCoordinate2D] = []
    private var passedCoords: [CLLocationCoordinate2D] = []
    private var destination: CLLocationCoordinate2D?
    private var pin: CLLocationCoordinate2D?
    private var maneuver: CLLocationCoordinate2D?
    private(set) var navigating = false
    /// Top/bottom screen space covered by panels (points).
    private var overlayInsets = UIEdgeInsets.zero
    /// Scanline texture over the map in the SCREEN style (see applyScreenLook).
    private let scanlines = UIView()
    let isCarPlay: Bool

    init(frame: CGRect, carPlay: Bool = false) {
        isCarPlay = carPlay
        mapView = MLNMapView(frame: frame)
        super.init()
        mapView.autoresizingMask = [.flexibleWidth, .flexibleHeight]
        mapView.backgroundColor = .black
        mapView.tintColor = Theme.cyan
        mapView.delegate = self
        mapView.logoView.isHidden = true            // attribution is shown by our own label
        mapView.attributionButton.isHidden = true
        mapView.showsScale = false
        mapView.compassViewPosition = .topRight
        mapView.maximumZoomLevel = 19
        mapView.minimumZoomLevel = 2
        mapView.automaticallyAdjustsContentInset = false
        mapView.locationManager = locationAdapter
        mapView.prefetchesTiles = false             // nothing to prefetch from a network
        applyFrameRate()
        scanlines.isUserInteractionEnabled = false
        scanlines.autoresizingMask = [.flexibleWidth, .flexibleHeight]
        scanlines.backgroundColor = UIColor(patternImage: MapController.scanlineTile())
        scanlines.isAccessibilityElement = false
        applyScreenLook()
        if !carPlay {
            let lp = UILongPressGestureRecognizer(target: self, action: #selector(longPress(_:)))
            mapView.addGestureRecognizer(lp)
        }
        NotificationCenter.default.addObserver(self, selector: #selector(settingsChanged),
                                               name: Settings.didChange, object: nil)
        NotificationCenter.default.addObserver(self, selector: #selector(packsChanged),
                                               name: PackStore.didChange, object: nil)
    }

    deinit {
        NotificationCenter.default.removeObserver(self)
    }

    // MARK: style

    private func applyFrameRate() {
        mapView.preferredFramesPerSecond = Settings.shared.smoothRendering
            ? MLNMapViewPreferredFramesPerSecond.maximum : MLNMapViewPreferredFramesPerSecond.lowPower
    }

    @objc private func settingsChanged() {
        applyFrameRate()
        applyScreenLook()
        reloadStyleIfNeeded(force: composedMode != Settings.shared.renderMode)
        applyNavigationLook()
        applyTilt()
    }

    /// The SCREEN style imitates a monitor: a light map under fine horizontal
    /// scanlines. The texture is a plain view right above MapLibre's drawing
    /// surface (below the compass), so it costs one blended layer and nothing
    /// per frame. Other styles remove it.
    private func applyScreenLook() {
        let screen = Settings.shared.renderMode == .screen
        mapView.backgroundColor = screen ? Theme.screenBackground : .black
        mapView.tintColor = screen ? Theme.screenInk : Theme.cyan
        if !screen {
            scanlines.removeFromSuperview()
            return
        }
        guard scanlines.superview == nil else { return }
        // MapLibre's drawing surface (an MTKView or GLKView) is its first subview.
        let views = mapView.subviews
        guard let surface = views.first(where: {
            let name = NSStringFromClass(type(of: $0))
            return name.contains("MTKView") || name.contains("GLKView")
        }) ?? views.first else { return }
        scanlines.frame = mapView.bounds
        mapView.insertSubview(scanlines, aboveSubview: surface)
    }

    /// One tile of the scanline pattern: three device pixels tall, the top
    /// pixel a faint navy line.
    private static func scanlineTile() -> UIImage {
        let scale = UIScreen.main.scale
        let size = CGSize(width: 1, height: 3 / scale)
        UIGraphicsBeginImageContextWithOptions(size, false, scale)
        defer { UIGraphicsEndImageContext() }
        Theme.screenInk.withAlphaComponent(0.13).setFill()
        UIRectFill(CGRect(x: 0, y: 0, width: 1, height: 1 / scale))
        return UIGraphicsGetImageFromCurrentImageContext() ?? UIImage()
    }

    private var appliedTilt: Bool?

    /// Tilts (3D) or flattens the camera to match Settings.tilt, keeping the
    /// current follow mode so navigation keeps following the user.
    func applyTilt(animated: Bool = true) {
        let want = Settings.shared.tilt
        guard appliedTilt != want else { return }
        appliedTilt = want
        let tracking = mapView.userTrackingMode
        let cam = mapView.camera
        cam.pitch = want ? 45 : 0
        mapView.setCamera(cam, withDuration: animated ? 0.35 : 0,
                          animationTimingFunction: CAMediaTimingFunction(name: .easeInEaseOut)) { [weak self] in
            guard let self = self else { return }
            if tracking != .none && self.mapView.userTrackingMode == .none {
                self.mapView.setUserTrackingMode(tracking, animated: false, completionHandler: nil)
            }
        }
    }

    @objc private func packsChanged() {
        // Reloads only if the set of packs (or a pack's build date) changed.
        reloadStyleIfNeeded()
    }

    /// Packs to draw: those overlapping the view (at most three), else the
    /// one closest to the view so the screen is never empty after launch.
    private func packsForView() -> [MapPack] {
        let all = PackStore.shared.packs.filter { $0.tilesURL != nil }
        guard !all.isEmpty else { return [] }
        let b = mapView.visibleCoordinateBounds
        var hits = all.filter {
            $0.intersects(minLat: b.sw.latitude, minLon: b.sw.longitude, maxLat: b.ne.latitude, maxLon: b.ne.longitude)
        }
        if hits.isEmpty {
            let c = mapView.centerCoordinate
            if let near = all.min(by: { distance($0.center, c) < distance($1.center, c) }) { hits = [near] }
        }
        return Array(hits.sorted { $0.id < $1.id }.prefix(3))
    }

    private func distance(_ a: CLLocationCoordinate2D, _ b: CLLocationCoordinate2D) -> Double {
        return tz_distance_m(a.latitude, a.longitude, b.latitude, b.longitude)
    }

    func reloadStyleIfNeeded(force: Bool = false) {
        let packs = packsForView()
        let ids = packs.map { "\($0.id)|\($0.manifest.created ?? "")" }
        let mode = Settings.shared.renderMode
        guard force || ids != composedPacks || mode != composedMode || !styleLoaded && composedMode == nil else { return }
        do {
            let url = try StyleComposer.compose(mode: mode, packs: packs)
            composedPacks = ids
            composedMode = mode
            styleLoaded = false
            mapView.styleURL = url
        } catch {
            NSLog("TRACE//ZERO style error: \(error)")
        }
    }

    func mapView(_ mapView: MLNMapView, didFinishLoading style: MLNStyle) {
        styleLoaded = true
        applyScreenLook()
        for name in MapImages.names {
            if let img = MapImages.image(named: name) { style.setImage(img, forName: name) }
        }
        applyOverlays()
        applyNavigationLook()
    }

    func mapView(_ mapView: MLNMapView, didFailToLoadImage imageName: String) -> UIImage? {
        return MapImages.image(named: imageName)
    }

    func mapView(styleForDefaultUserLocationAnnotationView mapView: MLNMapView) -> MLNUserLocationAnnotationViewStyle {
        let s = MLNUserLocationAnnotationViewStyle()
        s.puckFillColor = Theme.black
        s.puckArrowFillColor = Theme.cyan
        s.puckShadowColor = Theme.cyan
        s.puckShadowOpacity = 0.6
        s.haloFillColor = Theme.cyan
        return s
    }

    // MARK: overlays

    private func setShape(_ source: String, _ shape: MLNShape?) {
        guard styleLoaded, let src = mapView.style?.source(withIdentifier: source) as? MLNShapeSource else { return }
        src.shape = shape ?? MLNShapeCollectionFeature(shapes: [])
    }

    private func line(_ c: [CLLocationCoordinate2D]) -> MLNShape? {
        guard c.count >= 2 else { return nil }
        return MLNPolylineFeature(coordinates: c, count: UInt(c.count))
    }

    private func point(_ c: CLLocationCoordinate2D?) -> MLNShape? {
        guard let c = c else { return nil }
        let p = MLNPointFeature()
        p.coordinate = c
        return p
    }

    private func applyOverlays() {
        setShape("route", line(routeCoords))
        setShape("route-passed", line(passedCoords))
        setShape("dest", point(destination))
        setShape("pin", point(pin))
        setShape("maneuver", point(maneuver))
    }

    func showRoute(_ route: Route?) {
        routeCoords = route?.coordinates ?? []
        passedCoords = []
        destination = route?.destination ?? destination
        if route == nil { maneuver = nil }
        applyOverlays()
    }

    /// Splits the route at the current position: the part ahead is drawn
    /// bright, the part behind faint.
    func showProgress(route: Route, progress: NavProgress) {
        let seg = min(progress.segment, route.coordinates.count - 2)
        var ahead: [CLLocationCoordinate2D] = [progress.snapped]
        ahead.append(contentsOf: route.coordinates[(seg + 1)...])
        var behind = Array(route.coordinates[...seg])
        behind.append(progress.snapped)
        routeCoords = ahead
        passedCoords = behind
        if progress.nextManeuver < route.maneuvers.count {
            let m = route.maneuvers[progress.nextManeuver]
            maneuver = m.kind == .arrive ? nil : m.coordinate
        }
        applyOverlays()
    }

    func setDestination(_ c: CLLocationCoordinate2D?) {
        destination = c
        applyOverlays()
    }

    func setPin(_ c: CLLocationCoordinate2D?) {
        pin = c
        applyOverlays()
    }

    // MARK: navigation look and camera

    /// Dims the base map and hides minor labels while navigating so the
    /// route is the brightest thing on screen.
    private func applyNavigationLook() {
        guard styleLoaded, let style = mapView.style else { return }
        if let dim = style.layer(withIdentifier: "nav-dim") as? MLNFillStyleLayer {
            dim.fillOpacity = NSExpression(forConstantValue: navigating ? 0.55 : 0.0)
        }
        let hide = navigating && Settings.shared.hideLabelsWhileNavigating
        for layer in style.layers {
            let id = layer.identifier
            let base = id.components(separatedBy: "@").first ?? id
            if base == "poi-dot" || base == "poi-name" || base == "housenumber" {
                layer.isVisible = !hide
            }
        }
    }

    func setNavigating(_ on: Bool, mode: TravelMode) {
        navigating = on
        applyNavigationLook()
        if on {
            mapView.showsUserLocation = true
            followCourse(mode: mode)
        } else {
            maneuver = nil
            passedCoords = []
            applyOverlays()
            mapView.setUserTrackingMode(.none, animated: false, completionHandler: nil)
            let cam = mapView.camera
            cam.pitch = Settings.shared.tilt ? 45 : 0   // the 3D toggle applies when browsing too
            appliedTilt = Settings.shared.tilt
            mapView.setCamera(cam, animated: true)
        }
    }

    /// Navigation camera: follow with course, user arrow low on screen,
    /// zoom set by speed class of the travel mode.
    func followCourse(mode: TravelMode) {
        let cam = mapView.camera
        cam.pitch = Settings.shared.tilt ? 45 : 0
        appliedTilt = Settings.shared.tilt
        mapView.setCamera(cam, animated: false)
        let zoom: Double = mode == .car ? 16.2 : (mode == .bike ? 16.8 : 17.3)
        mapView.setZoomLevel(zoom, animated: true)
        mapView.setUserTrackingMode(.followWithCourse, animated: true, completionHandler: nil)
    }

    func followUser() {
        mapView.showsUserLocation = true
        mapView.setUserTrackingMode(.follow, animated: true, completionHandler: nil)
    }

    var isFollowing: Bool { return mapView.userTrackingMode != .none }

    /// Space covered by on-screen panels; the camera centres in the rest.
    /// While navigating the arrow is placed in the lower third of that area.
    func setPanels(top: CGFloat, bottom: CGFloat, left: CGFloat = 0) {
        var inset = UIEdgeInsets(top: top, left: left, bottom: bottom, right: 0)
        if navigating {
            let free = mapView.bounds.height - top - bottom
            inset.top += max(0, free * 0.35)
        }
        guard inset != overlayInsets else { return }
        overlayInsets = inset
        mapView.setContentInset(inset, animated: true, completionHandler: nil)
    }

    func fit(_ coords: [CLLocationCoordinate2D], padding: UIEdgeInsets) {
        guard let first = coords.first else { return }
        var sw = first, ne = first
        for c in coords {
            sw.latitude = min(sw.latitude, c.latitude)
            sw.longitude = min(sw.longitude, c.longitude)
            ne.latitude = max(ne.latitude, c.latitude)
            ne.longitude = max(ne.longitude, c.longitude)
        }
        mapView.setUserTrackingMode(.none, animated: false, completionHandler: nil)
        if coords.count == 1 || (ne.latitude - sw.latitude < 0.0005 && ne.longitude - sw.longitude < 0.0005) {
            mapView.setCenter(first, zoomLevel: 16, animated: true)
            return
        }
        mapView.setVisibleCoordinateBounds(MLNCoordinateBounds(sw: sw, ne: ne), edgePadding: padding,
                                           animated: true, completionHandler: nil)
    }

    func center(on c: CLLocationCoordinate2D, zoom: Double) {
        mapView.setUserTrackingMode(.none, animated: false, completionHandler: nil)
        mapView.setCenter(c, zoomLevel: zoom, animated: true)
    }

    /// Feed a location to the user arrow (raw, or snapped to the route).
    func updateUserLocation(_ loc: CLLocation) {
        locationAdapter.push(loc)
    }

    // MARK: delegate

    func mapView(_ mapView: MLNMapView, didChange mode: MLNUserTrackingMode, animated: Bool) {
        if mode == .none { onTrackingStopped?() }
    }

    func mapView(_ mapView: MLNMapView, regionDidChangeAnimated animated: Bool) {
        onRegionChanged?(mapView.centerCoordinate, mapView.zoomLevel)
        reloadStyleIfNeeded()
    }

    @objc private func longPress(_ g: UILongPressGestureRecognizer) {
        guard g.state == .began else { return }
        let c = mapView.convert(g.location(in: mapView), toCoordinateFrom: mapView)
        onLongPress?(c)
    }
}
