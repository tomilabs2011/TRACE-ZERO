# Installing TRACE//ZERO on the iPhone

There are two ways. On a jailbroken phone, **B** is the better one.

## Which Xcode

The app targets iOS 12.0 so it runs on the iPhone 5s.

| Xcode | Can build for iOS 12? | Notes |
|---|---|---|
| 15.x, 16.x | yes | |
| 26.x | yes | iOS 12 is below its official floor (15), but it only warns. |
| 27 and later | **no** | The build fails: Xcode 27 supports iOS 15 to 27 only. |

The Mac map builder (MapForge) and `tzforge` are not affected: they build
with any of these, including Xcode 27.

### If you have Xcode 27

Keep Xcode 27 and install Xcode 26 next to it, used only to build the
iPhone app:

1. Download Xcode 26.x (the last 26 release) from
   https://developer.apple.com/download/all/ with your free Apple ID.
2. Unpack it and rename it to `Xcode-26.app` in `/Applications`, so it sits
   beside `Xcode.app` instead of replacing it.
3. Run `Tools/build-ipa.sh` (route B below). It notices that the default
   Xcode is too new and uses `Xcode-26.app` automatically. To choose one
   explicitly, run the command below.

```bash
Tools/build-ipa.sh --xcode /Applications/Xcode-26.app
```

The script only uses Xcode 26's command line build tools, which often
still work when an older Xcode's window refuses to open on a newer macOS
(not verified on macOS 27). If Xcode 26 does not
work at all on your macOS, the fallback is to build on another Mac (or a
hosted macOS build service) that has Xcode 26, and copy the IPA over.

Do not raise the app's deployment target to iOS 15 to please Xcode 27: the
result would not install or run on iOS 12.

Debugging and pressing Run on an iOS 12 device needs an Xcode that still
talks to iOS 12 devices. Route B only needs Xcode to *build*.

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
  and then run the command below (older versions of the tool use
  `ideviceinstaller -i` instead of `install`).

```bash
ideviceinstaller install build/TraceZero.ipa
```

To update, build and install again. Your map packs and settings stay.

## Copying map packs

The app appears on the phone and in Finder as **TRACE ZERO** (with a space).
Its name must not contain `/`: Finder shows each app as a folder named
after it, and a slash in that name makes copies silently go nowhere.

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
