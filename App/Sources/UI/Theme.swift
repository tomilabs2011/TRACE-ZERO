import CoreText
import UIKit

/// The TRACE//ZERO visual system. Every colour and font of the interface
/// comes from here, and depends on the current look:
///
///   standard        black, one cyan (#00CCFF), red only for the destination;
///                   condensed system font (used with the four dark styles)
///   walker          the Alan Walker monitor world: royal-navy panels, cyan
///                   text, VT323 pixel font, coral destination (WALKER style)
///   walkerInverted  the same world inverted: cyan panels, navy text
///                   (WALKER INVERTED style)
///
/// Names describe roles, not colours: `fg` is text, outlines and icons;
/// `onFg` is what is drawn on top of an `fg`-filled area (a selected button).
/// The look is fixed while a screen exists; when the map style moves to a
/// different look, AppDelegate rebuilds the interface (see applyLook).
enum Theme {
    enum Look { case standard, walker, walkerInverted }

    private(set) static var look: Look = .standard

    static func apply(_ l: Look) { look = l }

    // The WALKER palette (the same values as Tools/make_styles.py).
    static let walkerCyan = rgb(0x3ADCE8)
    static let walkerNavy = rgb(0x0B1650)
    static let walkerRoyal = rgb(0x1A2A9E)
    static let walkerCoral = rgb(0xFF6B7E)
    static let standardCyan = UIColor(red: 0.0, green: 0.8, blue: 1.0, alpha: 1.0)   // #00CCFF

    static var fg: UIColor {
        switch look {
        case .standard: return standardCyan
        case .walker: return walkerCyan
        case .walkerInverted: return walkerNavy
        }
    }
    static var fgDim: UIColor { return fg.withAlphaComponent(look == .standard ? 0.45 : 0.6) }
    static var fgFaint: UIColor { return fg.withAlphaComponent(look == .standard ? 0.16 : 0.2) }
    static var onFg: UIColor {
        switch look {
        case .standard: return .black
        case .walker: return walkerNavy
        case .walkerInverted: return walkerCyan
        }
    }
    /// Floating panels and buttons over the map.
    static var panel: UIColor {
        switch look {
        case .standard: return UIColor(white: 0.0, alpha: 0.88)
        case .walker: return walkerRoyal.withAlphaComponent(0.94)
        case .walkerInverted: return walkerCyan.withAlphaComponent(0.94)
        }
    }
    /// Full-screen backgrounds (search, settings, lists).
    static var background: UIColor {
        switch look {
        case .standard: return .black
        case .walker: return walkerNavy
        case .walkerInverted: return walkerCyan
        }
    }
    /// Destination colour.
    static var accent: UIColor {
        return look == .standard ? UIColor(red: 1.0, green: 0.169, blue: 0.227, alpha: 1.0) : walkerCoral
    }

    /// Status bar over the map (WALKER's map is light) and over full screens.
    static var mapStatusBar: UIStatusBarStyle { return look == .walker ? .default : .lightContent }
    static var screenStatusBar: UIStatusBarStyle { return look == .walkerInverted ? .default : .lightContent }
    static var barStyle: UIBarStyle { return look == .walkerInverted ? .default : .black }
    static var keyboard: UIKeyboardAppearance { return look == .walkerInverted ? .light : .dark }
    static var scrollIndicator: UIScrollView.IndicatorStyle { return look == .walkerInverted ? .black : .white }
    static var spinner: UIActivityIndicatorView.Style { return look == .walkerInverted ? .gray : .white }

    private static func rgb(_ v: Int) -> UIColor {
        return UIColor(red: CGFloat((v >> 16) & 0xFF) / 255, green: CGFloat((v >> 8) & 0xFF) / 255,
                       blue: CGFloat(v & 0xFF) / 255, alpha: 1)
    }

    /// Hairline width that is exactly one device pixel.
    static var hairline: CGFloat { return 1.0 / UIScreen.main.scale }

    /// The UI font: condensed and technical in the standard look, the VT323
    /// pixel font (bundled, App/Resources/Fonts) in the WALKER looks. VT323
    /// is small for its point size, so it is scaled to match.
    static func font(_ size: CGFloat, weight: Weight = .demi) -> UIFont {
        if look != .standard {
            if let f = UIFont(name: "VT323-Regular", size: (size * 1.3).rounded()) { return f }
            return UIFont(name: "Menlo-Bold", size: size) ?? UIFont.boldSystemFont(ofSize: size)
        }
        let name: String
        switch weight {
        case .regular: name = "AvenirNextCondensed-Medium"
        case .demi: name = "AvenirNextCondensed-DemiBold"
        case .bold: name = "AvenirNextCondensed-Bold"
        }
        if let f = UIFont(name: name, size: size) { return f }
        switch weight {
        case .regular: return UIFont.systemFont(ofSize: size)
        case .demi: return UIFont.systemFont(ofSize: size, weight: .semibold)
        case .bold: return UIFont.boldSystemFont(ofSize: size)
        }
    }

    /// Same font with tabular (fixed-width) digits so numbers do not jitter.
    /// (VT323 is monospaced already.)
    static func numberFont(_ size: CGFloat, weight: Weight = .bold) -> UIFont {
        let base = font(size, weight: weight)
        if look != .standard { return base }
        let features: [[UIFontDescriptor.FeatureKey: Int]] = [[
            .featureIdentifier: kNumberSpacingType,
            .typeIdentifier: kMonospacedNumbersSelector,
        ]]
        let desc = base.fontDescriptor.addingAttributes([.featureSettings: features])
        return UIFont(descriptor: desc, size: size)
    }

