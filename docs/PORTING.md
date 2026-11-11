# Moving to a current iPhone

The app is written for iOS 12 on the iPhone 5s, but nothing in it is
specific to that phone. It already runs unchanged on newer iPhones and
iOS versions (built with Xcode 16 or 26). This page lists what to change,
and in which file, when you want to drop iOS 12 and use newer features.

## What carries over unchanged

- **Everything on the Mac side**: `tzforge`, `forge.sh`, the Mac app, the
  pack format. Packs built today work on any iPhone.
- **`Core/` (C)**: plain C99 and POSIX. It compiles for any Apple
  platform, Android (NDK) or Linux. Nothing there knows about iOS.
- **`App/Sources/Core` and `App/Sources/Navigation`**: Foundation,
  CoreLocation and AVFoundation only. A SwiftUI app can reuse them as they
  are.

## Step by step

### 1. Raise the deployment target

In `Tools/gen_xcodeproj.py` set `DEPLOYMENT_TARGET = "15.0"` (or newer),
then run `python3 Tools/gen_xcodeproj.py`. Xcode 27 and later require at
least 15.0.

### 2. Upgrade MapLibre

Set `MAPLIBRE_VERSION` in `Tools/gen_xcodeproj.py` to the latest 6.x and
regenerate. It is pinned to 6.10.0 only because that version is known to
run on the iPhone 5s (A7 GPU, iOS 12); newer versions add PMTiles fixes
and an ambient tile cache. API changes would be confined to
`App/Sources/Map/MapController.swift` and `MapLocationAdapter.swift`.

### 3. Adopt scenes (needed for future iOS versions)

iOS 12 has no `UIScene`, so the app uses `UIApplicationDelegate` with one
`UIWindow` (`App/Sources/App/AppDelegate.swift`). Apple has announced that
apps built with future SDKs must use the scene lifecycle. To switch:

- add `UIApplicationSceneManifest` to `App/Info.plist` with a window scene
  delegate that creates the `UIWindow` with `MainViewController`;
- for CarPlay add a `CPTemplateApplicationScene` configuration and move the
  body of `CarPlayController.connect` / `disconnect` into a
  `CPTemplateApplicationSceneDelegate`
  (`templateApplicationScene(_:didConnect:to:)`); the controller itself does
  not change;
- the `CPApplicationDelegate` extension can then be deleted.

### 4. Replace drawn icons with SF Symbols (optional)

`App/Sources/UI/Icons.swift` draws every icon because SF Symbols need
iOS 13. With iOS 13+ you can switch `IconButton` to
`UIImage(systemName:)`; `ManeuverGlyph` is still useful for turn arrows.

### 5. Things that become simpler with newer iOS (optional)

| Feature | iOS | Where |
|---|---|---|
| Dark mode colours, `UIColor` dynamic providers | 13 | `UI/Theme.swift` (the app is always dark anyway) |
| `UIFont.monospacedDigitSystemFont` everywhere / SF Mono | 13 | `UI/Theme.swift` |
| `CLLocationManager` instance `authorizationStatus`, `locationManagerDidChangeAuthorization` | 14 | `Navigation/LocationService.swift` |
| CarPlay `CPManeuver.symbolImage`, dashboard and instrument cluster scenes | 13–15 | `CarPlay/CarPlayController.swift` |
| Live Activities for the next turn on the lock screen | 16 | new |
| SwiftUI for Settings and Packs screens | 13+ | `UI/SettingsViewController.swift`, `UI/PacksViewController.swift` |

### 6. Larger screens

Layout is computed in `MainViewController.viewDidLayoutSubviews`. It
already uses safe-area insets (notch, home indicator) and switches to a
left-column layout in landscape. On big phones you may want larger panels
and fonts there.

## A complete rewrite in another stack

If you ever rewrite the UI (SwiftUI, or another platform), keep:

1. `Core/` as the engine (wrap the functions in `Core/tz_core.h`);
2. the pack format and the Mac tools as they are;
3. `Tools/make_styles.py`: the styles are standard MapLibre style JSON,
   which MapLibre GL JS, MapLibre Android and MapLibre iOS all read;
4. the logic of `NavigationSession`, `VoiceGuide` and `Instructions`,
   which is small and has no UI in it.
