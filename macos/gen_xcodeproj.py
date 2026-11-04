#!/usr/bin/env python3
"""Generates ExtBridge.xcodeproj (app + FSKit ExtensionKit extension).

The project uses Xcode's automatic signing, so `xcodebuild
-allowProvisioningUpdates` registers the App IDs, enables the FSKit Module
capability and creates the provisioning profiles by itself once an Apple
account is signed in to Xcode.

    python3 macos/gen_xcodeproj.py        # writes ./ExtBridge.xcodeproj
"""
import glob
import hashlib
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

objects = {}


def oid(*parts):
    return hashlib.md5('/'.join(parts).encode()).hexdigest()[:24].upper()


def q(s):
    s = str(s)
    if s and all(c.isalnum() or c in '._/$' for c in s) and not s.startswith('$('):
        return s
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


def render(v, ind):
    pad = '\t' * ind
    if isinstance(v, dict):
        out = '{\n'
        for k, x in v.items():
            out += pad + '\t' + q(k) + ' = ' + render(x, ind + 1) + ';\n'
        return out + pad + '}'
    if isinstance(v, list):
        out = '(\n'
        for x in v:
            out += pad + '\t' + render(x, ind + 1) + ',\n'
        return out + pad + ')'
    return q(v)


def add(key, obj):
    objects[key] = obj
    return key


# ---- files -------------------------------------------------------------

def file_ref(path, ftype):
    return add(oid('ref', path), {'isa': 'PBXFileReference', 'lastKnownFileType': ftype,
                                  'name': os.path.basename(path), 'path': path, 'sourceTree': 'SOURCE_ROOT'})


def build_file(target, ref, settings=None):
    o = {'isa': 'PBXBuildFile', 'fileRef': ref}
    if settings:
        o['settings'] = settings
    return add(oid('bf', target, ref), o)


shared = ['macos/Shared/Shared.swift']
ext_swift = sorted(glob.glob('macos/Extension/*.swift'))
app_swift = sorted(glob.glob('macos/App/*.swift'))
core_c = sorted(glob.glob('core/src/*.c'))
core_h = sorted(glob.glob('core/src/*.h')) + ['core/include/l2m/l2m.h']
plists = ['macos/Xcode/App-Info.plist', 'macos/Xcode/Extension-Info.plist']
ents = ['macos/Xcode/App.entitlements', 'macos/Xcode/Extension.entitlements']

refs = {}
for p in shared + ext_swift + app_swift:
    refs[p] = file_ref(p, 'sourcecode.swift')
for p in core_c:
    refs[p] = file_ref(p, 'sourcecode.c.c')
for p in core_h:
    refs[p] = file_ref(p, 'sourcecode.c.h')
for p in plists:
    refs[p] = file_ref(p, 'text.plist.xml')
for p in ents:
    refs[p] = file_ref(p, 'text.plist.entitlements')
refs['modulemap'] = file_ref('macos/L2MCore/module.modulemap', 'sourcecode.module-map')
icon = 'macos/App/AppIcon.icns'
has_icon = os.path.exists(icon)
if has_icon:
    refs[icon] = file_ref(icon, 'image.icns')

loc_refs = []
for lang in ('en', 'de'):
    path = f'macos/App/{lang}.lproj/Localizable.strings'
    loc_refs.append(add(oid('ref', path), {'isa': 'PBXFileReference', 'lastKnownFileType': 'text.plist.strings',
                                             'name': lang, 'path': path, 'sourceTree': 'SOURCE_ROOT'}))
loc_group = add(oid('variant', 'Localizable.strings'), {'isa': 'PBXVariantGroup', 'children': loc_refs,
                                                        'name': 'Localizable.strings', 'sourceTree': '<group>'})

app_product = add(oid('product', 'app'), {'isa': 'PBXFileReference', 'explicitFileType': 'wrapper.application',
                                          'includeInIndex': '0', 'path': 'ExtBridge.app', 'sourceTree': 'BUILT_PRODUCTS_DIR'})
