import Foundation

/// Restore images, downloaded in 64 MB pieces on a background URLSession.
///
/// Apple's CDN is slow on one connection (2–4 MB/s for macOS 27.2), and a 25 GB file takes hours. So every piece is
/// queued at once and iOS fetches 12 side by side (measured on a home connection: 6 gave 27 MB/s, 12 gave 38, 24 gave
/// 34); a piece stuck on a slow server is asked for again on a new connection; the transfers go on while Henge is in the
/// background, and after iOS closes it (but not after a swipe away in the app switcher, which cancels them). A piece
/// that finishes is kept, so pausing, a dropped connection or Henge closing only loses the pieces still on their way.
///
/// On disk, beside the finished images: `<build>.download/` holds `job.json`, `image` (the pieces so far, in order)
/// and pieces that came in ahead of their turn (`<n>.piece`). The finished image becomes `<build>.ipsw`.
final class ImageDownloads: NSObject, ObservableObject {
    static let shared = ImageDownloads()

    struct Status: Equatable {
        var received: Int64
        var total: Int64
        var paused: Bool
        var bytesPerSecond: Double
        /// Why it stopped by itself, if it did.
        var problem: String?

        var fraction: Double { total > 0 ? min(Double(received) / Double(total), 1) : 0 }

        /// "8.1 GB of 25.6 GB · 5.2 MB/s · about 1 hr 10 min left"
        var summary: String {
            var parts = ["\(ByteText.string(received)) of \(ByteText.string(total))"]
            if paused {
                parts.append("paused")
            } else if bytesPerSecond > 0 {
                parts.append("\(ByteText.string(Int64(bytesPerSecond)))/s")
                let left = Double(total - received) / bytesPerSecond
                let f = DateComponentsFormatter()
                f.unitsStyle = .short
                f.allowedUnits = left >= 3600 ? [.hour, .minute] : [.minute]
                if let text = f.string(from: max(left, 60)) { parts.append("about \(text) left") }
            }
            return parts.joined(separator: " · ")
        }
    }

    /// Downloads not finished yet, by build. Main thread.
    @Published private(set) var status: [String: Status] = [:]
    /// Given by the app delegate when iOS opens Henge in the background for finished transfers; called once they are handled.
    var backgroundEventsDone: (() -> Void)?

    private static let piece: Int64 = 64 << 20
    private static let connections = 12
    /// A piece going slower than this, and much slower than the others, gets a new connection.
    private static let slowAge: TimeInterval = 45, slowBytesPerSecond = 256.0 * 1024, slowShare = 0.25

    private struct Job: Codable {
        let id: String
        let url: URL
        let total: Int64
        var paused: Bool
        var problem: String?
    }

    private struct Running {
        let task: URLSessionTask
        var bytes: Int64
        /// When bytes began to come in (a piece can wait its turn for a long time), and how many it had then.
        var started: Date?
        var startBytes: Int64 = 0

        func rate(_ now: Date) -> Double? {
            guard let started, now.timeIntervalSince(started) > 15 else { return nil }
            return Double(bytes - startBytes) / now.timeIntervalSince(started)
        }
    }

    // Everything below belongs to `queue`, which is also the session's delegate queue.
    private let queue: OperationQueue = {
        let q = OperationQueue()
        q.maxConcurrentOperationCount = 1
        q.name = "Henge restore image downloads"
        return q
    }()
    private var session: URLSession?
    private var jobs: [String: Job] = [:]
    private var assembled: [String: Int] = [:]         // pieces in `image`, from the first
    private var waiting: [String: Set<Int>] = [:]      // pieces on disk that came in ahead of their turn
    private var running: [String: [Int: Running]] = [:]
    private var badAnswers: [String: Int] = [:]        // in a row
    private var samples: [String: [(Date, Int64)]] = [:]
    private var lastPublished = Date.distantPast

    // MARK: Asking (any thread)

    /// At launch, and when iOS opens Henge for its transfers: picks up the downloads where they are.
    func connect() {
        queue.addOperation { [self] in
            guard session == nil else { return }
            let cfg = URLSessionConfiguration.background(withIdentifier: (Bundle.main.bundleIdentifier ?? "Henge") + ".restore-images")
            cfg.isDiscretionary = false
            cfg.sessionSendsLaunchEvents = true
            cfg.httpMaximumConnectionsPerHost = Self.connections
            cfg.timeoutIntervalForRequest = 120
            cfg.timeoutIntervalForResource = 7 * 24 * 3600
            let s = URLSession(configuration: cfg, delegate: self, delegateQueue: queue)
            session = s
            loadJobs()
            s.getAllTasks { tasks in self.queue.addOperation { self.reconcile(tasks) } }
            checkSlowLater()
        }
    }

