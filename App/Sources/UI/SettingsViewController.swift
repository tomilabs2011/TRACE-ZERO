import UIKit

/// Preferences. Every row maps to one property of `Settings`.
final class SettingsViewController: UITableViewController {
    private struct Toggle {
        let title: String
        let detail: String
        let get: () -> Bool
        let set: (Bool) -> Void
    }

    private let toggles: [[Toggle]] = [
        [
            Toggle(title: "Voice guidance", detail: "Spoken turn instructions",
                   get: { Settings.shared.voice }, set: { Settings.shared.voice = $0 }),
            Toggle(title: "Automatic reroute", detail: "Recalculate when you leave the route",
                   get: { Settings.shared.autoReroute }, set: { Settings.shared.autoReroute = $0 }),
            Toggle(title: "Keep screen on", detail: "While navigating",
                   get: { Settings.shared.keepAwake }, set: { Settings.shared.keepAwake = $0 }),
            Toggle(title: "Navigate in background", detail: "Off: navigation pauses when the app or screen sleeps",
                   get: { Settings.shared.backgroundNavigation }, set: { Settings.shared.backgroundNavigation = $0 }),
            Toggle(title: "Hide shops while navigating", detail: "Fewer labels around the route",
                   get: { Settings.shared.hideLabelsWhileNavigating },
                   set: { Settings.shared.hideLabelsWhileNavigating = $0 }),
        ],
        [
            Toggle(title: "Tilted view (3D)", detail: "Also on the map: the 3D button",
                   get: { Settings.shared.tilt }, set: { Settings.shared.tilt = $0 }),
            Toggle(title: "Smooth (60 fps)", detail: "Uses more battery; 30 fps is the default",
                   get: { Settings.shared.smoothRendering }, set: { Settings.shared.smoothRendering = $0 }),
            Toggle(title: "Imperial units", detail: "Miles and feet",
                   get: { Settings.shared.imperial }, set: { Settings.shared.imperial = $0 }),
        ],
    ]