ext_product = add(oid('product', 'ext'), {'isa': 'PBXFileReference', 'explicitFileType': 'wrapper.extensionkit-extension',
                                          'includeInIndex': '0', 'path': 'ExtBridgeFS.appex', 'sourceTree': 'BUILT_PRODUCTS_DIR'})


def group(name, children):
    return add(oid('group', name), {'isa': 'PBXGroup', 'children': children, 'name': name, 'sourceTree': '<group>'})


g_core = group('Core', [refs[p] for p in core_c + core_h])
g_ext = group('Extension', [refs[p] for p in ext_swift] + [refs['modulemap']])
g_app = group('App', [refs[p] for p in app_swift] + ([refs[icon]] if has_icon else []) + [loc_group])
g_shared = group('Shared', [refs[p] for p in shared])
g_support = group('Supporting Files', [refs[p] for p in plists + ents])
g_products = group('Products', [app_product, ext_product])
main_group = add(oid('group', 'main'), {'isa': 'PBXGroup', 'children': [g_app, g_ext, g_shared, g_core, g_support, g_products],
                                        'sourceTree': '<group>'})

# ---- build phases ------------------------------------------------------

ext_sources = add(oid('phase', 'ext', 'sources'), {
    'isa': 'PBXSourcesBuildPhase', 'buildActionMask': '2147483647', 'runOnlyForDeploymentPostprocessing': '0',
    'files': [build_file('ext', refs[p]) for p in shared + ext_swift + core_c]})
ext_frameworks = add(oid('phase', 'ext', 'fw'), {
    'isa': 'PBXFrameworksBuildPhase', 'buildActionMask': '2147483647', 'files': [], 'runOnlyForDeploymentPostprocessing': '0'})
app_sources = add(oid('phase', 'app', 'sources'), {
    'isa': 'PBXSourcesBuildPhase', 'buildActionMask': '2147483647', 'runOnlyForDeploymentPostprocessing': '0',
    'files': [build_file('app', refs[p]) for p in shared + app_swift]})
app_frameworks = add(oid('phase', 'app', 'fw'), {
    'isa': 'PBXFrameworksBuildPhase', 'buildActionMask': '2147483647', 'files': [], 'runOnlyForDeploymentPostprocessing': '0'})
app_resources = add(oid('phase', 'app', 'res'), {
    'isa': 'PBXResourcesBuildPhase', 'buildActionMask': '2147483647', 'runOnlyForDeploymentPostprocessing': '0',
    'files': ([build_file('app', refs[icon])] if has_icon else []) + [build_file('app', loc_group)]})
embed = add(oid('phase', 'app', 'embed'), {
    'isa': 'PBXCopyFilesBuildPhase', 'buildActionMask': '2147483647', 'dstPath': '$(EXTENSIONS_FOLDER_PATH)',
    'dstSubfolderSpec': '16', 'name': 'Embed ExtensionKit Extensions', 'runOnlyForDeploymentPostprocessing': '0',
    'files': [build_file('embed', ext_product, {'ATTRIBUTES': ['RemoveHeadersOnCopy']})]})

# ---- configurations ----------------------------------------------------

common = {
    'SDKROOT': 'macosx',
    'MACOSX_DEPLOYMENT_TARGET': '15.4',
    'SWIFT_VERSION': '5.0',
    'CODE_SIGN_STYLE': 'Automatic',
    'ENABLE_HARDENED_RUNTIME': 'YES',
    'MARKETING_VERSION': '0.9.3',
    'CURRENT_PROJECT_VERSION': '5',
    'L2M_BUNDLE_PREFIX': 'de.nexami',
    'GCC_C_LANGUAGE_STANDARD': 'c11',
    'CLANG_ENABLE_OBJC_ARC': 'YES',
    'ENABLE_USER_SCRIPT_SANDBOXING': 'YES',
    'DEAD_CODE_STRIPPING': 'YES',
    'ARCHS': '$(ARCHS_STANDARD)',
    'ALWAYS_SEARCH_USER_PATHS': 'NO',
}


