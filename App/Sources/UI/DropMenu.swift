import UIKit

/// A small menu drawn by the app itself (black panel, cyan rows), used
/// instead of UIAlertController action sheets so it always matches the app
/// and is not restyled by system appearance tweaks.
final class DropMenu: UIView {
    struct Item {
        let title: String
        let icon: Icon
        let action: () -> Void
    }

    private let panel = UIView()
    private let items: [Item]
    private var rows: [MenuRow] = []
    private let anchor: CGRect

    static let rowHeight: CGFloat = 52
    static let width: CGFloat = 210

    /// Shows the menu in `view`, hanging below `anchor` (in view coordinates).
    @discardableResult
    static func show(_ items: [Item], in view: UIView, below anchor: CGRect) -> DropMenu {
        let m = DropMenu(items: items, anchor: anchor)
        m.frame = view.bounds
        m.autoresizingMask = [.flexibleWidth, .flexibleHeight]
        view.addSubview(m)
        m.alpha = 0
        m.panel.transform = CGAffineTransform(translationX: 0, y: -8)
        UIView.animate(withDuration: 0.15) {
            m.alpha = 1
            m.panel.transform = .identity
        }
        return m
    }

    private init(items: [Item], anchor: CGRect) {
        self.items = items
        self.anchor = anchor
        super.init(frame: .zero)
        backgroundColor = UIColor(white: 0, alpha: 0.35)
        Theme.stylePanel(panel)
        panel.clipsToBounds = true
        addSubview(panel)
        for (i, item) in items.enumerated() {
            let row = MenuRow(item: item, separator: i > 0)
            row.addTarget(self, action: #selector(rowTapped(_:)), for: .touchUpInside)
            panel.addSubview(row)
            rows.append(row)
        }
        let tap = UITapGestureRecognizer(target: self, action: #selector(backgroundTapped(_:)))
        tap.cancelsTouchesInView = false
        addGestureRecognizer(tap)
        isAccessibilityElement = false
        accessibilityViewIsModal = true
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is not used") }

    override func layoutSubviews() {
        super.layoutSubviews()
        let h = DropMenu.rowHeight * CGFloat(items.count)
        let x = min(max(8, anchor.maxX - DropMenu.width), bounds.width - DropMenu.width - 8)
        panel.frame = CGRect(x: x, y: anchor.maxY + 6, width: DropMenu.width, height: h)
        for (i, row) in rows.enumerated() {
            row.frame = CGRect(x: 0, y: CGFloat(i) * DropMenu.rowHeight, width: DropMenu.width, height: DropMenu.rowHeight)
        }
    }

    @objc private func rowTapped(_ row: MenuRow) {
        let action = row.item.action
        dismiss { action() }
    }

    @objc private func backgroundTapped(_ g: UITapGestureRecognizer) {
        if !panel.frame.contains(g.location(in: self)) { dismiss(nil) }
    }

    func dismiss(_ completion: (() -> Void)?) {
        UIView.animate(withDuration: 0.12, animations: { self.alpha = 0 }, completion: { _ in
            self.removeFromSuperview()
            completion?()
        })
    }
}

private final class MenuRow: UIControl {
    let item: DropMenu.Item
    private let separator: Bool

    init(item: DropMenu.Item, separator: Bool) {
        self.item = item
        self.separator = separator
        super.init(frame: .zero)
        backgroundColor = .clear
        isOpaque = false
        contentMode = .redraw
        isAccessibilityElement = true
        accessibilityLabel = item.title
        accessibilityTraits = .button
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is not used") }

    override var isHighlighted: Bool { didSet { setNeedsDisplay() } }

    override func draw(_ rect: CGRect) {
        let fg = isHighlighted ? Theme.black : Theme.cyan
        if isHighlighted {
            Theme.cyan.setFill()
            UIRectFill(bounds)
        }
        if separator {
            Theme.cyanDim.setFill()
            UIRectFill(CGRect(x: 12, y: 0, width: bounds.width - 24, height: Theme.hairline))
        }
        let s: CGFloat = 20
        Icons.draw(item.icon, in: CGRect(x: 16, y: (bounds.height - s) / 2, width: s, height: s), color: fg)
        let title = Theme.tracked(item.title, size: 16, weight: .bold, color: fg, spacing: 1.5)
        let size = title.size()
        title.draw(at: CGPoint(x: 50, y: (bounds.height - size.height) / 2))
    }
}
