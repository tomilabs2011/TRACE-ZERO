import CarPlay
import MapKit
import UIKit

/// CarPlay support (iOS 12 API: CPApplicationDelegate on the app delegate).
///
/// CarPlay only starts for apps signed with the
/// `com.apple.developer.carplay-maps` entitlement. Apple grants it to
/// navigation apps on request; on a jailbroken phone the entitlement can be
/// added when fake-signing (see docs/CARPLAY.md and Tools/build-ipa.sh).
/// Without it this code is simply never called.
///
/// The car screen shows its own map, search, route preview and turn-by-turn
/// guidance. Navigation itself lives in `Navigator`, so a route started on
/// the phone appears in the car and vice versa.
extension AppDelegate: CPApplicationDelegate {
    func application(_ application: UIApplication, didConnectCarInterfaceController interfaceController: CPInterfaceController,
                     to window: CPWindow) {
        CarPlayController.shared.connect(interfaceController, window: window)
    }

    func application(_ application: UIApplication, didDisconnectCarInterfaceController interfaceController: CPInterfaceController,
                     from window: CPWindow) {
        CarPlayController.shared.disconnect()
    }
}

final class CarPlayController: NSObject, CPMapTemplateDelegate, CPSearchTemplateDelegate {
    static let shared = CarPlayController()

    private var interface: CPInterfaceController?
    private var window: CPWindow?
    private var map: MapController?
    private var mapTemplate: CPMapTemplate?
    private var session: CPNavigationSession?
    private var trip: CPTrip?
    private var shownManeuver = -1
    private var cpManeuvers: [CPManeuver] = []
    private var lastResults: [SearchResult] = []
    private var locationObserver: UUID?

