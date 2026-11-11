import UIKit

/// Draws the arrow for one maneuver.
final class ManeuverGlyphView: UIView {
    var maneuver: Maneuver? { didSet { setNeedsDisplay() } }

    override init(frame: CGRect) {
        super.init(frame: frame)
        backgroundColor = .clear
        isOpaque = false
        contentMode = .redraw
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is not used") }

    override func draw(_ rect: CGRect) {
        guard let m = maneuver else { return }
        ManeuverGlyph.draw(in: bounds.insetBy(dx: 2, dy: 2), angle: m.glyphAngle, uturn: m.modifier == 4 && m.kind == .turn,
                           roundaboutExit: m.kind == .roundabout ? m.exit : nil, arrive: m.kind == .arrive,
                           color: Theme.fg, dimColor: Theme.fgDim)
    }
}

private func makeLabel(_ size: CGFloat, _ weight: Theme.Weight, lines: Int = 1, numbers: Bool = false) -> UILabel {
    let l = UILabel()
    l.font = numbers ? Theme.numberFont(size, weight: weight) : Theme.font(size, weight: weight)
    l.textColor = Theme.fg
    l.numberOfLines = lines
    l.adjustsFontSizeToFitWidth = lines == 1
    l.minimumScaleFactor = 0.6
    l.lineBreakMode = .byTruncatingTail
    return l
}

/// Top banner while navigating: arrow, distance, what to do, where.
final class NavBannerView: UIView {
    let glyph = ManeuverGlyphView()
    let distance = makeLabel(30, .bold, numbers: true)
    let primary = makeLabel(18, .demi)
    let road = makeLabel(20, .bold, lines: 2)
    let thenLabel = makeLabel(13, .demi)
    let thenGlyph = ManeuverGlyphView()
    private let thenBar = UIView()
    private(set) var showsThen = false

    override init(frame: CGRect) {
        super.init(frame: frame)
        Theme.stylePanel(self)
        thenBar.backgroundColor = Theme.fgFaint
        thenLabel.textColor = Theme.fg
        for v in [glyph, distance, primary, road, thenBar, thenGlyph, thenLabel] { addSubview(v) }
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is not used") }

    func show(maneuver m: Maneuver, distance d: Double, then next: Maneuver?, rerouting: Bool, offRoute: Bool) {
        glyph.maneuver = m
        if rerouting {
            distance.text = "REROUTING"
            primary.text = "Finding a new route…"
            road.text = ""
        } else if offRoute {
            distance.text = "OFF ROUTE"
            primary.text = "Return to the route"
            road.text = Settings.shared.autoReroute ? "or wait for a new route" : "or tap REROUTE"
        } else {
            distance.text = m.kind == .arrive && d < 30 ? "HERE" : Format.distance(d)
            primary.text = Instructions.primary(m)
            let r = m.kind == .arrive ? "Destination" : m.road
            road.text = r.isEmpty ? Instructions.secondary(m) : r
        }
        // "then" strip when the next maneuver follows closely
        if let n = next, !rerouting, !offRoute, n.distanceFromStart - m.distanceFromStart < 150 {
            showsThen = true
            thenGlyph.maneuver = n
            thenLabel.text = "THEN  " + Instructions.primary(n).uppercased()
        } else {
            showsThen = false
        }
        thenBar.isHidden = !showsThen
        thenGlyph.isHidden = !showsThen
        thenLabel.isHidden = !showsThen
        setNeedsLayout()
    }

    /// Height for a given width (the caller sizes the banner).
    func preferredHeight(width: CGFloat) -> CGFloat {
        return 104 + (showsThen ? 28 : 0)
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        let pad: CGFloat = 10
        let g: CGFloat = 64
        glyph.frame = CGRect(x: pad, y: pad + 4, width: g, height: g)
        let x = glyph.frame.maxX + 12
        let w = bounds.width - x - pad
        distance.frame = CGRect(x: x, y: pad - 2, width: w, height: 34)
        primary.frame = CGRect(x: x, y: distance.frame.maxY - 2, width: w, height: 22)
        road.frame = CGRect(x: pad, y: glyph.frame.maxY + 2, width: bounds.width - 2 * pad, height: 24)
        if road.frame.maxY > bounds.height - (showsThen ? 28 : 0) - 4 {
            road.frame.size.height = max(0, bounds.height - (showsThen ? 28 : 0) - 4 - road.frame.minY)
        }
        let ty = bounds.height - 28
        thenBar.frame = CGRect(x: 1, y: ty, width: bounds.width - 2, height: 27)
        thenGlyph.frame = CGRect(x: pad, y: ty + 3, width: 21, height: 21)
        thenLabel.frame = CGRect(x: thenGlyph.frame.maxX + 8, y: ty, width: bounds.width - thenGlyph.frame.maxX - 16, height: 28)
    }
}

/// Bottom bar while navigating: time left, distance, arrival, controls.
final class NavStatusView: UIView {
    let time = makeLabel(26, .bold, numbers: true)
    let detail = makeLabel(15, .demi, numbers: true)
    let endButton = TextButton(title: "END")
    let muteButton = IconButton(icon: .unmute)
    let rerouteButton = IconButton(icon: .reroute)
    let stepsButton = IconButton(icon: .list)