    /// Starts the download, or carries on with one already begun, and returns when the image is whole.
    /// A pause waits; deleting the download, or a problem that stops it, throws.
    @MainActor func download(_ r: MacOSRelease) async throws {
        if Installer.haveImage(r) { return }
        if status[r.id] == nil { try await begin(r) } else { resume(r.id) }
        while true {
            try await Task.sleep(nanoseconds: 1_000_000_000)
            if Installer.haveImage(r) { return }
            guard let s = status[r.id] else { throw Self.error("The download of \(r.name) was deleted.") }
            if let problem = s.problem { throw Self.error(problem) }
        }
    }

    /// Starts a new download (it asks Apple's server for the exact size first).
    @MainActor func begin(_ r: MacOSRelease) async throws {
        guard status[r.id] == nil, !Installer.haveImage(r) else { return }
        var request = URLRequest(url: r.url)
        request.httpMethod = "HEAD"
        let (_, response) = try await URLSession.shared.data(for: request)
        let http = response as? HTTPURLResponse
        guard let http, http.statusCode == 200, http.expectedContentLength > 0 else {
            throw Self.error("Apple's server didn't offer \(r.name) (it answered \(http?.statusCode ?? 0)). Try again later.")
        }
        let total = http.expectedContentLength
        let free = (try? URL(fileURLWithPath: NSHomeDirectory()).resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey]))?
            .volumeAvailableCapacityForImportantUsage ?? 0
        if free > 0 && free < total + (1 << 30) {
            throw Self.error("\(r.name) is \(ByteText.string(total)); this \(Device.name) has \(ByteText.string(free)) free.")
        }
        status[r.id] = Status(received: 0, total: total, paused: false, bytesPerSecond: 0) // shows at once
        HengeLog.log("download", "\(r.name): \(ByteText.string(total)) from \(r.url.absoluteString)")
        queue.addOperation { [self] in
            if jobs[r.id] != nil { // already begun (Henge had not shown it yet): carry on with it
                resume(r.id)
                return publish()
            }
            let job = Job(id: r.id, url: r.url, total: total, paused: false)
            do {
                try FileManager.default.createDirectory(at: folder(r.id), withIntermediateDirectories: true)
                Machines.excludeFromBackup(Installer.imageDirectory)
                FileManager.default.createFile(atPath: imageFile(r.id).path, contents: nil)
                jobs[r.id] = job
                save(job)
            } catch {
                HengeLog.log("download", "cannot make \(folder(r.id).path): \(error.localizedDescription)")
                return publish()
            }
            assembled[r.id] = 0
            schedule(r.id)
            publish()
        }
    }

    func pause(_ id: String) {
        queue.addOperation { [self] in
            guard var job = jobs[id], !job.paused else { return }
            job.paused = true
            jobs[id] = job
            save(job)
            cancelRunning(id)
            HengeLog.log("download", "\(id): paused")
            publish()
        }
    }

    func resume(_ id: String) {
        queue.addOperation { [self] in
            guard var job = jobs[id], job.paused else { return }
            job.paused = false
            job.problem = nil
            jobs[id] = job
            badAnswers[id] = 0
            save(job)
            HengeLog.log("download", "\(id): resumed")
            schedule(id)
            publish()
        }
    }

    /// Stops it and deletes what it has so far.
    func delete(_ id: String) {
        queue.addOperation { [self] in
            cancelRunning(id)
            forget(id)
            try? FileManager.default.removeItem(at: folder(id))
            HengeLog.log("download", "\(id): deleted")
            publish()
        }
    }

    // MARK: Pieces

    private func folder(_ id: String) -> URL { Installer.imageDirectory.appendingPathComponent("\(id).download", isDirectory: true) }
    private func imageFile(_ id: String) -> URL { folder(id).appendingPathComponent("image") }
    private func pieceFile(_ id: String, _ n: Int) -> URL { folder(id).appendingPathComponent("\(n).piece") }
    private func jobFile(_ id: String) -> URL { folder(id).appendingPathComponent("job.json") }

    private func pieceCount(_ job: Job) -> Int { Int((job.total + Self.piece - 1) / Self.piece) }
    private func pieceLength(_ job: Job, _ n: Int) -> Int64 { min(Self.piece, job.total - Int64(n) * Self.piece) }

    private static func fileSize(_ url: URL) -> Int64 {
        ((try? FileManager.default.attributesOfItem(atPath: url.path))?[.size] as? NSNumber)?.int64Value ?? -1
    }

    /// "27.2#12": the download and the piece a task is for.
    private static func piece(of task: URLSessionTask) -> (String, Int)? {
        guard let parts = task.taskDescription?.split(separator: "#"), parts.count == 2, let n = Int(parts[1]) else { return nil }
        return (String(parts[0]), n)
    }

    private func save(_ job: Job) {
        if let data = try? JSONEncoder().encode(job) { try? data.write(to: jobFile(job.id), options: .atomic) }
    }

    private func forget(_ id: String) {
        jobs[id] = nil
        assembled[id] = nil
        waiting[id] = nil
        running[id] = nil
        badAnswers[id] = nil
        samples[id] = nil
    }

    private func loadJobs() {
        let fm = FileManager.default
        for name in (try? fm.contentsOfDirectory(atPath: Installer.imageDirectory.path)) ?? [] where name.hasSuffix(".download") {
            let id = String(name.dropLast(".download".count))
            guard let data = try? Data(contentsOf: jobFile(id)), let job = try? JSONDecoder().decode(Job.self, from: data) else {
                try? fm.removeItem(at: folder(id))
                continue
            }
            jobs[id] = job
            tidy(id)
        }
    }

    /// After a launch: `image` back to a whole number of pieces (an append Henge closed in the middle of is undone),
    /// pieces already in it dropped, and the rest noted and added.
    private func tidy(_ id: String) {
        guard let job = jobs[id] else { return }
        let fm = FileManager.default
        let image = imageFile(id)
        if !fm.fileExists(atPath: image.path) { fm.createFile(atPath: image.path, contents: nil) }
        let size = max(Self.fileSize(image), 0)
        var whole = Int(size / Self.piece)
        if size == job.total {
            whole = pieceCount(job)
        } else if Int64(whole) * Self.piece != size, let h = try? FileHandle(forWritingTo: image) {
            try? h.truncate(atOffset: UInt64(whole) * UInt64(Self.piece))
            try? h.close()
        }
        assembled[id] = whole
        var ahead = Set<Int>()
        for name in (try? fm.contentsOfDirectory(atPath: folder(id).path)) ?? [] where name.hasSuffix(".piece") {
            guard let n = Int(name.dropLast(".piece".count)) else { continue }
            if n < whole || Self.fileSize(pieceFile(id, n)) != pieceLength(job, n) {
                try? fm.removeItem(at: pieceFile(id, n))
            } else {
                ahead.insert(n)
            }
        }
        waiting[id] = ahead
        HengeLog.log("download", "\(id): \(whole) of \(pieceCount(job)) pieces in, \(ahead.count) ahead\(job.paused ? ", paused" : "")")
        appendWaiting(id)
    }

    /// The transfers iOS still has, against the downloads on disk: strays cancelled, missing pieces asked for again.
    private func reconcile(_ tasks: [URLSessionTask]) {
        for task in tasks where task.state == .running || task.state == .suspended {
            guard case let (id, n)? = Self.piece(of: task), let job = jobs[id], !job.paused, n >= assembled[id] ?? 0,
                  waiting[id]?.contains(n) != true, running[id]?[n] == nil else {
                task.cancel()
                continue
            }
            running[id, default: [:]][n] = Running(task: task, bytes: task.countOfBytesReceived)
        }
        for id in jobs.keys { schedule(id) }
        publish()
    }

    /// Asks for every piece not here and not on its way. All at once: iOS runs a few at a time, and keeps going with
    /// the rest while Henge is in the background, which it would not do for transfers started from there.
    private func schedule(_ id: String) {
        guard let job = jobs[id], !job.paused, let session else { return }
        for n in (assembled[id] ?? 0)..<pieceCount(job) where waiting[id]?.contains(n) != true && running[id]?[n] == nil {
            start(job, n, session)
        }
    }

    private func start(_ job: Job, _ n: Int, _ session: URLSession) {
        var request = URLRequest(url: job.url)
        let first = Int64(n) * Self.piece
        request.setValue("bytes=\(first)-\(first + pieceLength(job, n) - 1)", forHTTPHeaderField: "Range")
        let task = session.downloadTask(with: request)
        task.taskDescription = "\(job.id)#\(n)"
        running[job.id, default: [:]][n] = Running(task: task, bytes: 0)
        task.resume()
    }

    private func cancelRunning(_ id: String) {
        for (_, r) in running[id] ?? [:] { r.task.cancel() }
        running[id] = nil
        samples[id] = nil
    }

    /// Adds the pieces that are next in line to `image`; when the last is in, it becomes the restore image.
    private func appendWaiting(_ id: String) {
        guard let job = jobs[id] else { return }
        var next = assembled[id] ?? 0
        if waiting[id]?.contains(next) == true {
            do {
                let out = try FileHandle(forWritingTo: imageFile(id))
                defer { try? out.close() }
                try out.seek(toOffset: UInt64(next) * UInt64(Self.piece))
                while waiting[id]?.contains(next) == true {
                    let input = try FileHandle(forReadingFrom: pieceFile(id, next))
                    // Each read is an autoreleased 8 MB buffer, freed only when the pool drains. Without a pool per
                    // read, the pieces iOS fetched while Henge was closed (hundreds, at 64 MB each) all stayed in
                    // memory until the loop ended, and iOS killed Henge (and the Mac booting beside it) at about 9 GB.
                    while try autoreleasepool(invoking: {
                        guard let data = try input.read(upToCount: 8 << 20), !data.isEmpty else { return false }
                        try out.write(contentsOf: data)
                        return true
                    }) {}
                    try input.close()
                    try out.synchronize() // before the piece goes: a piece is always in one place or the other
                    try? FileManager.default.removeItem(at: pieceFile(id, next))
                    waiting[id]?.remove(next)
                    next += 1
                    assembled[id] = next
                }
            } catch {
                return stop(id, "Henge couldn't save the download (\(error.localizedDescription)). Free some space, then tap Resume.")
            }
        }
        if next >= pieceCount(job) { finish(job) }
    }

    private func finish(_ job: Job) {
        let fm = FileManager.default
        let size = Self.fileSize(imageFile(job.id))
        guard size == job.total else {
            return stop(job.id, "The download came out the wrong size (\(size) bytes, not \(job.total)). Delete it and download again.")
        }
        let dest = Installer.imageDirectory.appendingPathComponent("\(job.id).ipsw")
        do {
            try? fm.removeItem(at: dest)
            try fm.moveItem(at: imageFile(job.id), to: dest)
        } catch {
            return stop(job.id, "Henge couldn't keep the download (\(error.localizedDescription)).")
        }
        try? fm.removeItem(at: folder(job.id))
        forget(job.id)
        HengeLog.log("download", "\(job.id): finished, \(ByteText.string(size))")
        publish()
    }

    /// Stops by itself, with a reason; Resume carries on.
    private func stop(_ id: String, _ problem: String) {
        guard var job = jobs[id] else { return }
        HengeLog.log("download", "\(id): stopped: \(problem)")
        job.paused = true
        job.problem = problem
        jobs[id] = job
        save(job)
        cancelRunning(id)
        publish()
    }

    /// Every 15 seconds while Henge runs (iOS doesn't run it in the background, where this isn't needed as much).
    private func checkSlowLater() {
        DispatchQueue.global().asyncAfter(deadline: .now() + 15) { [self] in
            queue.addOperation { [self] in
                restartSlowPieces()
                checkSlowLater()
            }
        }
    }

    /// Apple's CDN sometimes puts a connection on a server that trickles (a few KB/s while the others do MB/s), and
    /// that piece holds one of the few connections for an hour. Such a piece is asked for again, on a new connection.
    private func restartSlowPieces() {
        let now = Date()
        for (id, pieces) in running {
            guard let job = jobs[id], !job.paused, let session else { continue }
            // Against the others when there are enough of them; near the end, when there aren't, a trickle is enough.
            let rates = pieces.values.compactMap { $0.rate(now) }.sorted()
            let typical = rates.count >= 3 ? rates[rates.count / 2] : .infinity
            for (n, r) in pieces {
                guard let started = r.started, now.timeIntervalSince(started) > Self.slowAge, let rate = r.rate(now),
                      rate < Self.slowBytesPerSecond, rate < typical * Self.slowShare else { continue }
                HengeLog.log("download", "\(id) piece \(n): \(Int(rate / 1024)) KB/s\(typical.isFinite ? " against \(Int(typical / 1024)) KB/s" : ""); new connection")
                r.task.cancel()
                running[id]?[n] = nil
                start(job, n, session)
            }
        }
    }

    private func retryLater(_ id: String, _ n: Int, after seconds: Double) {
        DispatchQueue.global().asyncAfter(deadline: .now() + seconds) { [self] in
            queue.addOperation { [self] in
                guard let job = jobs[id], !job.paused, let session, n >= assembled[id] ?? 0,
                      waiting[id]?.contains(n) != true, running[id]?[n] == nil else { return }
                start(job, n, session)
            }
        }
    }

    // MARK: Showing

    private func publish() {
        lastPublished = Date()
        var snapshot: [String: Status] = [:]
        for (id, job) in jobs {
            var received = min(Int64(assembled[id] ?? 0) * Self.piece, job.total)
            for n in waiting[id] ?? [] { received += pieceLength(job, n) }
            for (_, r) in running[id] ?? [:] { received += r.bytes }
            received = min(received, job.total)
            var rate = 0.0
            if job.paused {
                samples[id] = nil
            } else {
                var s = samples[id, default: []]
                s.append((lastPublished, received))
                s.removeAll { lastPublished.timeIntervalSince($0.0) > 20 }
                samples[id] = s
                if let first = s.first, lastPublished.timeIntervalSince(first.0) > 3, received > first.1 {
                    rate = Double(received - first.1) / lastPublished.timeIntervalSince(first.0)
                }
            }
            snapshot[id] = Status(received: received, total: job.total, paused: job.paused, bytesPerSecond: rate, problem: job.problem)
        }
        DispatchQueue.main.async { self.status = snapshot }
    }

    private func publishSoon() {
        if Date().timeIntervalSince(lastPublished) > 0.5 { publish() }
    }

    private static func error(_ text: String) -> NSError {
        NSError(domain: "Henge", code: 4, userInfo: [NSLocalizedDescriptionKey: text])
    }
}