def configs(name, extra, appstore_extra=None):
    out = []
    # AppStore: like Release, plus the APPSTORE flag (tips through Apple, sandboxed app)
    for c in ('Debug', 'Release', 'AppStore'):
        s = dict(common if name == 'project' else {})
        s.update(extra)
        if name == 'project':
            if c == 'Debug':
                s.update({'ONLY_ACTIVE_ARCH': 'YES', 'SWIFT_OPTIMIZATION_LEVEL': '-Onone', 'GCC_OPTIMIZATION_LEVEL': '0',
                          'DEBUG_INFORMATION_FORMAT': 'dwarf'})
            else:
                s.update({'SWIFT_OPTIMIZATION_LEVEL': '-O', 'GCC_OPTIMIZATION_LEVEL': '2',
                          'DEBUG_INFORMATION_FORMAT': 'dwarf-with-dsym', 'SWIFT_COMPILATION_MODE': 'wholemodule'})
            if c == 'AppStore':
                s['SWIFT_ACTIVE_COMPILATION_CONDITIONS'] = 'APPSTORE'
        if c == 'AppStore' and appstore_extra:
            s.update(appstore_extra)
        out.append(add(oid('cfg', name, c), {'isa': 'XCBuildConfiguration', 'name': c, 'buildSettings': s}))
    return add(oid('cfglist', name), {'isa': 'XCConfigurationList', 'buildConfigurations': out,
                                      'defaultConfigurationIsVisible': '0', 'defaultConfigurationName': 'Release'})


ext_cfg = configs('ext', {
    'PRODUCT_NAME': 'ExtBridgeFS',
    'PRODUCT_BUNDLE_IDENTIFIER': '$(L2M_BUNDLE_PREFIX).ExtBridge.FSModule',
    'INFOPLIST_FILE': 'macos/Xcode/Extension-Info.plist',
    'CODE_SIGN_ENTITLEMENTS': 'macos/Xcode/Extension.entitlements',
    'SWIFT_INCLUDE_PATHS': '$(SRCROOT)/macos/L2MCore',
    'HEADER_SEARCH_PATHS': '$(SRCROOT)/core/include $(SRCROOT)/core/src',
    'SKIP_INSTALL': 'YES',
    'GENERATE_INFOPLIST_FILE': 'NO',
    'WARNING_CFLAGS': '-Wno-unused-parameter',
})
app_cfg = configs('app', {
    'PRODUCT_NAME': 'ExtBridge',
    'PRODUCT_BUNDLE_IDENTIFIER': '$(L2M_BUNDLE_PREFIX).ExtBridge',
    'INFOPLIST_FILE': 'macos/Xcode/App-Info.plist',
    'CODE_SIGN_ENTITLEMENTS': 'macos/Xcode/App.entitlements',
    'GENERATE_INFOPLIST_FILE': 'NO',
    'ASSETCATALOG_COMPILER_APPICON_NAME': '',
}, {'CODE_SIGN_ENTITLEMENTS': 'macos/Xcode/App-AppStore.entitlements'})
proj_cfg = configs('project', {})

# ---- targets -----------------------------------------------------------

ext_target = add(oid('target', 'ext'), {
    'isa': 'PBXNativeTarget', 'name': 'ExtBridgeFS', 'productName': 'ExtBridgeFS',
    'productReference': ext_product, 'productType': 'com.apple.product-type.extensionkit-extension',
    'buildConfigurationList': ext_cfg, 'buildPhases': [ext_sources, ext_frameworks], 'buildRules': [], 'dependencies': []})
proxy = add(oid('proxy'), {'isa': 'PBXContainerItemProxy', 'containerPortal': oid('project'), 'proxyType': '1',
                           'remoteGlobalIDString': ext_target, 'remoteInfo': 'ExtBridgeFS'})
