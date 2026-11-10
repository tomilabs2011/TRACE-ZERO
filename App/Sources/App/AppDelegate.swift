import UIKit

/// App entry point. Uses the classic UIApplicationDelegate + UIWindow
/// lifecycle because UIScene does not exist on iOS 12. CarPlay hooks into
/// this class through CPApplicationDelegate (see CarPlay/CarPlayController.swift).
/// docs/PORTING.md describes the switch to scenes for current iOS versions.
@main
final class AppDelegate: UIResponder, UIApplicationDelegate {
    var window: UIWindow?

    func application(_ application: UIApplication,
                     didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]?) -> Bool {
        UIDevice.current.isBatteryMonitoringEnabled = true   // GPS accuracy depends on charging
        PackStore.shared.reload()
        PackStore.shared.startWatching()

        let w = UIWindow(frame: UIScreen.main.bounds)
        w.backgroundColor = .black
        w.tintColor = Theme.cyan
        w.rootViewController = MainViewController()
        w.makeKeyAndVisible()
        window = w
        return true
    }

    func applicationDidBecomeActive(_ application: UIApplication) {
        // Packs may have been added or removed in Finder while we were away.
        PackStore.shared.reload()
    }

    func applicationDidReceiveMemoryWarning(_ application: UIApplication) {
        // Mapped pack files are clean pages the system can drop by itself;
        // the map view releases its own tile cache. Nothing else is cached.
    }
}