    override init(frame: CGRect) {
        super.init(frame: frame)
        Theme.stylePanel(self)
        for v in [time, detail, endButton, muteButton, rerouteButton, stepsButton] { addSubview(v) }
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is not used") }

    func show(progress p: NavProgress?, route: Route) {
        let remainingTime = p?.remainingTime ?? route.duration
        let remainingDist = p?.remainingDistance ?? route.distance
        time.text = Format.duration(remainingTime)
        detail.text = "\(Format.distance(remainingDist))  ·  ETA \(Format.arrival(after: remainingTime))"
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        let pad: CGFloat = 8
        let b: CGFloat = 44
        endButton.frame = CGRect(x: bounds.width - pad - 66, y: (bounds.height - b) / 2, width: 66, height: b)
        muteButton.frame = CGRect(x: endButton.frame.minX - 6 - b, y: endButton.frame.minY, width: b, height: b)
        stepsButton.frame = CGRect(x: muteButton.frame.minX - 6 - b, y: endButton.frame.minY, width: b, height: b)
        rerouteButton.frame = CGRect(x: stepsButton.frame.minX - 6 - b, y: endButton.frame.minY, width: b, height: b)
        let rightEdge = (rerouteButton.isHidden ? stepsButton.frame.minX : rerouteButton.frame.minX) - 6
        time.frame = CGRect(x: pad + 4, y: 6, width: rightEdge - pad - 4, height: 30)
        detail.frame = CGRect(x: pad + 4, y: time.frame.maxY, width: rightEdge - pad - 4, height: 20)
    }
}

/// Card for a selected place (search result or dropped pin).
final class PlaceCardView: UIView {
    let title = makeLabel(20, .bold, lines: 2)
    let subtitle = makeLabel(14, .regular)
    let distance = makeLabel(14, .demi, numbers: true)
    let modes = Segments(titles: TravelMode.allCases.map { $0.title })
    let routeButton = TextButton(title: "ROUTE", filled: true)
    let closeButton = IconButton(icon: .close)

