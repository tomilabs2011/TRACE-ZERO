import CoreLocation
import UIKit

/// Full-screen offline search. Results update while typing (debounced);
/// the query runs on a background queue and never blocks typing.
final class SearchViewController: UIViewController, UITableViewDataSource, UITableViewDelegate, UITextFieldDelegate {
    var near: CLLocationCoordinate2D?
    var onSelect: ((SearchResult) -> Void)?

    private let field = UITextField()
    private let cancel = UIButton(type: .system)
    private let table = UITableView(frame: .zero, style: .plain)
    private let info = UILabel()
    private var results: [SearchResult] = []
    private var pending: DispatchWorkItem?

    override var preferredStatusBarStyle: UIStatusBarStyle { return Theme.screenStatusBar }

    init() {
        super.init(nibName: nil, bundle: nil)
        modalPresentationStyle = .fullScreen
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is not used") }

    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = Theme.background

        Theme.stylePanel(field)
        field.textColor = Theme.fg
        field.tintColor = Theme.fg
        field.font = Theme.font(17, weight: .demi)
        field.attributedPlaceholder = Theme.tracked("Address, place, street, POI, lat/lon", size: 14,
                                                    color: Theme.fgDim, spacing: 0.5)
        field.keyboardAppearance = Theme.keyboard
        field.autocorrectionType = .no
        field.autocapitalizationType = .none
        field.returnKeyType = .search
        field.clearButtonMode = .whileEditing
        field.leftView = UIView(frame: CGRect(x: 0, y: 0, width: 12, height: 10))
        field.leftViewMode = .always
        field.delegate = self
        field.addTarget(self, action: #selector(textChanged), for: .editingChanged)

        cancel.setAttributedTitle(Theme.tracked("Close", size: 14, weight: .bold), for: .normal)
        cancel.addTarget(self, action: #selector(close), for: .touchUpInside)

        table.backgroundColor = Theme.background
        table.separatorColor = Theme.fgFaint
        table.indicatorStyle = Theme.scrollIndicator
        table.keyboardDismissMode = .onDrag
        table.dataSource = self
        table.delegate = self
        table.rowHeight = 58
        table.tableFooterView = UIView()

        info.numberOfLines = 0
        info.textAlignment = .center
        info.font = Theme.font(14, weight: .demi)
        info.textColor = Theme.fgDim
        info.text = PackStore.shared.packs.isEmpty
            ? "No map packs installed."
            : "Searching \(PackStore.shared.packs.map { $0.name }.joined(separator: ", ")) offline."

        for v in [field, cancel, table, info] as [UIView] { view.addSubview(v) }
    }

    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        field.becomeFirstResponder()
    }

    override func viewDidLayoutSubviews() {
        super.viewDidLayoutSubviews()
        let s = view.safeAreaInsets
        let top = max(s.top, 20) + 8
        cancel.frame = CGRect(x: view.bounds.width - s.right - 8 - 64, y: top, width: 64, height: 44)
        field.frame = CGRect(x: s.left + 8, y: top, width: cancel.frame.minX - s.left - 16, height: 44)
        table.frame = CGRect(x: 0, y: field.frame.maxY + 8, width: view.bounds.width,
                             height: view.bounds.height - field.frame.maxY - 8)
        info.frame = CGRect(x: 24, y: field.frame.maxY + 24, width: view.bounds.width - 48, height: 80)
    }

    @objc private func close() {
        field.resignFirstResponder()
        dismiss(animated: true)
    }

    @objc private func textChanged() {
        pending?.cancel()
        let text = field.text ?? ""
        let w = DispatchWorkItem { [weak self] in self?.run(text) }
        pending = w
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.15, execute: w)
    }

    private func run(_ text: String) {
        guard !text.trimmingCharacters(in: .whitespaces).isEmpty else {
            results = []
            table.reloadData()
            info.isHidden = false
            return
        }
        SearchEngine.shared.search(text, near: near) { [weak self] r in
            guard let self = self else { return }
            self.results = r
            self.info.isHidden = !r.isEmpty
            if r.isEmpty { self.info.text = "Nothing found for “\(text)”." }
            self.table.reloadData()
        }
    }

    func textFieldShouldReturn(_ textField: UITextField) -> Bool {
        if let first = results.first {
            choose(first)
        } else {
            textField.resignFirstResponder()
        }
        return true
    }

    private func choose(_ r: SearchResult) {
        field.resignFirstResponder()
        dismiss(animated: true) { [onSelect] in onSelect?(r) }
    }

    // MARK: table

    func tableView(_ tableView: UITableView, numberOfRowsInSection section: Int) -> Int {
        return results.count
    }

    func tableView(_ tableView: UITableView, cellForRowAt indexPath: IndexPath) -> UITableViewCell {
        let cell = tableView.dequeueReusableCell(withIdentifier: "r") as? ResultCell ?? ResultCell()
        cell.show(results[indexPath.row])
        return cell
    }

    func tableView(_ tableView: UITableView, didSelectRowAt indexPath: IndexPath) {
        tableView.deselectRow(at: indexPath, animated: true)
        choose(results[indexPath.row])
    }
}

private final class ResultCell: UITableViewCell {
    private let tag_ = UILabel()
    private let title = UILabel()
    private let subtitle = UILabel()
    private let distance = UILabel()

    init() {
        super.init(style: .default, reuseIdentifier: "r")
        backgroundColor = Theme.background
        let sel = UIView()
        sel.backgroundColor = Theme.fgFaint
        selectedBackgroundView = sel
        tag_.font = Theme.font(10, weight: .bold)
        tag_.textColor = Theme.onFg
        tag_.backgroundColor = Theme.fg
        tag_.textAlignment = .center
        tag_.layer.cornerRadius = 2
        tag_.clipsToBounds = true
        title.font = Theme.font(17, weight: .demi)
        title.textColor = Theme.fg
        subtitle.font = Theme.font(13, weight: .regular)
        subtitle.textColor = Theme.fgDim
        distance.font = Theme.numberFont(13, weight: .demi)
        distance.textColor = Theme.fg
        distance.textAlignment = .right
        for v in [tag_, title, subtitle, distance] { contentView.addSubview(v) }
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is not used") }

    func show(_ r: SearchResult) {
        tag_.text = r.tag
        title.text = r.title
        subtitle.text = r.subtitle
        distance.text = r.distance.map { Format.distance($0) } ?? ""
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        let w = contentView.bounds.width
        tag_.frame = CGRect(x: 12, y: 11, width: 44, height: 15)
        distance.frame = CGRect(x: w - 80, y: 8, width: 68, height: 20)
        title.frame = CGRect(x: 64, y: 6, width: distance.frame.minX - 70, height: 24)
        subtitle.frame = CGRect(x: 64, y: 30, width: w - 76, height: 18)
    }
}