    enum Weight { case regular, demi, bold }

    /// Letter-spaced uppercase label text, the "terminal" look.
    static func tracked(_ text: String, size: CGFloat, weight: Weight = .demi,
                        color: UIColor = Theme.fg, spacing: CGFloat = 1.2) -> NSAttributedString {
        return NSAttributedString(string: text.uppercased(), attributes: [
            .font: font(size, weight: weight),
            .foregroundColor: color,
            .kern: spacing,
        ])
    }

    /// Standard bordered panel look.
    static func stylePanel(_ v: UIView, corner: CGFloat = 4) {
        v.backgroundColor = panel
        v.layer.borderColor = fg.cgColor
        v.layer.borderWidth = 1
        v.layer.cornerRadius = corner
    }
}

/// A square button with a drawn icon and a cyan outline.
final class IconButton: UIButton {
    var icon: Icon { didSet { setNeedsDisplay() } }
    var isOn = false { didSet { setNeedsDisplay() } }

    init(icon: Icon) {
        self.icon = icon
        super.init(frame: .zero)
        backgroundColor = .clear
        isOpaque = false
        contentMode = .redraw
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is not used") }

    override var isHighlighted: Bool { didSet { setNeedsDisplay() } }

    override func draw(_ rect: CGRect) {
        let r = bounds.insetBy(dx: 0.5, dy: 0.5)
        let bg = UIBezierPath(roundedRect: r, cornerRadius: 4)
        (isOn || isHighlighted ? Theme.fg : Theme.panel).setFill()
        bg.fill()
        Theme.fg.setStroke()
        bg.lineWidth = 1
        bg.stroke()
        let color = (isOn || isHighlighted) ? Theme.onFg : Theme.fg
        Icons.draw(icon, in: bounds.insetBy(dx: bounds.width * 0.24, dy: bounds.height * 0.24), color: color)
    }
}

/// A text button in the panel style ("START", "END", ...).
final class TextButton: UIButton {
    var filled: Bool { didSet { refresh() } }

    init(title: String, filled: Bool = false) {
        self.filled = filled
        super.init(frame: .zero)
        layer.cornerRadius = 4
        layer.borderWidth = 1
        layer.borderColor = Theme.fg.cgColor
        setTitle(title, for: .normal)
        refresh()
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is not used") }

    func setText(_ title: String) {
        setTitle(title, for: .normal)
        refresh()
    }

    override var isHighlighted: Bool { didSet { refresh() } }
    override var isEnabled: Bool { didSet { alpha = isEnabled ? 1 : 0.4 } }

    private func refresh() {
        let inverted = filled != isHighlighted
        backgroundColor = inverted ? Theme.fg : Theme.panel
        let title = self.title(for: .normal) ?? ""
        setAttributedTitle(Theme.tracked(title, size: 16, weight: .bold,
                                         color: inverted ? Theme.onFg : Theme.fg, spacing: 2),
                           for: .normal)
    }
}

/// Segmented selector in the house style (used for WALK / BIKE / CAR).
final class Segments: UIControl {
    private(set) var titles: [String]
    var selectedIndex = 0 { didSet { setNeedsDisplay() } }

    init(titles: [String]) {
        self.titles = titles
        super.init(frame: .zero)
        backgroundColor = .clear
        isOpaque = false
        contentMode = .redraw
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is not used") }

    override func draw(_ rect: CGRect) {
        let r = bounds.insetBy(dx: 0.5, dy: 0.5)
        let outline = UIBezierPath(roundedRect: r, cornerRadius: 4)
        Theme.panel.setFill()
        outline.fill()
        let w = r.width / CGFloat(max(1, titles.count))
        for (i, t) in titles.enumerated() {
            let cell = CGRect(x: r.minX + CGFloat(i) * w, y: r.minY, width: w, height: r.height)
            if i == selectedIndex {
                Theme.fg.setFill()
                UIBezierPath(rect: cell).fill()
            } else if i > 0 && i != selectedIndex + 1 {
                Theme.fgDim.setFill()
                UIRectFill(CGRect(x: cell.minX, y: cell.minY + 6, width: Theme.hairline, height: cell.height - 12))
            }
            let s = Theme.tracked(t, size: 14, weight: .bold, color: i == selectedIndex ? Theme.onFg : Theme.fg, spacing: 1.5)
            let size = s.size()
            s.draw(at: CGPoint(x: cell.midX - size.width / 2, y: cell.midY - size.height / 2))
        }
        Theme.fg.setStroke()
        outline.lineWidth = 1
        outline.stroke()
    }

    override func endTracking(_ touch: UITouch?, with event: UIEvent?) {
        super.endTracking(touch, with: event)
        guard let p = touch?.location(in: self), bounds.contains(p), !titles.isEmpty else { return }
        let i = min(titles.count - 1, max(0, Int(p.x / (bounds.width / CGFloat(titles.count)))))
        if i != selectedIndex {
            selectedIndex = i
            sendActions(for: .valueChanged)
        }
    }
}

extension RenderMode {
    /// The interface look that goes with this map style.
    var look: Theme.Look {
        switch self {
        case .walker: return .walker
        case .walkerInverted: return .walkerInverted
        case .outline, .hatch, .solid, .minimal: return .standard
        }
    }
}
