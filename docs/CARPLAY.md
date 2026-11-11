# CarPlay

## What it does

When the phone is connected to a CarPlay car, TRACE//ZERO shows on the car
screen with:

- the same offline map (its own map view; render mode button on the map);
- **Search** (top left), with results from the installed packs;
- a route preview for the selected place, with **Go**;
- turn-by-turn guidance in the car's maneuver panel with distance and
  time to the next turn and the arrival estimate;
- voice guidance through the car speakers.

Navigation is shared with the phone: a route started on the phone appears
in the car and the other way round. While CarPlay is connected, navigation
keeps running even when the phone screen is off.

The code is `App/Sources/CarPlay/CarPlayController.swift`. It uses the
iOS 12 CarPlay API (`CPApplicationDelegate`, `CPMapTemplate`,
`CPNavigationSession`).

## The entitlement

iOS only shows an app in CarPlay if its signature contains the
`com.apple.developer.carplay-maps` entitlement. Apple grants it to
navigation apps on request (developer.apple.com → CarPlay), and a free
Apple ID cannot sign with it. Without it the app works normally on the
phone and simply does not appear in the car.

On a jailbroken phone you can sign the entitlement in yourself:

```bash
Tools/build-ipa.sh
```

This fake-signs the app with `App/TraceZero-CarPlay.entitlements` using
`ldid` (see [INSTALL.md](INSTALL.md)). AppSync Unified installs it, and
CarPlay sees an app with the navigation entitlement.

If you obtain the entitlement from Apple for normal signing, set
`CODE_SIGN_ENTITLEMENTS = App/TraceZero-CarPlay.entitlements` in the
target settings (in `Tools/gen_xcodeproj.py`, then regenerate).

## Status

The CarPlay code follows Apple's iOS 12 API, but it could not be tested
here: that needs a Mac with Xcode and either a CarPlay car or Apple's
CarPlay Simulator (in "Additional Tools for Xcode" on
developer.apple.com), and the app must carry the entitlement. CarPlay was the lowest priority for this version, so it is
kept small and isolated in one file that can be finished or adjusted
without touching the rest of the app.
