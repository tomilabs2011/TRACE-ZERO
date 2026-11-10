import UIKit

/// Every icon in the app is drawn with Core Graphics: SF Symbols do not exist
/// on iOS 12 and drawn icons stay crisp at any size.
enum Icon {
    case search, menu, locate, locateFilled, close, layers, packs, settings, reroute, mute, unmute, list
}

enum Icons {
    static func draw(_ icon: Icon, in r: CGRect, color: UIColor) {
        color.setStroke()
        color.setFill()
        let lw = max(1.5, r.width * 0.11)
        let p = UIBezierPath()
        p.lineWidth = lw
        p.lineCapStyle = .round
        p.lineJoinStyle = .round
        switch icon {
        case .search:
            let d = r.width * 0.62
            p.append(UIBezierPath(ovalIn: CGRect(x: r.minX, y: r.minY, width: d, height: d)))
            p.move(to: CGPoint(x: r.minX + d * 0.86, y: r.minY + d * 0.86))
            p.addLine(to: CGPoint(x: r.maxX, y: r.maxY))
            p.lineWidth = lw
            p.stroke()
        case .menu, .list:
            for i in 0..<3 {
                let y = r.minY + r.height * (0.18 + 0.32 * CGFloat(i))
                p.move(to: CGPoint(x: r.minX, y: y))
                p.addLine(to: CGPoint(x: r.maxX, y: y))
            }
            p.stroke()
        case .locate, .locateFilled:
            let c = CGPoint(x: r.midX, y: r.midY)
            let rad = r.width * 0.32
            p.append(UIBezierPath(arcCenter: c, radius: rad, startAngle: 0, endAngle: .pi * 2, clockwise: true))
            p.move(to: CGPoint(x: r.midX, y: r.minY)); p.addLine(to: CGPoint(x: r.midX, y: c.y - rad))
            p.move(to: CGPoint(x: r.midX, y: c.y + rad)); p.addLine(to: CGPoint(x: r.midX, y: r.maxY))
            p.move(to: CGPoint(x: r.minX, y: r.midY)); p.addLine(to: CGPoint(x: c.x - rad, y: r.midY))
            p.move(to: CGPoint(x: c.x + rad, y: r.midY)); p.addLine(to: CGPoint(x: r.maxX, y: r.midY))
            p.stroke()
            if icon == .locateFilled {
                UIBezierPath(arcCenter: c, radius: rad * 0.45, startAngle: 0, endAngle: .pi * 2, clockwise: true).fill()
            }
        case .close:
            p.move(to: CGPoint(x: r.minX, y: r.minY)); p.addLine(to: CGPoint(x: r.maxX, y: r.maxY))
            p.move(to: CGPoint(x: r.maxX, y: r.minY)); p.addLine(to: CGPoint(x: r.minX, y: r.maxY))
            p.stroke()
        case .layers:
            // three stacked diamonds
            for i in 0..<3 {
                let y = r.minY + r.height * (0.25 + 0.25 * CGFloat(i))
                let q = UIBezierPath()
                q.lineWidth = lw * 0.8
                q.lineJoinStyle = .round
                q.move(to: CGPoint(x: r.minX, y: y))
                q.addLine(to: CGPoint(x: r.midX, y: y - r.height * 0.2))
                q.addLine(to: CGPoint(x: r.maxX, y: y))
                q.addLine(to: CGPoint(x: r.midX, y: y + r.height * 0.2))
                q.close()
                q.stroke()
            }
        case .packs:
            // folder
            p.move(to: CGPoint(x: r.minX, y: r.maxY - r.height * 0.1))
            p.addLine(to: CGPoint(x: r.minX, y: r.minY + r.height * 0.15))
            p.addLine(to: CGPoint(x: r.minX + r.width * 0.38, y: r.minY + r.height * 0.15))
            p.addLine(to: CGPoint(x: r.minX + r.width * 0.5, y: r.minY + r.height * 0.3))
            p.addLine(to: CGPoint(x: r.maxX, y: r.minY + r.height * 0.3))
            p.addLine(to: CGPoint(x: r.maxX, y: r.maxY - r.height * 0.1))
            p.close()
            p.stroke()
        case .settings:
            let c = CGPoint(x: r.midX, y: r.midY)
            let inner: CGFloat = r.width * 0.3, outer: CGFloat = r.width * 0.5
            for i in 0..<8 {
                let a: CGFloat = CGFloat(i) * CGFloat.pi / 4
                p.move(to: CGPoint(x: c.x + cos(a) * inner, y: c.y + sin(a) * inner))
                p.addLine(to: CGPoint(x: c.x + cos(a) * outer, y: c.y + sin(a) * outer))
            }
            p.append(UIBezierPath(arcCenter: c, radius: r.width * 0.26, startAngle: 0, endAngle: CGFloat.pi * 2, clockwise: true))
            p.stroke()
        case .reroute:
            let c = CGPoint(x: r.midX, y: r.midY)
            let rad: CGFloat = r.width * 0.42
            let a0: CGFloat = -CGFloat.pi * 0.35
            let a1: CGFloat = CGFloat.pi * 1.35
            p.addArc(withCenter: c, radius: rad, startAngle: a0, endAngle: a1, clockwise: true)
            p.stroke()
            let tip = CGPoint(x: c.x + cos(a0) * rad, y: c.y + sin(a0) * rad)
            let head = UIBezierPath()
            head.move(to: CGPoint(x: tip.x - r.width * 0.28, y: tip.y - r.height * 0.02))
            head.addLine(to: tip)
            head.addLine(to: CGPoint(x: tip.x + r.width * 0.02, y: tip.y - r.height * 0.3))
            head.lineWidth = lw
            head.lineCapStyle = .round
            head.lineJoinStyle = .round
            head.stroke()
        case .mute, .unmute:
            // speaker
            let s = UIBezierPath()
            s.move(to: CGPoint(x: r.minX, y: r.minY + r.height * 0.35))
            s.addLine(to: CGPoint(x: r.minX + r.width * 0.22, y: r.minY + r.height * 0.35))
            s.addLine(to: CGPoint(x: r.minX + r.width * 0.5, y: r.minY + r.height * 0.1))
            s.addLine(to: CGPoint(x: r.minX + r.width * 0.5, y: r.maxY - r.height * 0.1))
            s.addLine(to: CGPoint(x: r.minX + r.width * 0.22, y: r.maxY - r.height * 0.35))
            s.addLine(to: CGPoint(x: r.minX, y: r.maxY - r.height * 0.35))
            s.close()
            s.fill()
            if icon == .mute {
                p.move(to: CGPoint(x: r.minX + r.width * 0.66, y: r.minY + r.height * 0.32))
                p.addLine(to: CGPoint(x: r.maxX, y: r.maxY - r.height * 0.32))
                p.move(to: CGPoint(x: r.maxX, y: r.minY + r.height * 0.32))
                p.addLine(to: CGPoint(x: r.minX + r.width * 0.66, y: r.maxY - r.height * 0.32))
            } else {
                let c = CGPoint(x: r.minX + r.width * 0.5, y: r.midY)
                let q: CGFloat = CGFloat.pi / 4
                let r1: CGFloat = r.width * 0.25, r2: CGFloat = r.width * 0.45
                p.move(to: CGPoint(x: c.x + cos(-q) * r1, y: c.y + sin(-q) * r1))
                p.addArc(withCenter: c, radius: r1, startAngle: -q, endAngle: q, clockwise: true)
                p.move(to: CGPoint(x: c.x + cos(-q) * r2, y: c.y + sin(-q) * r2))
                p.addArc(withCenter: c, radius: r2, startAngle: -q, endAngle: q, clockwise: true)
            }
            p.stroke()
        }
    }
}

