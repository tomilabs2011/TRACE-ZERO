import UIKit

/// Installed map packs: size, data date, what each contains. Packs are added
/// with Finder (no import step) and can be deleted here or in Finder.
final class PacksViewController: UITableViewController {
    private var packs: [MapPack] { return PackStore.shared.packs }
    private var problems: [PackStore.Problem] { return PackStore.shared.problems }

    init() {
        super.init(style: .grouped)
        title = "MAP PACKS"
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is not used") }

    override func viewDidLoad() {
        super.viewDidLoad()
        tableView.backgroundColor = Theme.background
        tableView.separatorColor = Theme.fgFaint
        tableView.indicatorStyle = Theme.scrollIndicator
        navigationItem.rightBarButtonItem = UIBarButtonItem(barButtonSystemItem: .done, target: self,
                                                            action: #selector(done))
        NotificationCenter.default.addObserver(self, selector: #selector(reload), name: PackStore.didChange, object: nil)
        PackStore.shared.reload()
    }

    @objc private func done() { dismiss(animated: true) }
    @objc private func reload() { tableView.reloadData() }

    override func numberOfSections(in tableView: UITableView) -> Int { return 3 }

    override func tableView(_ tableView: UITableView, numberOfRowsInSection section: Int) -> Int {
        switch section {
        case 0: return packs.count
        case 1: return problems.count
        default: return 0
        }
    }

    override func tableView(_ tableView: UITableView, titleForHeaderInSection section: Int) -> String? {
        switch section {
        case 0: return packs.isEmpty ? "No packs installed" : "Installed"
        case 1: return problems.isEmpty ? nil : "Cannot be used"
        default: return "How to add maps"
        }
    }

    override func tableView(_ tableView: UITableView, titleForFooterInSection section: Int) -> String? {
        guard section == 2 else { return nil }
        let store = PackStore.shared
        let free = (try? PackStore.shared.documents.resourceValues(forKeys: [.volumeAvailableCapacityKey]))?
            .volumeAvailableCapacity.map { ByteCountFormatter.string(fromByteCount: Int64($0), countStyle: .file) }
        return "1. Build a pack on your Mac with TRACE ZERO MAP FORGE.\n" +
            "2. Connect the iPhone, open Finder → iPhone → Files.\n" +
            "3. Drag the .vmap folder onto TRACE ZERO.\n" +
            "Packs appear here automatically. Swipe left to delete one." +
            (free.map { "\n\nFree space on the phone: \($0)" } ?? "") +
            "\n\nThis app reads: \(store.documents.path)" +
            "\nFound there: " + (store.documentNames.isEmpty ? "nothing" : store.documentNames.joined(separator: ", ")) +
            (store.scanError.map { "\nCannot read it: \($0)" } ?? "")
    }

    override func tableView(_ tableView: UITableView, willDisplayHeaderView view: UIView, forSection section: Int) {
        (view as? UITableViewHeaderFooterView)?.textLabel?.textColor = Theme.fg
    }

    override func tableView(_ tableView: UITableView, willDisplayFooterView view: UIView, forSection section: Int) {
        (view as? UITableViewHeaderFooterView)?.textLabel?.textColor = Theme.fgDim
    }

    override func tableView(_ tableView: UITableView, cellForRowAt indexPath: IndexPath) -> UITableViewCell {
        let cell = UITableViewCell(style: .subtitle, reuseIdentifier: nil)
        cell.backgroundColor = Theme.panel
        cell.textLabel?.textColor = Theme.fg
        cell.textLabel?.font = Theme.font(17, weight: .bold)
        cell.detailTextLabel?.textColor = Theme.fgDim
        cell.detailTextLabel?.font = Theme.font(13, weight: .regular)
        cell.detailTextLabel?.numberOfLines = 0
        cell.selectionStyle = .none
        if indexPath.section == 0 {
            let p = packs[indexPath.row]
            cell.textLabel?.text = p.name.uppercased()
            let size = ByteCountFormatter.string(fromByteCount: p.totalBytes, countStyle: .file)
            let date = String((p.manifest.osm_timestamp ?? p.manifest.created ?? "").prefix(10))
            var parts = [size]
            if !date.isEmpty { parts.append("data \(date)") }
            let modes = TravelMode.allCases.filter { p.has($0) }.map { $0.title.lowercased() }
            parts.append(modes.isEmpty ? "no routing" : modes.joined(separator: "/"))
            if p.tilesURL == nil { parts.append("no map tiles") }
            if p.manifest.files["search"] == nil { parts.append("no search") }
            cell.detailTextLabel?.text = parts.joined(separator: " · ") + "\n\(p.url.lastPathComponent)"
        } else {
            let pr = problems[indexPath.row]
            cell.textLabel?.text = pr.folder
            cell.detailTextLabel?.text = pr.message
        }
        return cell
    }

    override func tableView(_ tableView: UITableView, heightForRowAt indexPath: IndexPath) -> CGFloat {
        return 64
    }

    override func tableView(_ tableView: UITableView, canEditRowAt indexPath: IndexPath) -> Bool {
        return indexPath.section == 0
    }

    override func tableView(_ tableView: UITableView, commit editingStyle: UITableViewCell.EditingStyle,
                            forRowAt indexPath: IndexPath) {
        guard editingStyle == .delete, indexPath.section == 0 else { return }
        let p = packs[indexPath.row]
        let a = UIAlertController(title: "Delete \(p.name)?",
                                  message: "This removes the pack from the phone. You can copy it again from the Mac.",
                                  preferredStyle: .alert)
        a.addAction(UIAlertAction(title: "Cancel", style: .cancel))
        a.addAction(UIAlertAction(title: "Delete", style: .destructive) { [weak self] _ in
            if Navigator.shared.isActive && Navigator.shared.route?.packID == p.id {
                Navigator.shared.stop()
            }
            do {
                try PackStore.shared.delete(p)
            } catch {
                let e = UIAlertController(title: "Could not delete", message: error.localizedDescription,
                                          preferredStyle: .alert)
                e.addAction(UIAlertAction(title: "OK", style: .default))
                self?.present(e, animated: true)
            }
        })
        present(a, animated: true)
    }
}