extension ImageDownloads: URLSessionDownloadDelegate {
    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didWriteData bytesWritten: Int64,
                    totalBytesWritten: Int64, totalBytesExpectedToWrite: Int64) {
        guard case let (id, n)? = Self.piece(of: downloadTask), running[id]?[n]?.task === downloadTask else { return }
        running[id]?[n]?.bytes = totalBytesWritten
        if running[id]?[n]?.started == nil {
            running[id]?[n]?.started = Date()
            running[id]?[n]?.startBytes = totalBytesWritten - bytesWritten
        }
        publishSoon()
    }

    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didFinishDownloadingTo location: URL) {
        // The file is gone when this returns: keep it now.
        guard case let (id, n)? = Self.piece(of: downloadTask), let job = jobs[id], n >= assembled[id] ?? 0 else { return }
        let http = downloadTask.response as? HTTPURLResponse
        let status = http?.statusCode ?? 0
        let size = Self.fileSize(location)
        // "bytes 0-67108863/25595669503": the whole file must still be the one this download began with.
        let range = http?.value(forHTTPHeaderField: "Content-Range") ?? ""
        guard status == 206, size == pieceLength(job, n), range.hasSuffix("/\(job.total)") else {
            badAnswers[id, default: 0] += 1
            HengeLog.log("download", "\(id) piece \(n): Apple's server answered \(status), \(size) bytes, range \(range)")
            return // didCompleteWithError asks again
        }
        let file = pieceFile(id, n)
        try? FileManager.default.removeItem(at: file)
        guard (try? FileManager.default.moveItem(at: location, to: file)) != nil else { return }
        waiting[id, default: []].insert(n)
        badAnswers[id] = 0
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        guard case let (id, n)? = Self.piece(of: task) else { return }
        if running[id]?[n]?.task === task { running[id]?[n] = nil }
        guard let job = jobs[id] else { return }
        if waiting[id]?.contains(n) == true {
            appendWaiting(id)
            return publish()
        }
        if job.paused || (error as? URLError)?.code == .cancelled { return }
        if (badAnswers[id] ?? 0) >= 8 {
            return stop(id, "Apple's server isn't sending \(id) properly right now. Tap Resume to try again, or try later.")
        }
        // A dropped connection: ask for the piece again, sooner at first.
        if let error { HengeLog.log("download", "\(id) piece \(n): \(error.localizedDescription); asking again") }
        retryLater(id, n, after: min(60, 3 * Double(max(badAnswers[id] ?? 0, 1))))
        publishSoon()
    }

    func urlSessionDidFinishEvents(forBackgroundURLSession session: URLSession) {
        DispatchQueue.main.async {
            let done = self.backgroundEventsDone
            self.backgroundEventsDone = nil
            done?()
        }
    }
}
