import Foundation

/// Human-readable text for maneuvers, shared by the banner, the step list,
/// voice guidance and CarPlay. English only for now; every string the user
/// hears or reads is built here, so localisation means changing one file.
enum Instructions {
    static func verb(_ m: Maneuver) -> String {
        switch m.kind {
        case .depart: return "Head out"
        case .arrive: return "Arrive"
        case .ferry: return "Take the ferry"
        case .roundabout:
            if m.exit <= 0 { return "Enter the roundabout" }
            return "Take the \(ordinal(m.exit)) exit"
        case .ramp: return m.modifier < 0 ? "Take the ramp on the left" : "Take the ramp on the right"
        case .fork: return m.modifier < 0 ? "Keep left" : "Keep right"
        case .continueOn: return "Continue"
        case .turn:
            switch m.modifier {
            case -3: return "Sharp left"
            case -2: return "Turn left"
            case -1: return "Slight left"
            case 1: return "Slight right"
            case 2: return "Turn right"
            case 3: return "Sharp right"
            case 4: return "Make a U-turn"
            default: return "Go straight"
            }
        }
    }

    /// Main banner line: what to do.
    static func primary(_ m: Maneuver) -> String {
        if m.kind == .roundabout && m.exit > 0 { return "Roundabout · exit \(m.exit)" }
        return verb(m)
    }

    /// Second banner line: where.
    static func secondary(_ m: Maneuver) -> String {
        if m.kind == .arrive { return "at your destination" }
        let road = m.road
        if road.isEmpty { return "" }
        switch m.kind {
        case .ramp: return "towards \(road)"
        default: return "onto \(road)"
        }
    }

    /// Full sentence for lists and voice: "Turn left onto Váci út".
    static func sentence(_ m: Maneuver) -> String {
        if m.kind == .arrive { return "You have arrived" }
        if m.kind == .depart { return m.road.isEmpty ? "Head out" : "Head out on \(m.road)" }
        let s = secondary(m)
        let v: String
        if m.kind == .roundabout && m.exit > 0 { v = "At the roundabout, take the \(ordinal(m.exit)) exit" }
        else { v = verb(m) }
        return s.isEmpty ? v : "\(v) \(s)"
    }

    /// Spoken announcement, with distance unless the maneuver is imminent.
    static func spoken(_ m: Maneuver, in distance: Double?, then next: Maneuver?) -> String {
        var text: String
        if m.kind == .arrive {
            text = distance.map { "In \(Format.spokenDistance($0)), you will arrive at your destination" }
                ?? "You have arrived at your destination"
        } else if let d = distance {
            text = "In \(Format.spokenDistance(d)), " + lowerFirst(sentence(m))
        } else {
            text = sentence(m)
        }
        // "then" when two maneuvers come right after each other
        if distance == nil, let n = next, n.kind != .arrive, n.distanceFromStart - m.distanceFromStart < 60 {
            text += ", then " + lowerFirst(sentence(n))
        }
        return text
    }

    private static func lowerFirst(_ s: String) -> String {
        guard let f = s.first else { return s }
        return String(f).lowercased() + String(s.dropFirst())
    }

    static func ordinal(_ n: Int) -> String {
        switch n {
        case 1: return "first"
        case 2: return "second"
        case 3: return "third"
        case 4: return "fourth"
        case 5: return "fifth"
        case 6: return "sixth"
        case 7: return "seventh"
        case 8: return "eighth"
        default: return "\(n)th"
        }
    }
}
