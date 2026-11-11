import CoreLocation
import UIKit

/// The single main screen. States:
///   browse     map, search, locate
///   place      a selected point with a ROUTE button
///   preview    a computed route with START
///   navigating turn-by-turn
/// Layout is done by hand in viewDidLayoutSubviews: one code path for the
/// 4-inch portrait screen and one for landscape (panels move to a left
/// column), which keeps the 320 pt layout exact.
final class MainViewController: UIViewController {
    private enum Mode {
        case browse
        case place(SearchResult)
        case preview(target: SearchResult, route: Route?)
        case navigating
    }

    private var mode: Mode = .browse { didSet { modeChanged() } }

    private let map = MapController(frame: UIScreen.main.bounds)
    private let searchBar = UIButton(type: .custom)
    private let menuButton = IconButton(icon: .menu)
    private let locateButton = IconButton(icon: .locate)
    private let layersButton = IconButton(icon: .layers)
    private let tiltButton = TextButton(title: "3D")
    private let recenterButton = TextButton(title: "RECENTER")
    private let attribution = UILabel()
    private let placeCard = PlaceCardView()
    private let previewCard = RoutePreviewView()
    private let banner = NavBannerView()
    private let status = NavStatusView()
    private let emptyNotice = UILabel()

    private var locationObserver: UUID?
    private var authObserver: UUID?
    private var routeRequest = 0

    override var preferredStatusBarStyle: UIStatusBarStyle { return Theme.mapStatusBar }

    // MARK: setup

    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = Theme.background
        map.mapView.frame = view.bounds
        view.addSubview(map.mapView)