    func connect(_ ic: CPInterfaceController, window w: CPWindow) {
        interface = ic
        window = w
        LocationService.shared.carPlayConnected = true
        if LocationService.shared.mode == .off { LocationService.shared.set(.browse) }

        let vc = UIViewController()
        let m = MapController(frame: w.bounds, carPlay: true)
        vc.view = m.mapView
        w.rootViewController = vc
        map = m
        m.reloadStyleIfNeeded(force: true)
        m.mapView.showsUserLocation = true
        m.followUser()

        let t = CPMapTemplate()
        t.mapDelegate = self
        t.guidanceBackgroundColor = .black
        if #available(iOS 13.0, *) { t.tripEstimateStyle = .dark }
        t.automaticallyHidesNavigationBar = false
        let search = CPBarButton(type: .text) { [weak self] _ in self?.openSearch() }
        search.title = "Search"
        t.leadingNavigationBarButtons = [search]
        let recenter = CPMapButton { [weak self] _ in self?.recenter() }
        recenter.image = Self.icon(.locate)
        let mode = CPMapButton { _ in
            let all = RenderMode.allCases
            let i = all.firstIndex(of: Settings.shared.renderMode) ?? 0
            Settings.shared.renderMode = all[(i + 1) % all.count]
        }
        mode.image = Self.icon(.layers)
        t.mapButtons = [recenter, mode]
        ic.setRootTemplate(t, animated: false)
        mapTemplate = t

        let nc = NotificationCenter.default
        nc.addObserver(self, selector: #selector(navStarted), name: Navigator.didStart, object: nil)
        nc.addObserver(self, selector: #selector(navUpdated), name: Navigator.didUpdate, object: nil)
        nc.addObserver(self, selector: #selector(navEnded), name: Navigator.didEnd, object: nil)
        locationObserver = LocationService.shared.observe { [weak self] loc in
            guard let self = self, !Navigator.shared.isActive else { return }
            self.map?.updateUserLocation(loc)
        }
        if Navigator.shared.isActive { navStarted() }
    }

    func disconnect() {
        NotificationCenter.default.removeObserver(self)
        if let o = locationObserver { LocationService.shared.remove(o) }
        locationObserver = nil
        LocationService.shared.carPlayConnected = false
        session = nil
        trip = nil
        mapTemplate = nil
        map = nil
        window?.rootViewController = nil
        window = nil
        interface = nil
    }

    private static func icon(_ i: Icon) -> UIImage {
        let size = CGSize(width: 32, height: 32)
        UIGraphicsBeginImageContextWithOptions(size, false, 0)
        defer { UIGraphicsEndImageContext() }
        Icons.draw(i, in: CGRect(origin: .zero, size: size).insetBy(dx: 5, dy: 5), color: Theme.standardCyan)
        return UIGraphicsGetImageFromCurrentImageContext() ?? UIImage()
    }

    private func recenter() {
        guard let m = map else { return }
        if let r = Navigator.shared.route { m.followCourse(mode: r.mode) } else { m.followUser() }
    }

    // MARK: search

    private func openSearch() {
        let s = CPSearchTemplate()
        s.delegate = self
        interface?.pushTemplate(s, animated: true)
    }

    func searchTemplate(_ searchTemplate: CPSearchTemplate, updatedSearchText searchText: String,
                        completionHandler: @escaping ([CPListItem]) -> Void) {
        let near = LocationService.shared.last?.coordinate
        SearchEngine.shared.search(searchText, near: near, limit: 12) { [weak self] results in
            self?.lastResults = results
            completionHandler(results.enumerated().map { i, r in
                let item = CPListItem(text: r.title,
                                      detailText: [r.subtitle, r.distance.map { Format.distance($0) } ?? ""]
                                        .filter { !$0.isEmpty }.joined(separator: " · "))
                item.userInfo = i
                return item
            })
        }
    }

    func searchTemplate(_ searchTemplate: CPSearchTemplate, selectedResult item: CPListItem,
                        completionHandler: @escaping () -> Void) {
        guard let i = item.userInfo as? Int, i < lastResults.count else { completionHandler(); return }
        let target = lastResults[i]
        interface?.popToRootTemplate(animated: true)
        completionHandler()
        preview(target)
    }

    // MARK: route preview

    private func mapItem(_ c: CLLocationCoordinate2D, name: String) -> MKMapItem {
        let item = MKMapItem(placemark: MKPlacemark(coordinate: c))
        item.name = name
        return item
    }

    private func preview(_ target: SearchResult) {
        guard let here = LocationService.shared.last else { return }
        let mode = Settings.shared.travelMode
        Router.shared.route(from: here.coordinate, heading: here.course >= 0 && here.speed > 2 ? here.course : nil,
                            to: target.coordinate, mode: mode) { [weak self] result in
            guard let self = self, let t = self.mapTemplate else { return }
            guard case .success(let route) = result else { return }
            let choice = CPRouteChoice(summaryVariants: ["\(Format.duration(route.duration)) · \(Format.distance(route.distance))"],
                                       additionalInformationVariants: [mode.title.capitalized],
                                       selectionSummaryVariants: [Format.duration(route.duration)])
            choice.userInfo = route
            let trip = CPTrip(origin: self.mapItem(here.coordinate, name: "Current location"),
                              destination: self.mapItem(target.coordinate, name: target.title),
                              routeChoices: [choice])
            trip.userInfo = target.title
            self.map?.showRoute(route)
            self.map?.setDestination(route.destination)
            self.map?.fit(route.coordinates, padding: UIEdgeInsets(top: 40, left: 40, bottom: 40, right: 40))
            t.showTripPreviews([trip], textConfiguration: nil)
        }
    }

    func mapTemplate(_ mapTemplate: CPMapTemplate, startedTrip trip: CPTrip, using routeChoice: CPRouteChoice) {
        mapTemplate.hideTripPreviews()
        guard let route = routeChoice.userInfo as? Route else { return }
        self.trip = trip
        Navigator.shared.start(route: route, destinationName: trip.userInfo as? String ?? "Destination")
    }

    func mapTemplateDidCancelNavigation(_ mapTemplate: CPMapTemplate) {
        Navigator.shared.stop()
    }

    // MARK: guidance

    @objc private func navStarted() {
        guard let t = mapTemplate, let route = Navigator.shared.route else { return }
        if trip == nil {
            // Started on the phone: show it in the car too.
            let origin = route.coordinates.first ?? route.destination
            let choice = CPRouteChoice(summaryVariants: [Format.duration(route.duration)],
                                       additionalInformationVariants: [], selectionSummaryVariants: [])
            trip = CPTrip(origin: mapItem(origin, name: "Start"),
                          destination: mapItem(route.destination, name: Navigator.shared.destinationName),
                          routeChoices: [choice])
        }
        if let trip = trip { session = t.startNavigationSession(for: trip) }
        map?.setDestination(route.destination)
        map?.showRoute(route)
        map?.setNavigating(true, mode: route.mode)
        shownManeuver = -1
        cpManeuvers = []
        navUpdated()
    }

    @objc private func navUpdated() {
        let nav = Navigator.shared
        guard let s = session, let route = nav.route else { return }
        if let p = nav.progress {
            map?.showProgress(route: route, progress: p)
            if let d = nav.displayLocation { map?.updateUserLocation(d) }
            if p.nextManeuver != shownManeuver || cpManeuvers.isEmpty {
                shownManeuver = p.nextManeuver
                let upcoming = route.maneuvers[p.nextManeuver...].prefix(2)
                cpManeuvers = upcoming.map { makeManeuver($0) }
                s.upcomingManeuvers = cpManeuvers
            }
            if let first = cpManeuvers.first {
                s.updateEstimates(CPTravelEstimates(distanceRemaining: Measurement(value: p.distanceToNextManeuver, unit: UnitLength.meters),
                                                    timeRemaining: max(0, route.maneuvers[p.nextManeuver].timeFromStart - (route.duration - p.remainingTime))),
                                  for: first)
            }
            if let trip = trip {
                mapTemplate?.updateEstimates(CPTravelEstimates(distanceRemaining: Measurement(value: p.remainingDistance, unit: UnitLength.meters),
                                                               timeRemaining: p.remainingTime), for: trip)
            }
            if nav.arrived {
                s.finishTrip()
                session = nil
            }
        }
    }

    @objc private func navEnded() {
        session?.cancelTrip()
        session = nil
        trip = nil
        map?.setNavigating(false, mode: Settings.shared.travelMode)
        map?.showRoute(nil)
        map?.setDestination(nil)
        map?.followUser()
    }

    private func makeManeuver(_ m: Maneuver) -> CPManeuver {
        let cp = CPManeuver()
        let full = Instructions.sentence(m)
        cp.instructionVariants = [full, Instructions.primary(m)]
        let img = ManeuverGlyph.image(size: CGSize(width: 48, height: 48), angle: m.glyphAngle,
                                      uturn: m.modifier == 4 && m.kind == .turn,
                                      roundaboutExit: m.kind == .roundabout ? m.exit : nil,
                                      arrive: m.kind == .arrive, color: .white)
        cp.symbolSet = CPImageSet(lightContentImage: img, darkContentImage: img)
        return cp
    }
}
