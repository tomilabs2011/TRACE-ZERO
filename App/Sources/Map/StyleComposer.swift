import Foundation

/// Turns a style template from Resources/Styles into a style for the packs
/// currently on screen:
///   * "{GLYPHS}" -> the bundled glyph folder
///   * the "omt" source -> one `pmtiles://file://...` source per pack, with
///     every base-map layer repeated for each extra pack (ids "<id>@1", ...)
/// Navigation layers (route, destination, dim veil) are left unchanged.
enum StyleComposer {
    /// Base-map layer ids get this suffix for the 2nd, 3rd... pack.
    static func layerID(_ base: String, pack index: Int) -> String {
        return index == 0 ? base : "\(base)@\(index)"
    }

    static func compose(mode: RenderMode, packs: [MapPack]) throws -> URL {
        guard let template = Bundle.main.url(forResource: mode.rawValue, withExtension: "json",
                                             subdirectory: "Styles") else {
            throw PackError.core("Style \(mode.rawValue) is missing from the app bundle")
        }
        let data = try Data(contentsOf: template)
        guard var style = try JSONSerialization.jsonObject(with: data) as? [String: Any],
              var sources = style["sources"] as? [String: Any],
              let layers = style["layers"] as? [[String: Any]] else {
            throw PackError.core("Style \(mode.rawValue) is damaged")
        }
        let glyphs = Bundle.main.resourceURL!.appendingPathComponent("Glyphs", isDirectory: true).absoluteString
        let glyphBase = glyphs.hasSuffix("/") ? String(glyphs.dropLast()) : glyphs
        style["glyphs"] = "\(glyphBase)/{fontstack}/{range}.pbf"

        let withTiles = packs.filter { $0.tilesURL != nil }
        let omt = sources.removeValue(forKey: "omt") as? [String: Any] ?? [:]
        for (i, pack) in withTiles.enumerated() {
            var src = omt
            src["url"] = "pmtiles://" + pack.tilesURL!.absoluteString
            sources[sourceID(i)] = src
        }
        style["sources"] = sources

        var out: [[String: Any]] = []
        for layer in layers {
            guard (layer["source"] as? String) == "omt" else {
                out.append(layer)
                continue
            }
            for i in 0..<withTiles.count {
                var copy = layer
                copy["id"] = layerID(layer["id"] as? String ?? "layer", pack: i)
                copy["source"] = sourceID(i)
                out.append(copy)
            }
        }
        style["layers"] = out

        let json = try JSONSerialization.data(withJSONObject: style, options: [])
        let dir = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("styles", isDirectory: true)
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let key = ([mode.rawValue] + withTiles.map { "\($0.id)-\($0.manifest.created ?? "")" }).joined(separator: "|")
        let url = dir.appendingPathComponent("style-\(stableHash(key)).json")
        try json.write(to: url, options: .atomic)
        return url
    }

    private static func sourceID(_ i: Int) -> String {
        return i == 0 ? "omt" : "omt@\(i)"
    }

    /// FNV-1a, stable across launches (Swift's hashValue is randomised).
    private static func stableHash(_ s: String) -> String {
        var h: UInt64 = 0xcbf29ce484222325
        for b in s.utf8 {
            h ^= UInt64(b)
            h = h &* 0x100000001b3
        }
        return String(h, radix: 16)
    }
}