        searchBar.addTarget(self, action: #selector(openSearch), for: .touchUpInside)
        Theme.stylePanel(searchBar)
        searchBar.contentHorizontalAlignment = .left
        searchBar.titleEdgeInsets = UIEdgeInsets(top: 0, left: 40, bottom: 0, right: 8)
        searchBar.setAttributedTitle(Theme.tracked("Search offline", size: 15, weight: .demi, color: Theme.fgDim),
                                     for: .normal)
        let glass = SearchGlass(frame: CGRect(x: 12, y: 12, width: 20, height: 20))
        glass.isUserInteractionEnabled = false
        searchBar.addSubview(glass)

        menuButton.addTarget(self, action: #selector(openMenu), for: .touchUpInside)
        locateButton.addTarget(self, action: #selector(locateTapped), for: .touchUpInside)
        layersButton.addTarget(self, action: #selector(cycleRenderMode), for: .touchUpInside)
        tiltButton.addTarget(self, action: #selector(tiltTapped), for: .touchUpInside)
        tiltButton.accessibilityLabel = "3D view"
        recenterButton.addTarget(self, action: #selector(recenterTapped), for: .touchUpInside)

        attribution.text = "© OpenStreetMap contributors"
        attribution.font = Theme.font(10, weight: .regular)
        attribution.textColor = Theme.fgDim
        attribution.shadowColor = Theme.background
        attribution.shadowOffset = CGSize(width: 0, height: 1)

        emptyNotice.numberOfLines = 0
        emptyNotice.textAlignment = .center
        emptyNotice.backgroundColor = Theme.panel
        emptyNotice.layer.borderColor = Theme.fg.cgColor
        emptyNotice.layer.borderWidth = 1
        emptyNotice.layer.cornerRadius = 4
        emptyNotice.clipsToBounds = true

        placeCard.closeButton.addTarget(self, action: #selector(closeCard), for: .touchUpInside)
        placeCard.routeButton.addTarget(self, action: #selector(routeTapped), for: .touchUpInside)
        placeCard.modes.addTarget(self, action: #selector(modeSegmentChanged(_:)), for: .valueChanged)
        previewCard.closeButton.addTarget(self, action: #selector(closeCard), for: .touchUpInside)
        previewCard.startButton.addTarget(self, action: #selector(startTapped), for: .touchUpInside)
        previewCard.stepsButton.addTarget(self, action: #selector(showSteps), for: .touchUpInside)
        previewCard.modes.addTarget(self, action: #selector(modeSegmentChanged(_:)), for: .valueChanged)
        status.endButton.addTarget(self, action: #selector(endTapped), for: .touchUpInside)
        status.muteButton.addTarget(self, action: #selector(muteTapped), for: .touchUpInside)
        status.rerouteButton.addTarget(self, action: #selector(rerouteTapped), for: .touchUpInside)
        status.stepsButton.addTarget(self, action: #selector(showSteps), for: .touchUpInside)

        for v in [attribution, searchBar, menuButton, locateButton, layersButton, tiltButton, recenterButton,
                  emptyNotice, placeCard, previewCard, banner, status] as [UIView] {
            view.addSubview(v)
        }

        map.onLongPress = { [weak self] c in self?.select(coordinate: c) }
        map.onTrackingStopped = { [weak self] in self?.refreshButtons() }
        map.onRegionChanged = { c, z in Settings.shared.lastCamera = (c.latitude, c.longitude, z) }

        let nc = NotificationCenter.default
        nc.addObserver(self, selector: #selector(navigatorUpdated), name: Navigator.didUpdate, object: nil)
        nc.addObserver(self, selector: #selector(navigatorStarted), name: Navigator.didStart, object: nil)
        nc.addObserver(self, selector: #selector(navigatorEnded), name: Navigator.didEnd, object: nil)
        nc.addObserver(self, selector: #selector(packsChanged), name: PackStore.didChange, object: nil)

        locationObserver = LocationService.shared.observe { [weak self] loc in
            guard let self = self, !Navigator.shared.isActive else { return }
            self.map.updateUserLocation(loc)
        }
        authObserver = LocationService.shared.observeAuthorization { [weak self] in
            guard let self = self else { return }
            if LocationService.shared.authorized { self.map.mapView.showsUserLocation = true }
        }

        restoreCamera()
        map.reloadStyleIfNeeded(force: true)
        if LocationService.shared.authorized { map.mapView.showsUserLocation = true }
        modeChanged()
        packsChanged()
        // The screen is rebuilt when the look changes (AppDelegate.applyLook);
        // pick up a navigation that is already running.
        if Navigator.shared.isActive {
            navigatorStarted()
        } else {
            LocationService.shared.set(.browse)
        }
    }

    deinit {
        if let id = locationObserver { LocationService.shared.remove(id) }
        if let id = authObserver { LocationService.shared.remove(id) }
    }

    private func restoreCamera() {
        if let c = Settings.shared.lastCamera {
            map.mapView.setCenter(CLLocationCoordinate2D(latitude: c.lat, longitude: c.lon), zoomLevel: c.zoom, animated: false)
        } else if let p = PackStore.shared.packs.first {
            map.mapView.setCenter(p.center, zoomLevel: 7, animated: false)
        }
    }

    @objc private func packsChanged() {
        let store = PackStore.shared
        if store.packs.isEmpty {
            var text = "NO MAP PACKS\n\nConnect the iPhone to your Mac, open Finder → iPhone → Files, " +
                "and drag a .vmap folder onto TRACE ZERO."
            if !store.problems.isEmpty {
                text += "\n\n" + store.problems.map { "\($0.folder): \($0.message)" }.joined(separator: "\n")
            } else if !store.documentNames.isEmpty {
                text += "\n\nIn the app's folder: " + store.documentNames.prefix(6).joined(separator: ", ")
            }
            emptyNotice.attributedText = NSAttributedString(string: text, attributes: [
                .font: Theme.font(14, weight: .demi), .foregroundColor: Theme.fg])
            emptyNotice.isHidden = false
        } else {
            emptyNotice.isHidden = true
            if Settings.shared.lastCamera == nil, let p = store.packs.first {
                map.mapView.setCenter(p.center, zoomLevel: 7, animated: false)
            }
        }
        view.setNeedsLayout()
    }

    // MARK: layout

    override func viewDidLayoutSubviews() {
        super.viewDidLayoutSubviews()
        map.mapView.frame = view.bounds
        let safe = view.safeAreaInsets
        let landscape = view.bounds.width > view.bounds.height
        let m: CGFloat = 8
        let top = max(safe.top, landscape ? 0 : 20) + m
        let bottom = view.bounds.height - max(safe.bottom, 0) - m
        let left = safe.left + m
        let right = view.bounds.width - safe.right - m
        let column: CGFloat = landscape ? min(320, (right - left) * 0.5) : right - left

        // top bar
        let b: CGFloat = 44
        menuButton.frame = CGRect(x: right - b, y: top, width: b, height: b)
        searchBar.frame = CGRect(x: left, y: top, width: (landscape ? column : menuButton.frame.minX - 8 - left), height: b)

        var panelBottom = bottom
        var panelLeft: CGFloat = 0
        let navigating: Bool
        if case .navigating = mode { navigating = true } else { navigating = false }

        searchBar.isHidden = navigating
        menuButton.isHidden = navigating

        if navigating {
            let bh = banner.preferredHeight(width: column)
            banner.frame = CGRect(x: left, y: top, width: column, height: bh)
            status.frame = CGRect(x: left, y: bottom - 64, width: column, height: 64)
            panelBottom = landscape ? bottom : status.frame.minY
            panelLeft = landscape ? column + left : 0
        }

        // cards
        let cardH = PlaceCardView.height
        placeCard.frame = CGRect(x: left, y: bottom - cardH, width: column, height: cardH)
        previewCard.frame = CGRect(x: left, y: bottom - RoutePreviewView.height, width: column, height: RoutePreviewView.height)
        switch mode {
        case .place: if !landscape { panelBottom = placeCard.frame.minY } else { panelLeft = column + left }
        case .preview: if !landscape { panelBottom = previewCard.frame.minY } else { panelLeft = column + left }
        default: break
        }

        // side buttons above the bottom panel (right edge)
        let bx = right - b
        layersButton.frame = CGRect(x: bx, y: panelBottom - 8 - b, width: b, height: b)
        locateButton.frame = CGRect(x: bx, y: layersButton.frame.minY - 8 - b, width: b, height: b)
        if landscape && !navigating {
            layersButton.frame.origin.y = bottom - b
            locateButton.frame.origin.y = layersButton.frame.minY - 8 - b
        }
        // 3D toggle: above the other two while browsing; alone on the right
        // edge (where they were) while navigating.
        tiltButton.frame = navigating
            ? CGRect(x: bx, y: (landscape ? bottom : panelBottom) - 8 - b, width: b, height: b)
            : CGRect(x: bx, y: locateButton.frame.minY - 8 - b, width: b, height: b)
        if navigating && landscape { tiltButton.frame.origin.y = bottom - b }
        recenterButton.frame = CGRect(x: (landscape ? panelLeft + 8 : left), y: panelBottom - 8 - b,
                                      width: 130, height: b)
        attribution.frame = CGRect(x: left + 2, y: (landscape || navigating ? bottom - 14 : panelBottom - 16),
                                   width: 220, height: 14)
        if navigating && !landscape { attribution.frame.origin.y = status.frame.minY - 16 }
        if navigating && landscape { attribution.frame.origin.x = panelLeft + 8 }

        emptyNotice.frame = CGRect(x: left + 16, y: view.bounds.midY - 110, width: right - left - 32, height: 220)

        // map content area (what the camera centres in)
        let covTop = navigating && !landscape ? banner.frame.maxY : (navigating ? 0 : searchBar.frame.maxY)
        let covBottom = view.bounds.height - (navigating && !landscape ? status.frame.minY : panelBottom)
        map.setPanels(top: covTop, bottom: max(0, covBottom), left: landscape ? panelLeft : 0)
    }

    // MARK: state

    private func modeChanged() {
        let navigating: Bool
        switch mode {
        case .browse:
            navigating = false
            placeCard.isHidden = true
            previewCard.isHidden = true
        case .place(let r):
            navigating = false
            placeCard.isHidden = false
            previewCard.isHidden = true
            placeCard.title.text = r.title
            placeCard.subtitle.text = r.subtitle
            if let here = LocationService.shared.last?.coordinate {
                let d = tz_distance_m(here.latitude, here.longitude, r.coordinate.latitude, r.coordinate.longitude)
                placeCard.distance.text = "\(Format.distance(d)) away"
            } else {
                placeCard.distance.text = ""
            }
            placeCard.modes.selectedIndex = TravelMode.allCases.firstIndex(of: Settings.shared.travelMode) ?? 0
        case .preview:
            navigating = false
            placeCard.isHidden = true
            previewCard.isHidden = false
            previewCard.modes.selectedIndex = TravelMode.allCases.firstIndex(of: Settings.shared.travelMode) ?? 0
        case .navigating:
            navigating = true
            placeCard.isHidden = true
            previewCard.isHidden = true
        }
        banner.isHidden = !navigating
        status.isHidden = !navigating
        refreshButtons()
        view.setNeedsLayout()
        UIView.animate(withDuration: 0.2) { self.view.layoutIfNeeded() }
    }

    private func refreshButtons() {
        let navigating: Bool
        if case .navigating = mode { navigating = true } else { navigating = false }
        recenterButton.isHidden = !(navigating && !map.isFollowing)
        locateButton.isHidden = navigating
        layersButton.isHidden = navigating
        locateButton.icon = map.isFollowing ? .locateFilled : .locate
        tiltButton.filled = Settings.shared.tilt
    }

    // MARK: actions

    @objc private func openSearch() {
        let vc = SearchViewController()
        vc.near = LocationService.shared.last?.coordinate ?? map.mapView.centerCoordinate
        vc.onSelect = { [weak self] r in self?.select(result: r) }
        present(vc, animated: true)
    }

    @objc private func openMenu() {
        let anchor = menuButton.convert(menuButton.bounds, to: view)
        DropMenu.show([
            DropMenu.Item(title: "Map packs", icon: .packs) { [weak self] in
                self?.present(UINavigationController.themed(root: PacksViewController()), animated: true)
            },
            DropMenu.Item(title: "Settings", icon: .settings) { [weak self] in
                self?.present(UINavigationController.themed(root: SettingsViewController()), animated: true)
            },
        ], in: view, below: anchor)
    }

    @objc private func tiltTapped() {
        Settings.shared.tilt.toggle()   // MapController tilts the camera on the change
        refreshButtons()
    }

    @objc private func locateTapped() {
        let ls = LocationService.shared
        if ls.denied {
            let a = UIAlertController(title: "Location is off",
                                      message: "Allow location for TRACE ZERO in Settings → Privacy → Location Services.",
                                      preferredStyle: .alert)
            a.addAction(UIAlertAction(title: "OK", style: .default))
            present(a, animated: true)
            return
        }
        ls.set(.browse)
        map.followUser()
        refreshButtons()
    }

    @objc private func cycleRenderMode() {
        let all = RenderMode.allCases
        let i = all.firstIndex(of: Settings.shared.renderMode) ?? 0
        let next = all[(i + 1) % all.count]
        Settings.shared.renderMode = next
        Toast.show(next.title, in: view, top: searchBar.frame.maxY + 8)
    }

    @objc private func recenterTapped() {
        map.followCourse(mode: Navigator.shared.route?.mode ?? Settings.shared.travelMode)
        refreshButtons()
    }

    @objc private func closeCard() {
        routeRequest += 1
        map.showRoute(nil)
        map.setPin(nil)
        map.setDestination(nil)
        mode = .browse
    }

    private func select(coordinate c: CLLocationCoordinate2D) {
        let d = SearchEngine.describe(c)
        select(result: SearchResult(title: d.title, subtitle: d.subtitle, coordinate: c, kind: .coordinate,
                                    sub: 0, distance: nil, score: 0))
    }

    private func select(result r: SearchResult) {
        if case .navigating = mode { return }
        routeRequest += 1
        map.showRoute(nil)
        map.setDestination(nil)
        map.setPin(r.coordinate)
        mode = .place(r)
        map.center(on: r.coordinate, zoom: max(map.mapView.zoomLevel, r.kind == .place ? 13 : 16))
    }

    @objc private func modeSegmentChanged(_ s: Segments) {
        let m = TravelMode.allCases[s.selectedIndex]
        Settings.shared.travelMode = m
        if case .preview(let target, _) = mode { computeRoute(to: target) }
    }

    @objc private func routeTapped() {
        guard case .place(let r) = mode else { return }
        computeRoute(to: r)
    }

    private func computeRoute(to target: SearchResult) {
        guard let here = LocationService.shared.last else {
            mode = .preview(target: target, route: nil)
            previewCard.show(error: LocationService.shared.denied
                ? "Location is off. Allow it in Settings to route from here."
                : "Waiting for GPS position…")
            LocationService.shared.set(.browse)
            return
        }
        routeRequest += 1
        let req = routeRequest
        mode = .preview(target: target, route: nil)
        previewCard.showLoading()
        map.setPin(nil)
        map.setDestination(target.coordinate)
        let heading: CLLocationDirection? = (here.course >= 0 && here.speed > 2) ? here.course : nil
        Router.shared.route(from: here.coordinate, heading: heading, to: target.coordinate,
                            mode: Settings.shared.travelMode) { [weak self] result in
            guard let self = self, req == self.routeRequest else { return }
            switch result {
            case .success(let route):
                self.mode = .preview(target: target, route: route)
                self.previewCard.show(route: route)
                self.map.showRoute(route)
                self.view.layoutIfNeeded()
                self.map.fit(route.coordinates, padding: UIEdgeInsets(top: 40, left: 30, bottom: 40, right: 60))
            case .failure(let e):
                self.previewCard.show(error: e.description)
                self.map.showRoute(nil)
            }
        }
    }

    @objc private func startTapped() {
        guard case .preview(let target, let route?) = mode else { return }
        Navigator.shared.start(route: route, destinationName: target.title)
    }

    @objc private func endTapped() {
        Navigator.shared.stop()
    }

    @objc private func muteTapped() {
        VoiceGuide.shared.muted.toggle()
        if VoiceGuide.shared.muted { VoiceGuide.shared.stop() }
        status.muteButton.icon = VoiceGuide.shared.muted ? .mute : .unmute
    }

    @objc private func rerouteTapped() {
        Navigator.shared.reroute()
    }

    @objc private func showSteps() {
        let route: Route?
        switch mode {
        case .preview(_, let r): route = r
        case .navigating: route = Navigator.shared.route
        default: route = nil
        }
        guard let r = route else { return }
        let vc = StepsViewController(route: r, from: Navigator.shared.progress?.nextManeuver ?? 0)
        present(UINavigationController.themed(root: vc), animated: true)
    }

    // MARK: navigation updates

    @objc private func navigatorStarted() {
        guard let route = Navigator.shared.route else { return }
        mode = .navigating
        map.setPin(nil)
        map.setDestination(route.destination)
        map.showRoute(route)
        status.muteButton.icon = VoiceGuide.shared.muted ? .mute : .unmute
        map.setNavigating(true, mode: route.mode)
        navigatorUpdated()
    }

    @objc private func navigatorEnded() {
        map.setNavigating(false, mode: Settings.shared.travelMode)
        map.showRoute(nil)
        map.setDestination(nil)
        mode = .browse
    }

    @objc private func navigatorUpdated() {
        let nav = Navigator.shared
        guard nav.isActive, let route = nav.route else { return }
        if case .navigating = mode {} else { navigatorStarted(); return }
        let p = nav.progress
        if let p = p {
            map.showProgress(route: route, progress: p)
            if let d = nav.displayLocation { map.updateUserLocation(d) }
            let next = p.nextManeuver < route.maneuvers.count ? route.maneuvers[p.nextManeuver] : route.maneuvers.last
            let after = p.nextManeuver + 1 < route.maneuvers.count ? route.maneuvers[p.nextManeuver + 1] : nil
            if let m = next {
                banner.show(maneuver: m, distance: p.distanceToNextManeuver, then: after,
                            rerouting: nav.isRerouting, offRoute: p.offRoute)
            }
        } else if let first = route.maneuvers.first(where: { $0.kind != .depart }) ?? route.maneuvers.first {
            banner.show(maneuver: first, distance: first.distanceFromStart, then: nil,
                        rerouting: nav.isRerouting, offRoute: false)
        }
        status.show(progress: p, route: route)
        let showReroute = (p?.offRoute ?? false) || nav.rerouteError != nil
        if status.rerouteButton.isHidden == showReroute {
            status.rerouteButton.isHidden = !showReroute
            status.setNeedsLayout()
        }
        if let e = nav.rerouteError, !nav.isRerouting {
            Toast.show(e, in: view, top: banner.frame.maxY + 8)
        }
        if nav.arrived { status.time.text = "ARRIVED" }
        nav.updateIdleTimer()
        view.setNeedsLayout()
        refreshButtons()
    }
}

/// Magnifier drawn inside the search field.
private final class SearchGlass: UIView {
    override init(frame: CGRect) {
        super.init(frame: frame)
        backgroundColor = .clear
        isOpaque = false
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is not used") }

    override func draw(_ rect: CGRect) {
        Icons.draw(.search, in: bounds.insetBy(dx: 2, dy: 2), color: Theme.fg)
    }
}

extension UINavigationController {
    /// Navigation controller in the current look.
    static func themed(root: UIViewController) -> UINavigationController {
        let nav = UINavigationController(rootViewController: root)
        nav.navigationBar.barStyle = Theme.barStyle
        nav.navigationBar.isTranslucent = false
        nav.navigationBar.barTintColor = Theme.background
        nav.navigationBar.tintColor = Theme.fg
        nav.navigationBar.titleTextAttributes = [.foregroundColor: Theme.fg, .font: Theme.font(17, weight: .bold),
                                                 .kern: 1.5]
        nav.modalPresentationStyle = .fullScreen
        return nav
    }
}