    init() {
        super.init(style: .grouped)
        title = "SETTINGS"
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is not used") }

    override func viewDidLoad() {
        super.viewDidLoad()
        tableView.backgroundColor = Theme.background
        tableView.separatorColor = Theme.fgFaint
        tableView.indicatorStyle = Theme.scrollIndicator
        navigationItem.rightBarButtonItem = UIBarButtonItem(barButtonSystemItem: .done, target: self,
                                                            action: #selector(done))
    }

    @objc private func done() { dismiss(animated: true) }

    // sections: 0 render mode, 1 navigation toggles, 2 display toggles, 3 about
    override func numberOfSections(in tableView: UITableView) -> Int { return 4 }

    override func tableView(_ tableView: UITableView, numberOfRowsInSection section: Int) -> Int {
        switch section {
        case 0: return RenderMode.allCases.count
        case 1: return toggles[0].count
        case 2: return toggles[1].count
        default: return 1
        }
    }

    override func tableView(_ tableView: UITableView, titleForHeaderInSection section: Int) -> String? {
        return ["Map style", "Navigation", "Display", "About"][section]
    }

    override func tableView(_ tableView: UITableView, willDisplayHeaderView view: UIView, forSection section: Int) {
        (view as? UITableViewHeaderFooterView)?.textLabel?.textColor = Theme.fg
    }

    override func tableView(_ tableView: UITableView, heightForRowAt indexPath: IndexPath) -> CGFloat {
        // the pixel font of the WALKER look is taller per line
        if indexPath.section == 3 { return Theme.look == .standard ? 120 : 150 }
        return Theme.look == .standard ? 56 : 68
    }

    override func tableView(_ tableView: UITableView, cellForRowAt indexPath: IndexPath) -> UITableViewCell {
        let cell = UITableViewCell(style: .subtitle, reuseIdentifier: nil)
        cell.backgroundColor = Theme.panel
        cell.tintColor = Theme.fg
        cell.textLabel?.textColor = Theme.fg
        cell.textLabel?.font = Theme.font(17, weight: .demi)
        cell.detailTextLabel?.textColor = Theme.fgDim
        cell.detailTextLabel?.font = Theme.font(12, weight: .regular)
        cell.detailTextLabel?.numberOfLines = 0
        let sel = UIView()
        sel.backgroundColor = Theme.fgFaint
        cell.selectedBackgroundView = sel
        switch indexPath.section {
        case 0:
            let m = RenderMode.allCases[indexPath.row]
            cell.textLabel?.text = m.title
            cell.detailTextLabel?.text = m.detail
            cell.accessoryType = m == Settings.shared.renderMode ? .checkmark : .none
        case 1, 2:
            let t = toggles[indexPath.section - 1][indexPath.row]
            cell.textLabel?.text = t.title
            cell.detailTextLabel?.text = t.detail
            let sw = UISwitch()
            sw.onTintColor = Theme.fg
            sw.isOn = t.get()
            sw.tag = indexPath.section * 100 + indexPath.row
            sw.addTarget(self, action: #selector(toggled(_:)), for: .valueChanged)
            cell.accessoryView = sw
            cell.selectionStyle = .none
        default:
            let v = Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "?"
            let b = Bundle.main.infoDictionary?["CFBundleVersion"] as? String ?? "?"
            cell.textLabel?.text = "TRACE//ZERO \(v) (\(b))"
            cell.detailTextLabel?.text = "Offline maps and navigation. Map data © OpenStreetMap contributors, " +
                "available under the Open Database License (openstreetmap.org/copyright). " +
                "Rendering: MapLibre Native. Labels: Noto Sans and VT323 (SIL OFL)."
            cell.selectionStyle = .none
        }
        return cell
    }

    override func tableView(_ tableView: UITableView, didSelectRowAt indexPath: IndexPath) {
        tableView.deselectRow(at: indexPath, animated: true)
        guard indexPath.section == 0 else { return }
        Settings.shared.renderMode = RenderMode.allCases[indexPath.row]
        tableView.reloadSections(IndexSet(integer: 0), with: .none)
    }

    @objc private func toggled(_ sw: UISwitch) {
        let section = sw.tag / 100, row = sw.tag % 100
        guard section >= 1, section <= 2 else { return }
        toggles[section - 1][row].set(sw.isOn)
        Navigator.shared.updateIdleTimer()
    }
}

/// The list of instructions for a route.
final class StepsViewController: UITableViewController {
    private let route: Route
    private let firstIndex: Int

    init(route: Route, from index: Int) {
        self.route = route
        self.firstIndex = max(0, min(index, route.maneuvers.count - 1))
        super.init(style: .plain)
        title = "STEPS"
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is not used") }

    override func viewDidLoad() {
        super.viewDidLoad()
        tableView.backgroundColor = Theme.background
        tableView.separatorColor = Theme.fgFaint
        tableView.indicatorStyle = Theme.scrollIndicator
        tableView.rowHeight = 60
        tableView.tableFooterView = UIView()
        navigationItem.rightBarButtonItem = UIBarButtonItem(barButtonSystemItem: .done, target: self,
                                                            action: #selector(done))
    }

    @objc private func done() { dismiss(animated: true) }

    override func tableView(_ tableView: UITableView, numberOfRowsInSection section: Int) -> Int {
        return route.maneuvers.count - firstIndex
    }

    override func tableView(_ tableView: UITableView, cellForRowAt indexPath: IndexPath) -> UITableViewCell {
        let m = route.maneuvers[firstIndex + indexPath.row]
        let cell = UITableViewCell(style: .subtitle, reuseIdentifier: nil)
        cell.backgroundColor = Theme.background
        cell.selectionStyle = .none
        cell.textLabel?.text = Instructions.sentence(m)
        cell.textLabel?.textColor = Theme.fg
        cell.textLabel?.font = Theme.font(16, weight: .demi)
        cell.textLabel?.numberOfLines = 2
        // distance of the leg that follows this instruction
        let nextDist = firstIndex + indexPath.row + 1 < route.maneuvers.count
            ? route.maneuvers[firstIndex + indexPath.row + 1].distanceFromStart : route.distance
        let leg = nextDist - m.distanceFromStart
        cell.detailTextLabel?.text = m.kind == .arrive ? "" : "then \(Format.distance(leg))"
        cell.detailTextLabel?.textColor = Theme.fgDim
        cell.detailTextLabel?.font = Theme.numberFont(13, weight: .regular)
        cell.imageView?.image = ManeuverGlyph.image(size: CGSize(width: 34, height: 34), angle: m.glyphAngle,
                                                    uturn: m.modifier == 4 && m.kind == .turn,
                                                    roundaboutExit: m.kind == .roundabout ? m.exit : nil,
                                                    arrive: m.kind == .arrive, color: Theme.fg)
        return cell
    }
}
