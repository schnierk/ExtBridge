//
//  ExtBridge - data shared between the app and the file system extension.
//
//  The extension runs sandboxed in its own process. It talks to the app
//  through small JSON files in the shared app group container:
//
//    prefs.json              settings written by the app, read by the extension
//    status/<uuid>.json      live state of every volume, written by the extension
//    commands/<uuid>.<cmd>   one-shot requests from the app (e.g. "verify")
//    results/<uuid>.verify.json  progress and report of a consistency scan
//
//  SPDX-License-Identifier: MIT
//

import Foundation

enum L2MShared {
    /// Short file system name, as shown by `mount` and used with `mount -F -t`.
    static let fsShortName = "extbridge"
    /// Default app group (the build script writes the real one into Info.plist).
    static let defaultGroup = "extbridge"

    static var groupID: String {
        (Bundle.main.object(forInfoDictionaryKey: "L2MAppGroup") as? String) ?? defaultGroup
    }

    static var container: URL? {
        FileManager.default.containerURL(forSecurityApplicationGroupIdentifier: groupID)
    }

    static func dir(_ name: String) -> URL? {
        guard let c = container else { return nil }
        let u = c.appendingPathComponent(name, isDirectory: true)
        try? FileManager.default.createDirectory(at: u, withIntermediateDirectories: true)
        return u
    }

    static var prefsURL: URL? { container?.appendingPathComponent("prefs.json") }

    static func loadPrefs() -> L2MPrefs {
        storedPrefs() ?? L2MPrefs()
    }

    /// nil if the app never saved settings (or they cannot be read)
    static func storedPrefs() -> L2MPrefs? {
        guard let u = prefsURL, let d = try? Data(contentsOf: u),
              let p = try? JSONDecoder().decode(L2MPrefs.self, from: d) else { return nil }
        return p
    }

    static func savePrefs(_ p: L2MPrefs) {
        guard let u = prefsURL else { return }
        let enc = JSONEncoder()
        enc.outputFormatting = [.prettyPrinted, .sortedKeys]
        if let d = try? enc.encode(p) { try? d.write(to: u, options: .atomic) }
    }

    static func writeJSON<T: Encodable>(_ value: T, to url: URL) {
        let enc = JSONEncoder()
        enc.dateEncodingStrategy = .iso8601
        enc.outputFormatting = [.sortedKeys]
        if let d = try? enc.encode(value) { try? d.write(to: url, options: .atomic) }
    }

    static func readJSON<T: Decodable>(_ type: T.Type, from url: URL) -> T? {
        guard let d = try? Data(contentsOf: url) else { return nil }
        let dec = JSONDecoder()
        dec.dateDecodingStrategy = .iso8601
        return try? dec.decode(type, from: d)
    }
}

/// Per volume settings, keyed by the ext4 UUID.
struct L2MVolumePrefs: Codable, Equatable {
    /// Mount read-only (never writes a single byte to the disk).
    var readOnly = false
    /// Do not mount automatically when the disk is attached.
    var noAutoMount = false

    init(readOnly: Bool = false, noAutoMount: Bool = false) {
        self.readOnly = readOnly
        self.noAutoMount = noAutoMount
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        readOnly = (try? c.decodeIfPresent(Bool.self, forKey: .readOnly)) ?? false
        noAutoMount = (try? c.decodeIfPresent(Bool.self, forKey: .noAutoMount)) ?? false
    }
}

struct L2MPrefs: Codable, Equatable {
    var volumes: [String: L2MVolumePrefs] = [:]
    /// Default for volumes that were never configured.
    var defaultReadOnly = false
    /// Show files of the Linux user as owned by the Mac user (and back).
    var mapOwners = true
    var linuxUID: UInt32 = 1000
    var linuxGID: UInt32 = 1000
    var macUID: UInt32 = 501
    var macGID: UInt32 = 20
    /// Keep the Linux disk free of Finder helper files.
    var blockDSStore = true
    var blockAppleDouble = true
    var blockSpotlight = true
    var blockTrash = false
    /// UUIDs for which the app asked for a one-time manual mount.
    var mountRequests: [String] = []
    /// Volumes (UUID -> until when) that may be mounted for writing although
    /// the check before the mount found problems. Only set on purpose, e.g.
    /// to delete one damaged file; an exception ends by itself.
    var checkExceptions: [String: Date] = [:]

