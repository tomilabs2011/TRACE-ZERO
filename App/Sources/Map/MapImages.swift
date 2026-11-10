import UIKit

/// Images the styles refer to ("hatch", "hatch-water", "chevron"). Drawn at
/// runtime so no sprite sheet is needed; the render test in Tools/ draws the
/// same shapes on a canvas.
enum MapImages {
    static let names = ["hatch", "hatch-water", "chevron"]

    static func image(named name: String) -> UIImage? {
        switch name {
        case "hatch": return hatch(size: 12, alpha: 0.30)
        case "hatch-water": return hatch(size: 8, alpha: 0.55)
        case "chevron": return chevron()
        default: return nil
        }
    }

    /// One diagonal line per tile; tiles join into continuous 45° hatching.
    private static func hatch(size: CGFloat, alpha: CGFloat) -> UIImage {
        let s = CGSize(width: size, height: size)
        return render(s) { ctx in
            ctx.setStrokeColor(Theme.cyan.withAlphaComponent(alpha).cgColor)
            ctx.setLineWidth(1)
            ctx.move(to: CGPoint(x: -1, y: size + 1))
            ctx.addLine(to: CGPoint(x: size + 1, y: -1))
            ctx.strokePath()
        }
    }

    /// Black ">" chevron. Line-placed icons point along the line's direction,
    /// which is +x in image space, so the chevron points right.
    private static func chevron() -> UIImage {
        let s = CGSize(width: 24, height: 24)
        return render(s) { ctx in
            ctx.setStrokeColor(UIColor.black.cgColor)
            ctx.setLineWidth(4.5)
            ctx.setLineCap(.round)
            ctx.setLineJoin(.round)
            ctx.move(to: CGPoint(x: 8, y: 5))
            ctx.addLine(to: CGPoint(x: 16, y: 12))
            ctx.addLine(to: CGPoint(x: 8, y: 19))
            ctx.strokePath()
        }
    }

    private static func render(_ size: CGSize, _ draw: (CGContext) -> Void) -> UIImage {
        UIGraphicsBeginImageContextWithOptions(size, false, UIScreen.main.scale)
        defer { UIGraphicsEndImageContext() }
        guard let ctx = UIGraphicsGetCurrentContext() else { return UIImage() }
        draw(ctx)
        return UIGraphicsGetImageFromCurrentImageContext() ?? UIImage()
    }
}