    override init(frame: CGRect) {
        super.init(frame: frame)
        Theme.stylePanel(self)
        subtitle.textColor = Theme.fgDim
        subtitle.adjustsFontSizeToFitWidth = false
        for v in [title, subtitle, distance, modes, routeButton, closeButton] { addSubview(v) }
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is not used") }

    static let height: CGFloat = 158

    override func layoutSubviews() {
        super.layoutSubviews()
        let pad: CGFloat = 10
        closeButton.frame = CGRect(x: bounds.width - pad - 36, y: pad, width: 36, height: 36)
        let w = closeButton.frame.minX - pad - 8
        title.frame = CGRect(x: pad + 2, y: pad - 2, width: w, height: 26)
        subtitle.frame = CGRect(x: pad + 2, y: title.frame.maxY, width: w, height: 18)
        distance.frame = CGRect(x: pad + 2, y: subtitle.frame.maxY + 2, width: w, height: 18)
        let by = bounds.height - pad - 44
        routeButton.frame = CGRect(x: bounds.width - pad - 96, y: by, width: 96, height: 44)
        modes.frame = CGRect(x: pad, y: by, width: routeButton.frame.minX - pad - 8, height: 44)
    }
}

/// Card for a computed route before starting.
final class RoutePreviewView: UIView {
    let time = makeLabel(28, .bold, numbers: true)
    let detail = makeLabel(15, .demi, numbers: true)
    let message = makeLabel(15, .demi, lines: 2)
    let modes = Segments(titles: TravelMode.allCases.map { $0.title })
    let startButton = TextButton(title: "START", filled: true)
    let stepsButton = IconButton(icon: .list)
    let closeButton = IconButton(icon: .close)
    let spinner = UIActivityIndicatorView(style: Theme.spinner)

    override init(frame: CGRect) {
        super.init(frame: frame)
        Theme.stylePanel(self)
        spinner.color = Theme.fg
        spinner.hidesWhenStopped = true
        for v in [time, detail, message, modes, startButton, stepsButton, closeButton, spinner] { addSubview(v) }
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is not used") }

    static let height: CGFloat = 158

    func showLoading() {
        time.text = ""
        detail.text = ""
        message.text = ""
        spinner.startAnimating()
        startButton.isEnabled = false
        stepsButton.isEnabled = false
    }

    func show(route: Route) {
        spinner.stopAnimating()
        message.text = ""
        time.text = Format.duration(route.duration)
        detail.text = "\(Format.distance(route.distance))  ·  ETA \(Format.arrival(after: route.duration))"
        startButton.isEnabled = true
        stepsButton.isEnabled = true
    }

    func show(error: String) {
        spinner.stopAnimating()
        time.text = ""
        detail.text = ""
        message.text = error
        startButton.isEnabled = false
        stepsButton.isEnabled = false
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        let pad: CGFloat = 10
        closeButton.frame = CGRect(x: bounds.width - pad - 36, y: pad, width: 36, height: 36)
        stepsButton.frame = CGRect(x: closeButton.frame.minX - 8 - 36, y: pad, width: 36, height: 36)
        let w = stepsButton.frame.minX - pad - 8
        time.frame = CGRect(x: pad + 2, y: pad - 4, width: w, height: 34)
        detail.frame = CGRect(x: pad + 2, y: time.frame.maxY, width: w, height: 20)
        message.frame = CGRect(x: pad + 2, y: pad, width: w, height: 54)
        spinner.center = CGPoint(x: pad + 20, y: pad + 22)
        let by = bounds.height - pad - 44
        startButton.frame = CGRect(x: bounds.width - pad - 96, y: by, width: 96, height: 44)
        modes.frame = CGRect(x: pad, y: by, width: startButton.frame.minX - pad - 8, height: 44)
    }
}

/// Brief message at the top of the screen.
final class Toast {
    static func show(_ text: String, in view: UIView, top: CGFloat) {
        let l = UILabel()
        l.attributedText = Theme.tracked(text, size: 14, weight: .bold, color: Theme.onFg, spacing: 1.2)
        l.textAlignment = .center
        l.backgroundColor = Theme.fg
        l.layer.cornerRadius = 4
        l.clipsToBounds = true
        l.numberOfLines = 2
        let w = min(view.bounds.width - 32, 340)
        let size = l.sizeThatFits(CGSize(width: w - 20, height: 60))
        l.frame = CGRect(x: (view.bounds.width - w) / 2, y: top, width: w, height: size.height + 16)
        l.alpha = 0
        view.addSubview(l)
        UIView.animate(withDuration: 0.2, animations: { l.alpha = 1 }, completion: { _ in
            UIView.animate(withDuration: 0.3, delay: 2.2, options: [], animations: { l.alpha = 0 },
                           completion: { _ in l.removeFromSuperview() })
        })
    }
}