dep = add(oid('dep'), {'isa': 'PBXTargetDependency', 'target': ext_target, 'targetProxy': proxy})
app_target = add(oid('target', 'app'), {
    'isa': 'PBXNativeTarget', 'name': 'ExtBridge', 'productName': 'ExtBridge',
    'productReference': app_product, 'productType': 'com.apple.product-type.application',
    'buildConfigurationList': app_cfg, 'buildPhases': [app_sources, app_frameworks, app_resources, embed],
    'buildRules': [], 'dependencies': [dep]})

project = add(oid('project'), {
    'isa': 'PBXProject',
    'attributes': {'BuildIndependentTargetsInParallel': '1', 'LastSwiftUpdateCheck': '2600', 'LastUpgradeCheck': '2600',
                   'TargetAttributes': {app_target: {'CreatedOnToolsVersion': '26.0'},
                                        ext_target: {'CreatedOnToolsVersion': '26.0'}}},
    'buildConfigurationList': proj_cfg, 'compatibilityVersion': 'Xcode 14.0', 'developmentRegion': 'en',
    'hasScannedForEncodings': '0', 'knownRegions': ['de', 'en', 'Base'], 'mainGroup': main_group,
    'productRefGroup': g_products, 'projectDirPath': '', 'projectRoot': '', 'targets': [app_target, ext_target]})

text = '// !$*UTF8*$!\n' + render({'archiveVersion': '1', 'classes': {}, 'objectVersion': '60',
                                   'objects': objects, 'rootObject': project}, 0) + '\n'
os.makedirs('ExtBridge.xcodeproj', exist_ok=True)
with open('ExtBridge.xcodeproj/project.pbxproj', 'w') as f:
    f.write(text)

# shared scheme so `xcodebuild -scheme ExtBridge` works
os.makedirs('ExtBridge.xcodeproj/xcshareddata/xcschemes', exist_ok=True)
with open('ExtBridge.xcodeproj/xcshareddata/xcschemes/ExtBridge.xcscheme', 'w') as f:
    f.write(f'''<?xml version="1.0" encoding="UTF-8"?>
<Scheme LastUpgradeVersion = "2600" version = "1.7">
   <BuildAction parallelizeBuildables = "YES" buildImplicitDependencies = "YES">
      <BuildActionEntries>
         <BuildActionEntry buildForTesting = "YES" buildForRunning = "YES" buildForProfiling = "YES" buildForArchiving = "YES" buildForAnalyzing = "YES">
            <BuildableReference BuildableIdentifier = "primary" BlueprintIdentifier = "{app_target}" BuildableName = "ExtBridge.app" BlueprintName = "ExtBridge" ReferencedContainer = "container:ExtBridge.xcodeproj">
            </BuildableReference>
         </BuildActionEntry>
      </BuildActionEntries>
   </BuildAction>
   <LaunchAction buildConfiguration = "Debug" selectedDebuggerIdentifier = "Xcode.DebuggerFoundation.Debugger.LLDB" selectedLauncherIdentifier = "Xcode.DebuggerFoundation.Launcher.LLDB" launchStyle = "0" useCustomWorkingDirectory = "NO" ignoresPersistentStateOnLaunch = "NO" debugDocumentVersioning = "YES" debugServiceExtension = "internal" allowLocationSimulation = "YES">
      <BuildableProductRunnable runnableDebuggingMode = "0">
         <BuildableReference BuildableIdentifier = "primary" BlueprintIdentifier = "{app_target}" BuildableName = "ExtBridge.app" BlueprintName = "ExtBridge" ReferencedContainer = "container:ExtBridge.xcodeproj">
         </BuildableReference>
      </BuildableProductRunnable>
   </LaunchAction>
   <ArchiveAction buildConfiguration = "Release" revealArchiveInOrganizer = "YES">
   </ArchiveAction>
</Scheme>
''')
print('wrote ExtBridge.xcodeproj')