/// Draws the arrow for a maneuver: the road comes in from the bottom and
/// leaves at the turn angle. Used by the navigation banner and for CarPlay.
enum ManeuverGlyph {
    /// angle: degrees, 0 = straight, +90 = right, -90 = left; uturn draws a U.
    static func draw(in r: CGRect, angle: Double, uturn: Bool, roundaboutExit: Int?, arrive: Bool,
                     color: UIColor, dimColor: UIColor) {
        let w = r.width
        let lw = w * 0.12
        let base = CGPoint(x: r.midX, y: r.maxY - w * 0.04)
        let joint = CGPoint(x: r.midX, y: r.minY + r.height * 0.52)
        color.setStroke()
        color.setFill()

        if arrive {
            // flag on a pole
            let pole = UIBezierPath()
            pole.lineWidth = lw * 0.8
            pole.lineCapStyle = .round
            pole.move(to: CGPoint(x: r.minX + w * 0.3, y: r.maxY - w * 0.05))
            pole.addLine(to: CGPoint(x: r.minX + w * 0.3, y: r.minY + w * 0.08))
            pole.stroke()
            let flag = UIBezierPath()
            flag.move(to: CGPoint(x: r.minX + w * 0.3, y: r.minY + w * 0.08))
            flag.addLine(to: CGPoint(x: r.maxX - w * 0.1, y: r.minY + w * 0.25))
            flag.addLine(to: CGPoint(x: r.minX + w * 0.3, y: r.minY + w * 0.45))
            flag.close()
            flag.fill()
            return
        }

        if let exit = roundaboutExit {
            let c = CGPoint(x: r.midX, y: r.midY + w * 0.08)
            let rad = w * 0.22
            let ring = UIBezierPath(arcCenter: c, radius: rad, startAngle: 0, endAngle: .pi * 2, clockwise: true)
            ring.lineWidth = lw * 0.7
            dimColor.setStroke()
            ring.stroke()
            color.setStroke()
            let entry = UIBezierPath()
            entry.lineWidth = lw
            entry.lineCapStyle = .round
            entry.move(to: base)
            entry.addLine(to: CGPoint(x: c.x, y: c.y + rad))
            entry.stroke()
            // exit direction from the given angle
            let a = CGFloat((angle - 90) * Double.pi / 180)
            let from = CGPoint(x: c.x + cos(a) * rad, y: c.y + sin(a) * rad)
            let to = CGPoint(x: c.x + cos(a) * w * 0.46, y: c.y + sin(a) * w * 0.46)
            arrow(from: from, to: to, lw: lw, head: w * 0.2)
            if exit > 0 {
                let s = NSAttributedString(string: "\(exit)", attributes: [
                    .font: Theme.font(w * 0.26, weight: .bold), .foregroundColor: color])
                let sz = s.size()
                s.draw(at: CGPoint(x: c.x - sz.width / 2, y: c.y - sz.height / 2))
            }
            return
        }

        if uturn {
            let p = UIBezierPath()
            p.lineWidth = lw
            p.lineCapStyle = .round
            let right = r.midX + w * 0.16, left = r.midX - w * 0.16
            p.move(to: CGPoint(x: right, y: base.y))
            p.addLine(to: CGPoint(x: right, y: r.minY + w * 0.4))
            p.addArc(withCenter: CGPoint(x: r.midX, y: r.minY + w * 0.4), radius: w * 0.16,
                     startAngle: 0, endAngle: .pi, clockwise: false)
            p.addLine(to: CGPoint(x: left, y: r.minY + w * 0.62))
            p.stroke()
            arrow(from: CGPoint(x: left, y: r.minY + w * 0.5), to: CGPoint(x: left, y: r.minY + w * 0.78),
                  lw: lw, head: w * 0.22)
            return
        }

        let stem = UIBezierPath()
        stem.lineWidth = lw
        stem.lineCapStyle = .round
        stem.lineJoinStyle = .round
        stem.move(to: base)
        stem.addLine(to: joint)
        stem.stroke()
        let a = CGFloat((angle - 90) * Double.pi / 180)
        let len = r.height * 0.44
        let end = CGPoint(x: joint.x + cos(a) * len, y: joint.y + sin(a) * len)
        arrow(from: joint, to: end, lw: lw, head: w * 0.24)
    }

