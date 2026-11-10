import AVFoundation
import Foundation

/// Spoken guidance with the system voice (works offline on iOS 12).
/// Decides *when* to speak from distance and speed, so the same rules apply
/// on the phone and in CarPlay.
final class VoiceGuide: NSObject, AVSpeechSynthesizerDelegate {
    static let shared = VoiceGuide()

    private let synth = AVSpeechSynthesizer()
    private var spoken: [Int: Set<Stage>] = [:]   // maneuver index -> stages already announced
    private var routeToken = 0
    var muted = false

    private enum Stage { case far, near, now }

    override init() {
        super.init()
        synth.delegate = self
    }

    /// Call when a new route starts (including reroutes).
    func reset() {
        spoken.removeAll()
        routeToken += 1
    }

    func say(_ text: String) {
        guard Settings.shared.voice, !muted else { return }
        let session = AVAudioSession.sharedInstance()
        try? session.setCategory(.playback, mode: .voicePrompt,
                                 options: [.duckOthers, .interruptSpokenAudioAndMixWithOthers])
        try? session.setActive(true)
        let u = AVSpeechUtterance(string: text)
        u.voice = AVSpeechSynthesisVoice(language: "en-GB") ?? AVSpeechSynthesisVoice(language: "en-US")
        u.rate = AVSpeechUtteranceDefaultSpeechRate * 1.02
        synth.speak(u)
    }

    func stop() {
        synth.stopSpeaking(at: .immediate)
    }

    /// Announce upcoming maneuvers based on progress. Call on every GPS fix.
    func update(route: Route, progress: NavProgress, speed: Double) {
        guard progress.nextManeuver < route.maneuvers.count else { return }
        let i = progress.nextManeuver
        let m = route.maneuvers[i]
        let d = progress.distanceToNextManeuver
        let v = max(speed, route.mode == .car ? 8 : (route.mode == .bike ? 4 : 1.3))

        // Distances: "now" ~ 6 s ahead, "near" ~ 20 s, "far" for long stretches.
        let nowDist = max(route.mode == .car ? 60 : 20, v * 6)
        let nearDist = max(route.mode == .car ? 250 : 80, v * 20)
        let farDist: Double = route.mode == .car ? (v > 22 ? 2000 : 1000) : 400

        var done = spoken[i] ?? []
        let next = i + 1 < route.maneuvers.count ? route.maneuvers[i + 1] : nil
        if d <= nowDist {
            if !done.contains(.now) {
                done.formUnion([.now, .near, .far])
                say(Instructions.spoken(m, in: nil, then: next))
            }
        } else if d <= nearDist {
            if !done.contains(.near) {
                done.formUnion([.near, .far])
                say(Instructions.spoken(m, in: roundedDistance(d), then: nil))
            }
        } else if d <= farDist && d > nearDist * 1.8 {
            if !done.contains(.far) {
                done.insert(.far)
                say(Instructions.spoken(m, in: roundedDistance(d), then: nil))
            }
        }
        spoken[i] = done
    }

    private func roundedDistance(_ d: Double) -> Double {
        if d < 100 { return (d / 10).rounded() * 10 }
        if d < 1000 { return (d / 50).rounded() * 50 }
        return (d / 100).rounded() * 100
    }

    // Give other apps their audio back after speaking.
    func speechSynthesizer(_ synthesizer: AVSpeechSynthesizer, didFinish utterance: AVSpeechUtterance) {
        if !synthesizer.isSpeaking {
            try? AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation)
        }
    }
}
