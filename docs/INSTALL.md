# Installing TRACE//ZERO on the iPhone

There are two ways. On a jailbroken phone, **B** is the better one.

## Which Xcode

The app targets iOS 12.0 so it runs on the iPhone 5s.

| Xcode | Can build for iOS 12? | Notes |
|---|---|---|
| 15.x, 16.x | yes | Supported deployment targets go down to iOS 12. |
| 26.x | yes | Officially documents iOS 15 as the floor, but still accepts 12.0. |
| 27 (beta) | no | Reported to reject deployment targets below 15. |

Debugging and pressing Run on an iOS 12 device needs an Xcode that still
talks to iOS 12 devices. If yours does not, use route B: it only needs
Xcode to *build*.

## A. Xcode and an Apple ID (any phone)

1. Install Xcode and select it once:
   `sudo xcode-select -s /Applications/Xcode.app/Contents/Developer`
2. Open `TraceZero.xcodeproj`. Xcode downloads MapLibre (Swift Package
   Manager) on first open.
3. Select the TraceZero target → Signing & Capabilities → choose your team.
   If Xcode complains that the bundle id is taken, change
   `com.tracezero.app` to something of your own (also in
   `Tools/gen_xcodeproj.py` if you regenerate the project).
4. Connect the phone, trust the Mac, select the phone, press Run.

With a free Apple ID the signature expires after 7 days; run again from
Xcode to renew it. CarPlay does not appear with this route (the CarPlay
entitlement cannot be signed with a normal Apple ID).

## B. Jailbroken phone (recommended for your 5s)

The app is built without a certificate and fake-signed with `ldid`. A
jailbroken phone with **AppSync Unified** accepts it, the install never
expires, and the CarPlay entitlement can be included.

On the Mac:

```bash
brew install ldid
```

```bash
Tools/build-ipa.sh
```

(`Tools/build-ipa.sh --no-carplay` leaves the CarPlay entitlement out.)

The result is `build/TraceZero.ipa`. Install it any way you like:

- copy it to the phone and open it with Filza (needs AppSync Unified), or
- with libimobiledevice: `brew install libimobiledevice ideviceinstaller`
  and then run the command below.

```bash
ideviceinstaller -i build/TraceZero.ipa
```

To update, build and install again. Your map packs and settings stay.

## Copying map packs

1. Build a pack: see [MapBuilder/README.md](../MapBuilder/README.md).
2. Connect the phone, open Finder, select the iPhone, open the **Files** tab.
3. Drag the `.vmap` folder onto **TRACE//ZERO**.

The app picks it up immediately (it watches its Documents folder). Packs
are excluded from iCloud and computer backups, because they can be large
and can always be copied again. Delete packs in the app (Menu → Map packs,
swipe left) or in Finder.

Rough sizes for the whole pack (map + routing + search): a city is
50–200 MB, Hungary is about 1 GB, Germany is several GB. The 32 GB 5s has
room for several countries.
