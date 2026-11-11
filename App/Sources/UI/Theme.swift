import CoreText
import UIKit

/// The TRACE//ZERO visual system: black, one cyan (#00CCFF), and red only for
/// the destination. Fonts are system fonts available since iOS 9, so nothing
/// needs to be bundled.
enum Theme {
    static let cyan = UIColor(red: 0.0, green: 0.8, blue: 1.0, alpha: 1.0)          // #00CCFF
    static let cyanDim = UIColor(red: 0.0, green: 0.8, blue: 1.0, alpha: 0.45)
    static let cyanFaint = UIColor(red: 0.0, green: 0.8, blue: 1.0, alpha: 0.16)
    static let red = UIColor(red: 1.0, green: 0.169, blue: 0.227, alpha: 1.0)       // #FF2B3A
    static let black = UIColor.black
    static let panel = UIColor(white: 0.0, alpha: 0.88)
    /// Navy line colour of the SCREEN map style (#0B1650, see Tools/make_styles.py).
    static let screenInk = UIColor(red: 11 / 255.0, green: 22 / 255.0, blue: 80 / 255.0, alpha: 1.0)
    /// Background of the SCREEN map style (#3ADCE8).
    static let screenBackground = UIColor(red: 58 / 255.0, green: 220 / 255.0, blue: 232 / 255.0, alpha: 1.0)

    /// Hairline width that is exactly one device pixel.
    static var hairline: CGFloat { return 1.0 / UIScreen.main.scale }

    /// Condensed, technical-looking UI font with a safe fallback.
    static func font(_ size: CGFloat, weight: Weight = .demi) -> UIFont {
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
    static func numberFont(_ size: CGFloat, weight: Weight = .bold) -> UIFont {
        let base = font(size, weight: weight)
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
                        color: UIColor = Theme.cyan, spacing: CGFloat = 1.2) -> NSAttributedString {
        return NSAttributedString(string: text.uppercased(), attributes: [
            .font: font(size, weight: weight),
            .foregroundColor: color,
            .kern: spacing,
        ])
    }

    /// Standard bordered panel look.
    static func stylePanel(_ v: UIView, corner: CGFloat = 4) {
        v.backgroundColor = panel
        v.layer.borderColor = cyan.cgColor
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
        (isOn || isHighlighted ? Theme.cyan : Theme.panel).setFill()
        bg.fill()
        Theme.cyan.setStroke()
        bg.lineWidth = 1
        bg.stroke()
        let color = (isOn || isHighlighted) ? Theme.black : Theme.cyan
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
        layer.borderColor = Theme.cyan.cgColor
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
        backgroundColor = inverted ? Theme.cyan : Theme.panel
        let title = self.title(for: .normal) ?? ""
        setAttributedTitle(Theme.tracked(title, size: 16, weight: .bold,
                                         color: inverted ? Theme.black : Theme.cyan, spacing: 2),
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
                Theme.cyan.setFill()
                UIBezierPath(rect: cell).fill()
            } else if i > 0 && i != selectedIndex + 1 {
                Theme.cyanDim.setFill()
                UIRectFill(CGRect(x: cell.minX, y: cell.minY + 6, width: Theme.hairline, height: cell.height - 12))
            }
            let s = Theme.tracked(t, size: 14, weight: .bold, color: i == selectedIndex ? Theme.black : Theme.cyan, spacing: 1.5)
            let size = s.size()
            s.draw(at: CGPoint(x: cell.midX - size.width / 2, y: cell.midY - size.height / 2))
        }
        Theme.cyan.setStroke()
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
