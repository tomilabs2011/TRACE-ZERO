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

        Theme.apply(Settings.shared.renderMode.look)
        let w = UIWindow(frame: UIScreen.main.bounds)
        w.backgroundColor = Theme.background
        w.tintColor = Theme.fg
        w.rootViewController = MainViewController()
        w.makeKeyAndVisible()
        window = w
        NotificationCenter.default.addObserver(self, selector: #selector(settingsChanged),
                                               name: Settings.didChange, object: nil)
        return true
    }

    @objc private func settingsChanged() {
        guard Settings.shared.renderMode.look != Theme.look else { return }
        // Not inside the notification: the old screen is still handling it.
        DispatchQueue.main.async { self.applyLook() }
    }

    /// Rebuilds the interface in the look of the current map style (WALKER
    /// moves the whole app into its own colours and font). Navigation and
    /// everything else that matters lives outside the screen (Navigator,
    /// Settings, PackStore), so a fresh screen picks it up; an open place
    /// card or route preview is closed. Settings stays open if it was.
    private func applyLook() {
        let look = Settings.shared.renderMode.look
        guard look != Theme.look, let w = window else { return }
        Theme.apply(look)
        var reopenSettings = false
        if let presented = w.rootViewController?.presentedViewController {
            reopenSettings = (presented as? UINavigationController)?.viewControllers.first is SettingsViewController
            presented.dismiss(animated: false)
        }
        let root = MainViewController()
        w.backgroundColor = Theme.background
        w.tintColor = Theme.fg
        UIView.transition(with: w, duration: 0.3, options: .transitionCrossDissolve, animations: {
            w.rootViewController = root
        }, completion: { _ in
            if reopenSettings {
                root.present(UINavigationController.themed(root: SettingsViewController()), animated: false)
            }
        })
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