    private static func arrow(from: CGPoint, to: CGPoint, lw: CGFloat, head: CGFloat) {
        let dx = to.x - from.x, dy = to.y - from.y
        let len = max(0.001, sqrt(dx * dx + dy * dy))
        let ux = dx / len, uy = dy / len
        let shaftEnd = CGPoint(x: to.x - ux * head * 0.6, y: to.y - uy * head * 0.6)
        let shaft = UIBezierPath()
        shaft.lineWidth = lw
        shaft.lineCapStyle = .round
        shaft.lineJoinStyle = .round
        shaft.move(to: from)
        shaft.addLine(to: shaftEnd)
        shaft.stroke()
        let tri = UIBezierPath()
        tri.move(to: to)
        tri.addLine(to: CGPoint(x: to.x - ux * head - uy * head * 0.62, y: to.y - uy * head + ux * head * 0.62))
        tri.addLine(to: CGPoint(x: to.x - ux * head + uy * head * 0.62, y: to.y - uy * head - ux * head * 0.62))
        tri.close()
        tri.fill()
    }

    /// The glyph as an image (for CarPlay maneuvers and table cells).
    static func image(size: CGSize, angle: Double, uturn: Bool, roundaboutExit: Int?, arrive: Bool,
                      color: UIColor) -> UIImage {
        UIGraphicsBeginImageContextWithOptions(size, false, 0)
        defer { UIGraphicsEndImageContext() }
        draw(in: CGRect(origin: .zero, size: size), angle: angle, uturn: uturn,
             roundaboutExit: roundaboutExit, arrive: arrive, color: color, dimColor: color.withAlphaComponent(0.4))
        return UIGraphicsGetImageFromCurrentImageContext() ?? UIImage()
    }
}
