import CoreLocation
import Foundation

/// Finds the map packs in the app's Documents folder (where Finder's
/// "Files" tab puts them) and keeps the list up to date while the app runs.
/// No import step: dropping `hungary.vmap` in Finder is enough.
final class PackStore {
    static let shared = PackStore()
    static let didChange = Notification.Name("TraceZeroPacksDidChange")

    struct Problem {
        let folder: String
        let message: String
    }

    private(set) var packs: [MapPack] = []
    private(set) var problems: [Problem] = []

    private var watcher: DispatchSourceFileSystemObject?
    private var pendingScan: DispatchWorkItem?

    let documents: URL = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]

    private init() {}

    /// Rescans Documents. Cheap: only manifests are read.
    func reload() {
        let fm = FileManager.default
        let items = (try? fm.contentsOfDirectory(at: documents, includingPropertiesForKeys: [.isDirectoryKey],
                                                 options: [.skipsHiddenFiles])) ?? []
        var found: [MapPack] = []
        var bad: [Problem] = []
        // Keep already loaded packs (and their open files) when nothing changed.
        let existing = Dictionary(packs.map { ($0.url.standardizedFileURL.path, $0) }, uniquingKeysWith: { a, _ in a })
        for item in items.sorted(by: { $0.lastPathComponent < $1.lastPathComponent }) {
            var isDir: ObjCBool = false
            guard fm.fileExists(atPath: item.path, isDirectory: &isDir), isDir.boolValue else { continue }
            let hasManifest = fm.fileExists(atPath: item.appendingPathComponent("manifest.json").path)
            guard item.pathExtension == "vmap" || hasManifest else { continue }
            excludeFromBackup(item)
            if let old = existing[item.standardizedFileURL.path], manifestUnchanged(old) {
                found.append(old)
                continue
            }
            do {
                found.append(try MapPack(url: item))
            } catch {
                bad.append(Problem(folder: item.lastPathComponent, message: "\(error)"))
            }
        }
        packs = found
        problems = bad
        NotificationCenter.default.post(name: PackStore.didChange, object: self)
    }

    private func manifestUnchanged(_ pack: MapPack) -> Bool {
        let murl = pack.url.appendingPathComponent("manifest.json")
        guard let data = try? Data(contentsOf: murl),
              let m = try? JSONDecoder().decode(PackManifest.self, from: data) else { return false }
        return m.created == pack.manifest.created && m.id == pack.manifest.id
    }

    /// Map packs can be gigabytes; never let them fill the phone's iCloud or
    /// computer backups. They can always be copied again from the Mac.
    private func excludeFromBackup(_ url: URL) {
        var u = url
        var values = URLResourceValues()
        values.isExcludedFromBackup = true
        try? u.setResourceValues(values)
    }

    /// Watch Documents for packs added or removed through Finder.
    func startWatching() {
        guard watcher == nil else { return }
        let fd = open(documents.path, O_EVTONLY)
        guard fd >= 0 else { return }
        let src = DispatchSource.makeFileSystemObjectSource(fileDescriptor: fd, eventMask: [.write, .rename, .delete],
                                                            queue: .main)
        src.setEventHandler { [weak self] in self?.scheduleReload() }
        src.setCancelHandler { close(fd) }
        src.resume()
        watcher = src
    }

    private func scheduleReload() {
        // A Finder copy produces many events; wait until it settles.
        pendingScan?.cancel()
        let w = DispatchWorkItem { [weak self] in self?.reload() }
        pendingScan = w
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.5, execute: w)
    }

    func delete(_ pack: MapPack) throws {
        try FileManager.default.removeItem(at: pack.url)
        reload()
    }

    // MARK: lookup

    /// Packs whose area covers the coordinate.
    func packs(containing c: CLLocationCoordinate2D) -> [MapPack] {
        return packs.filter { $0.contains(c) }
    }

    /// The pack to route in: one that covers both points. Among several,
    /// the smallest area is preferred (a city pack over a country pack).
    func pack(for a: CLLocationCoordinate2D, and b: CLLocationCoordinate2D) -> MapPack? {
        return packs.filter { $0.contains(a) && $0.contains(b) }
            .min { ($0.maxLat - $0.minLat) * ($0.maxLon - $0.minLon) < ($1.maxLat - $1.minLat) * ($1.maxLon - $1.minLon) }
    }
}
