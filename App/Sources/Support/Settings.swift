import Foundation

/// Map render modes. Each one is a style file in Resources/Styles.
enum RenderMode: String, CaseIterable {
    case outline, hatch, solid, minimal

    var title: String {
        switch self {
        case .outline: return "OUTLINE"
        case .hatch: return "HATCH"
        case .solid: return "SOLID"
        case .minimal: return "MINIMAL"
        }
    }
}

/// Travel profiles, in the order shown in the app. Raw values match tz_profile.
enum TravelMode: Int, CaseIterable {
    case foot = 2, bike = 1, car = 0

    var title: String {
        switch self {
        case .foot: return "WALK"
        case .bike: return "BIKE"
        case .car: return "CAR"
        }
    }

    /// File inside a pack holding this profile's routing graph.
    var graphFile: String {
        switch self {
        case .foot: return "foot.tzg"
        case .bike: return "bike.tzg"
        case .car: return "car.tzg"
        }
    }
}

/// User preferences, stored in UserDefaults. Read on the main thread.
final class Settings {
    static let shared = Settings()
    static let didChange = Notification.Name("TraceZeroSettingsDidChange")

    private let d = UserDefaults.standard

    private init() {
        d.register(defaults: [
            "renderMode": RenderMode.outline.rawValue,
            "travelMode": TravelMode.foot.rawValue,   // walking is the default
            "voice": true,
            "autoReroute": true,
            "keepAwake": true,
            "backgroundNavigation": false,
            "smoothRendering": false,                // 30 fps saves battery on the 5s
            "tilt": false,
            "imperial": false,
            "hideLabelsWhileNavigating": true,
        ])
    }

    private func changed() {
        NotificationCenter.default.post(name: Settings.didChange, object: self)
    }

    var renderMode: RenderMode {
        get { RenderMode(rawValue: d.string(forKey: "renderMode") ?? "") ?? .outline }
        set { d.set(newValue.rawValue, forKey: "renderMode"); changed() }
    }

    var travelMode: TravelMode {
        get { TravelMode(rawValue: d.integer(forKey: "travelMode")) ?? .foot }
        set { d.set(newValue.rawValue, forKey: "travelMode"); changed() }
    }

    /// Spoken turn instructions.
    var voice: Bool {
        get { d.bool(forKey: "voice") }
        set { d.set(newValue, forKey: "voice"); changed() }
    }

    /// Recalculate automatically after leaving the route.
    var autoReroute: Bool {
        get { d.bool(forKey: "autoReroute") }
        set { d.set(newValue, forKey: "autoReroute"); changed() }
    }

    /// Keep the screen on while navigating.
    var keepAwake: Bool {
        get { d.bool(forKey: "keepAwake") }
        set { d.set(newValue, forKey: "keepAwake"); changed() }
    }

    /// Keep navigating (GPS and voice) when the app is in the background.
    /// Off by default: navigation pauses when the app or screen sleeps.
    var backgroundNavigation: Bool {
        get { d.bool(forKey: "backgroundNavigation") }
        set { d.set(newValue, forKey: "backgroundNavigation"); changed() }
    }

    /// 60 fps map rendering instead of 30 fps.
    var smoothRendering: Bool {
        get { d.bool(forKey: "smoothRendering") }
        set { d.set(newValue, forKey: "smoothRendering"); changed() }
    }

    /// Tilted 3D-style camera while navigating.
    var tilt: Bool {
        get { d.bool(forKey: "tilt") }
        set { d.set(newValue, forKey: "tilt"); changed() }
    }

    var imperial: Bool {
        get { d.bool(forKey: "imperial") }
        set { d.set(newValue, forKey: "imperial"); changed() }
    }

    /// Hide shop/café labels and house numbers while navigating.
    var hideLabelsWhileNavigating: Bool {
        get { d.bool(forKey: "hideLabelsWhileNavigating") }
        set { d.set(newValue, forKey: "hideLabelsWhileNavigating"); changed() }
    }

    /// Last map position, restored on launch.
    var lastCamera: (lat: Double, lon: Double, zoom: Double)? {
        get {
            guard d.object(forKey: "camLat") != nil else { return nil }
            return (d.double(forKey: "camLat"), d.double(forKey: "camLon"), d.double(forKey: "camZoom"))
        }
        set {
            if let v = newValue {
                d.set(v.lat, forKey: "camLat")
                d.set(v.lon, forKey: "camLon")
                d.set(v.zoom, forKey: "camZoom")
            } else {
                d.removeObject(forKey: "camLat")
            }
        }
    }
}
