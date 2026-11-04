//
//  ExtBridge - optional I/O trace for debugging.
//
//  While the file commands/trace-io exists in the shared container, every
//  call that maps, completes or resizes file data is written as one text
//  line to trace/<uuid>-<time>.log:
//
//    <µs since start> <thread> <event> key=value ...
//
//  The extension checks for the switch file once a second, so a trace can
//  be started and stopped while a volume is mounted.
//
//  SPDX-License-Identifier: MIT
//

import Foundation

final class IOTrace {
    private let lock = NSLock()
    private var buffer = Data()
    private var handle: FileHandle?
    private let start = DispatchTime.now().uptimeNanoseconds
    /// read without the lock on the fast path; a stale value only drops or adds one line
    private(set) var isOn = false

    static var switchURL: URL? { L2MShared.dir("commands")?.appendingPathComponent("trace-io") }

    /// Starts or stops the trace to match the switch file.
    func update(uuid: String) {
        let want = Self.switchURL.map { FileManager.default.fileExists(atPath: $0.path) } ?? false
        if want == isOn { return }
        lock.lock()
        defer { lock.unlock() }
        if want {
            guard let dir = L2MShared.dir("trace") else { return }
            let f = DateFormatter()
            f.dateFormat = "yyyyMMdd-HHmmss"
            let url = dir.appendingPathComponent("\(uuid)-\(f.string(from: Date())).log")
            FileManager.default.createFile(atPath: url.path, contents: nil)
            handle = try? FileHandle(forWritingTo: url)
            isOn = handle != nil
        } else {
            writeLocked()
            try? handle?.close()
            handle = nil
            isOn = false
        }
    }

    func line(_ text: @autoclosure () -> String) {
        guard isOn else { return }
        let t = (DispatchTime.now().uptimeNanoseconds - start) / 1000
        var tid: UInt64 = 0
        pthread_threadid_np(nil, &tid)
        let s = "\(t) \(tid) \(text())\n"
        lock.lock()
        buffer.append(contentsOf: s.utf8)
        if buffer.count > 1 << 20 { writeLocked() }
        lock.unlock()
    }

    func flush() {
        lock.lock()
        writeLocked()
        lock.unlock()
    }

    private func writeLocked() {
        guard !buffer.isEmpty else { return }
        try? handle?.write(contentsOf: buffer)
        buffer.removeAll(keepingCapacity: true)
    }
}
