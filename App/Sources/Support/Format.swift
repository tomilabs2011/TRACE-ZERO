import Foundation

/// Distance, duration and clock formatting used by the HUD, lists and voice.
enum Format {
    /// "350 m", "1.2 km", "12 km" (or feet/miles when imperial).
    static func distance(_ meters: Double, imperial: Bool = Settings.shared.imperial) -> String {
        let m = max(0, meters)
        if imperial {
            let feet = m * 3.28084
            if feet < 1000 { return "\(roundTo(feet, step: feet < 300 ? 10 : 50)) ft" }
            let miles = m / 1609.344
            return miles < 10 ? String(format: "%.1f mi", miles) : "\(Int(miles.rounded())) mi"
        }
        if m < 1000 { return "\(roundTo(m, step: m < 100 ? 5 : (m < 300 ? 10 : 50))) m" }
        let km = m / 1000
        return km < 10 ? String(format: "%.1f km", km) : "\(Int(km.rounded())) km"
    }

    /// Spoken form: "350 metres", "1.2 kilometres".
    static func spokenDistance(_ meters: Double, imperial: Bool = Settings.shared.imperial) -> String {
        let m = max(0, meters)
        if imperial {
            let feet = m * 3.28084
            if feet < 1000 { return "\(roundTo(feet, step: 50)) feet" }
            let miles = m / 1609.344
            return miles < 10 ? String(format: "%.1f miles", miles) : "\(Int(miles.rounded())) miles"
        }
        if m < 1000 { return "\(roundTo(m, step: m < 300 ? 10 : 50)) metres" }
        let km = m / 1000
        return km < 10 ? String(format: "%.1f kilometres", km) : "\(Int(km.rounded())) kilometres"
    }

    /// "4 min", "1 h 05 min".
    static func duration(_ seconds: Double) -> String {
        let s = max(0, Int(seconds.rounded()))
        if s < 60 { return "<1 min" }
        let mins = (s + 30) / 60
        if mins < 60 { return "\(mins) min" }
        return String(format: "%d h %02d min", mins / 60, mins % 60)
    }

    private static let clock: DateFormatter = {
        let f = DateFormatter()
        f.dateStyle = .none
        f.timeStyle = .short
        return f
    }()

    /// Arrival clock time for a duration from now.
    static func arrival(after seconds: Double) -> String {
        return clock.string(from: Date().addingTimeInterval(seconds))
    }

    private static func roundTo(_ v: Double, step: Double) -> Int {
        return Int((v / step).rounded() * step)
    }
}
