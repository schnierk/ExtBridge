//
//  ExtBridge - entry point of the FSKit app extension.
//
//  SPDX-License-Identifier: MIT
//

import ExtensionFoundation
import FSKit

@main
struct ExtBridgeFSExtension: UnaryFileSystemExtension {
    let fileSystem = Ext4FileSystem()
}