    func prefs(for uuid: String) -> L2MVolumePrefs {
        volumes[uuid] ?? L2MVolumePrefs(readOnly: defaultReadOnly, noAutoMount: false)
    }

    func hasCheckException(_ uuid: String, now: Date = Date()) -> Bool {
        (checkExceptions[uuid] ?? .distantPast) > now
    }

    init() {}

    /// Settings written by an older version lack newer keys (or have an older
    /// type): keep the defaults for those instead of rejecting the whole file,
    /// which would make every disk read-only.
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        volumes = (try? c.decodeIfPresent([String: L2MVolumePrefs].self, forKey: .volumes)) ?? volumes
        defaultReadOnly = (try? c.decodeIfPresent(Bool.self, forKey: .defaultReadOnly)) ?? defaultReadOnly
        mapOwners = (try? c.decodeIfPresent(Bool.self, forKey: .mapOwners)) ?? mapOwners
        linuxUID = (try? c.decodeIfPresent(UInt32.self, forKey: .linuxUID)) ?? linuxUID
        linuxGID = (try? c.decodeIfPresent(UInt32.self, forKey: .linuxGID)) ?? linuxGID
        macUID = (try? c.decodeIfPresent(UInt32.self, forKey: .macUID)) ?? macUID
        macGID = (try? c.decodeIfPresent(UInt32.self, forKey: .macGID)) ?? macGID
        blockDSStore = (try? c.decodeIfPresent(Bool.self, forKey: .blockDSStore)) ?? blockDSStore
        blockAppleDouble = (try? c.decodeIfPresent(Bool.self, forKey: .blockAppleDouble)) ?? blockAppleDouble
        blockSpotlight = (try? c.decodeIfPresent(Bool.self, forKey: .blockSpotlight)) ?? blockSpotlight
        blockTrash = (try? c.decodeIfPresent(Bool.self, forKey: .blockTrash)) ?? blockTrash
        mountRequests = (try? c.decodeIfPresent([String].self, forKey: .mountRequests)) ?? mountRequests
        checkExceptions = (try? c.decodeIfPresent([String: Date].self, forKey: .checkExceptions)) ?? checkExceptions
    }
}

/// Live state of a volume, published by the extension.
struct L2MVolumeStatus: Codable, Identifiable, Equatable {
    var id: String { uuid }
    var uuid: String
    var label: String
    var bsdName: String
    var mounted: Bool
    var readOnly: Bool
    var readOnlyReason: String
    var features: [String]
    var unsupported: [String]
    var blockSize: UInt32
    var blocks: UInt64
    var freeBlocks: UInt64
    var reservedBlocks: UInt64
    var inodes: UInt32
    var freeInodes: UInt32
    var inodeSize: UInt32
    var groups: UInt32
    var state: String
    var mountCount: UInt32
    var maxMountCount: Int32
    var errorCount: UInt32
    var created: Date?
    var lastMount: Date?
    var lastWrite: Date?
    var lastCheck: Date?
    var lastError: Date?
    var kbytesWritten: UInt64
    var hasJournal: Bool
    var journalBlocks: UInt32
    var journalReplayed: Bool
    var journalTransactions: UInt32
    var journalBlocksReplayed: UInt32
    var metadataChecksums: Bool
    var hashAlgorithm: String
    var lastMountedOn: String
    var bytesReadSinceMount: UInt64
    var bytesWrittenSinceMount: UInt64
    var updated: Date
}

/// Progress / result of a consistency scan.
struct L2MVerifyResult: Codable, Equatable {
    var uuid: String
    var running: Bool
    var fraction: Double
    var problems: Int
    var report: String
    var started: Date
    var finished: Date?
}
