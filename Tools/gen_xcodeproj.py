#!/usr/bin/env python3
"""Generate TraceZero.xcodeproj from the files on disk.

    python3 Tools/gen_xcodeproj.py

Run it after adding or removing source files. The project is plain data:
every setting lives in this script, so the project can be reviewed, diffed
and recreated without Xcode. Object IDs are derived from file paths, so the
output is stable between runs.

Settings worth knowing (change them here, then re-run):
  BUNDLE_ID            your own reverse-DNS id if Xcode asks for a unique one
  DEPLOYMENT_TARGET    12.0 for the iPhone 5s (iOS 12); see docs/PORTING.md
  MAPLIBRE_VERSION     pinned to 6.10.0, the version known to run on the 5s
"""
import hashlib
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROJECT = os.path.join(ROOT, "TraceZero.xcodeproj")

NAME = "TraceZero"
BUNDLE_ID = "com.tracezero.app"
DEPLOYMENT_TARGET = "12.0"
MARKETING_VERSION = "2.0"
BUILD_NUMBER = "1"
MAPLIBRE_URL = "https://github.com/maplibre/maplibre-gl-native-distribution"
MAPLIBRE_VERSION = "6.10.0"


def oid(*parts):
    return hashlib.md5("/".join(parts).encode()).hexdigest()[:24].upper()


def q(s):
    """Quote a pbxproj string when needed."""
    if s and all(c.isalnum() or c in "._/$" for c in s) and not s[0].isdigit():
        return s
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


FILETYPES = {
    ".swift": "sourcecode.swift", ".c": "sourcecode.c.c", ".h": "sourcecode.c.h",
    ".plist": "text.plist.xml", ".entitlements": "text.plist.entitlements",
    ".storyboard": "file.storyboard", ".xcassets": "folder.assetcatalog", ".md": "net.daringfireball.markdown",
}


class Group:
    def __init__(self, name, path):
        self.name, self.path, self.children = name, path, []
        self.id = oid("group", path or name)


def main():
    objects = {}      # id -> (isa, body lines)
    sources, resources = [], []

    def add(obj_id, isa, fields):
        objects[obj_id] = (isa, fields)

    def file_ref(rel):
        ext = os.path.splitext(rel)[1]
        fid = oid("file", rel)
        ftype = FILETYPES.get(ext, "folder" if os.path.isdir(os.path.join(ROOT, rel)) else "text")
        add(fid, "PBXFileReference", [("lastKnownFileType", ftype), ("path", q(os.path.basename(rel))),
                                      ("sourceTree", '"<group>"')])
        return fid

    def build_file(fid, rel, kind):
        bid = oid("build", kind, rel)
        add(bid, "PBXBuildFile", [("fileRef", fid)])
        return bid

    # ---- groups mirror the folders
    main = Group("", "")
    app = Group("App", "App")
    core = Group("Core", "Core")
    products = Group("Products", None)
    products.id = oid("group", "Products")
    main.children = [app, core, products]

    def scan(group, rel_dir):
        abs_dir = os.path.join(ROOT, rel_dir)
        for name in sorted(os.listdir(abs_dir)):
            if name.startswith("."):
                continue
            rel = os.path.join(rel_dir, name)
            full = os.path.join(ROOT, rel)
            ext = os.path.splitext(name)[1]
            if os.path.isdir(full) and ext != ".xcassets" and name not in ("Styles", "Glyphs"):
                sub = Group(name, rel)
                scan(sub, rel)
                group.children.append(sub)
                continue
            fid = file_ref(rel)
            group.children.append(fid)
            if ext in (".swift", ".c"):
                sources.append(build_file(fid, rel, "src"))
            elif ext in (".storyboard", ".xcassets") or name in ("Styles", "Glyphs"):
                resources.append(build_file(fid, rel, "res"))

    scan(app, "App")
    scan(core, "Core")

    def emit_group(g):
        kids = [c.id if isinstance(c, Group) else c for c in g.children]
        fields = [("children", "(" + "".join(f"\n\t\t\t\t{k}," for k in kids) + "\n\t\t\t)")]
        if g.path:
            fields.append(("path", q(os.path.basename(g.path))))
        elif g.name:
            fields.append(("name", q(g.name)))
        fields.append(("sourceTree", '"<group>"'))
        add(g.id, "PBXGroup", fields)
        for c in g.children:
            if isinstance(c, Group):
                emit_group(c)

    # ---- product, package, phases
    app_ref = oid("product", NAME)
    add(app_ref, "PBXFileReference", [("explicitFileType", "wrapper.application"), ("includeInIndex", "0"),
                                      ("path", f"{NAME}.app"), ("sourceTree", "BUILT_PRODUCTS_DIR")])
    products.children = [app_ref]
    emit_group(main)

    pkg = oid("package", "maplibre")
    add(pkg, "XCRemoteSwiftPackageReference", [
        ("repositoryURL", q(MAPLIBRE_URL)),
        ("requirement", "{\n\t\t\t\tkind = exactVersion;\n\t\t\t\tversion = " + MAPLIBRE_VERSION + ";\n\t\t\t}")])
    pkg_dep = oid("package-product", "MapLibre")
    add(pkg_dep, "XCSwiftPackageProductDependency", [("package", pkg), ("productName", "MapLibre")])
    fw_build = oid("build", "framework", "MapLibre")
    add(fw_build, "PBXBuildFile", [("productRef", pkg_dep)])

    def phase(kind, files):
        pid = oid("phase", kind)
        add(pid, kind, [("buildActionMask", "2147483647"),
                        ("files", "(" + "".join(f"\n\t\t\t\t{f}," for f in files) + "\n\t\t\t)"),
                        ("runOnlyForDeploymentPostprocessing", "0")])
        return pid

    p_src = phase("PBXSourcesBuildPhase", sources)
    p_fw = phase("PBXFrameworksBuildPhase", [fw_build])
    p_res = phase("PBXResourcesBuildPhase", resources)

    # ---- build settings
    common_project = {
        "ALWAYS_SEARCH_USER_PATHS": "NO",
        "CLANG_ANALYZER_NONNULL": "YES",
        "CLANG_ENABLE_MODULES": "YES",
        "CLANG_ENABLE_OBJC_ARC": "YES",
        "CLANG_WARN_BOOL_CONVERSION": "YES",
        "CLANG_WARN_DOCUMENTATION_COMMENTS": "NO",
        "CLANG_WARN_EMPTY_BODY": "YES",
        "CLANG_WARN_INFINITE_RECURSION": "YES",
        "CLANG_WARN_INT_CONVERSION": "YES",
        "CLANG_WARN_UNREACHABLE_CODE": "YES",
        "COPY_PHASE_STRIP": "NO",
        "ENABLE_STRICT_OBJC_MSGSEND": "YES",
        "GCC_C_LANGUAGE_STANDARD": "gnu11",
        "GCC_NO_COMMON_BLOCKS": "YES",
        "GCC_WARN_64_TO_32_BIT_CONVERSION": "NO",
        "GCC_WARN_UNDECLARED_SELECTOR": "YES",
        "GCC_WARN_UNINITIALIZED_AUTOS": "YES_AGGRESSIVE",
        "GCC_WARN_UNUSED_FUNCTION": "YES",
        "GCC_WARN_UNUSED_VARIABLE": "YES",
        "IPHONEOS_DEPLOYMENT_TARGET": DEPLOYMENT_TARGET,
        "SDKROOT": "iphoneos",
    }
    debug_project = dict(common_project, **{
        "DEBUG_INFORMATION_FORMAT": "dwarf",
        "ENABLE_TESTABILITY": "YES",
        "GCC_DYNAMIC_NO_PIC": "NO",
        "GCC_OPTIMIZATION_LEVEL": "0",
        "GCC_PREPROCESSOR_DEFINITIONS": '("DEBUG=1", "$(inherited)")',
        "ONLY_ACTIVE_ARCH": "YES",
        "SWIFT_ACTIVE_COMPILATION_CONDITIONS": "DEBUG",
        "SWIFT_OPTIMIZATION_LEVEL": '"-Onone"',
    })
    release_project = dict(common_project, **{
        "DEBUG_INFORMATION_FORMAT": '"dwarf-with-dsym"',
        "ENABLE_NS_ASSERTIONS": "NO",
        "GCC_OPTIMIZATION_LEVEL": "s",
        "SWIFT_COMPILATION_MODE": "wholemodule",
        "SWIFT_OPTIMIZATION_LEVEL": '"-O"',
        "VALIDATE_PRODUCT": "YES",
    })
    target_common = {
        "ASSETCATALOG_COMPILER_APPICON_NAME": "AppIcon",
        "CODE_SIGN_STYLE": "Automatic",
        "CURRENT_PROJECT_VERSION": BUILD_NUMBER,
        "DEVELOPMENT_TEAM": '""',
        "ENABLE_BITCODE": "NO",
        "HEADER_SEARCH_PATHS": '"$(SRCROOT)/Core"',
        "INFOPLIST_FILE": "App/Info.plist",
        "IPHONEOS_DEPLOYMENT_TARGET": DEPLOYMENT_TARGET,
        "LD_RUNPATH_SEARCH_PATHS": '("$(inherited)", "@executable_path/Frameworks")',
        "MARKETING_VERSION": MARKETING_VERSION,
        "PRODUCT_BUNDLE_IDENTIFIER": BUNDLE_ID,
        "PRODUCT_NAME": NAME,
        "SWIFT_OBJC_BRIDGING_HEADER": '"App/Sources/TraceZero-Bridging-Header.h"',
        "SWIFT_VERSION": "5.0",
        "TARGETED_DEVICE_FAMILY": "1",
    }

    def config_list(kind, name, debug, release):
        d = oid("config", kind, "Debug")
        r = oid("config", kind, "Release")
        for cid, cname, settings in ((d, "Debug", debug), (r, "Release", release)):
            body = "{" + "".join(f"\n\t\t\t\t{k} = {v};" for k, v in sorted(settings.items())) + "\n\t\t\t}"
            add(cid, "XCBuildConfiguration", [("buildSettings", body), ("name", cname)])
        lid = oid("configlist", kind)
        add(lid, "XCConfigurationList", [("buildConfigurations", f"(\n\t\t\t\t{d},\n\t\t\t\t{r},\n\t\t\t)"),
                                         ("defaultConfigurationIsVisible", "0"),
                                         ("defaultConfigurationName", "Release")])
        return lid

    proj_cfg = config_list("project", NAME, debug_project, release_project)
    tgt_cfg = config_list("target", NAME, target_common, target_common)

    target = oid("target", NAME)
    add(target, "PBXNativeTarget", [
        ("buildConfigurationList", tgt_cfg),
        ("buildPhases", f"(\n\t\t\t\t{p_src},\n\t\t\t\t{p_fw},\n\t\t\t\t{p_res},\n\t\t\t)"),
        ("buildRules", "(\n\t\t\t)"),
        ("dependencies", "(\n\t\t\t)"),
        ("name", NAME),
        ("packageProductDependencies", f"(\n\t\t\t\t{pkg_dep},\n\t\t\t)"),
        ("productName", NAME),
        ("productReference", app_ref),
        ("productType", '"com.apple.product-type.application"'),
    ])
    project = oid("project", NAME)
    add(project, "PBXProject", [
        ("attributes", "{\n\t\t\t\tBuildIndependentTargetsInParallel = 1;\n\t\t\t\tLastSwiftUpdateCheck = 1500;"
                       "\n\t\t\t\tLastUpgradeCheck = 1500;\n\t\t\t}"),
        ("buildConfigurationList", proj_cfg),
        ("compatibilityVersion", '"Xcode 14.0"'),
        ("developmentRegion", "en"),
        ("hasScannedForEncodings", "0"),
        ("knownRegions", "(\n\t\t\t\ten,\n\t\t\t\tBase,\n\t\t\t)"),
        ("mainGroup", main.id),
        ("packageReferences", f"(\n\t\t\t\t{pkg},\n\t\t\t)"),
        ("productRefGroup", products.id),
        ("projectDirPath", '""'),
        ("projectRoot", '""'),
        ("targets", f"(\n\t\t\t\t{target},\n\t\t\t)"),
    ])

    # ---- write project.pbxproj, sections sorted by isa like Xcode does
    out = ["// !$*UTF8*$!", "{", "\tarchiveVersion = 1;", "\tclasses = {", "\t};", "\tobjectVersion = 56;", "\tobjects = {"]
    for isa in sorted({v[0] for v in objects.values()}):
        out.append("")
        out.append(f"/* Begin {isa} section */")
        for obj_id in sorted(k for k, v in objects.items() if v[0] == isa):
            fields = objects[obj_id][1]
            body = "".join(f"\n\t\t\t{k} = {v};" for k, v in fields)
            out.append(f"\t\t{obj_id} = {{\n\t\t\tisa = {isa};{body}\n\t\t}};")
        out.append(f"/* End {isa} section */")
    out += ["\t};", f"\trootObject = {project};", "}", ""]
    os.makedirs(PROJECT, exist_ok=True)
    with open(os.path.join(PROJECT, "project.pbxproj"), "w") as f:
        f.write("\n".join(out))

    # ---- workspace stub and shared scheme (xcodebuild needs a scheme for packages)
    ws = os.path.join(PROJECT, "project.xcworkspace")
    os.makedirs(ws, exist_ok=True)
    with open(os.path.join(ws, "contents.xcworkspacedata"), "w") as f:
        f.write('<?xml version="1.0" encoding="UTF-8"?>\n<Workspace\n   version = "1.0">\n'
                '   <FileRef\n      location = "self:">\n   </FileRef>\n</Workspace>\n')
    schemes = os.path.join(PROJECT, "xcshareddata", "xcschemes")
    os.makedirs(schemes, exist_ok=True)
    ref = (f'<BuildableReference BuildableIdentifier = "primary" BlueprintIdentifier = "{target}" '
           f'BuildableName = "{NAME}.app" BlueprintName = "{NAME}" ReferencedContainer = "container:TraceZero.xcodeproj">'
           '</BuildableReference>')
    scheme = f'''<?xml version="1.0" encoding="UTF-8"?>
<Scheme LastUpgradeVersion = "1500" version = "1.7">
   <BuildAction parallelizeBuildables = "YES" buildImplicitDependencies = "YES">
      <BuildActionEntries>
         <BuildActionEntry buildForTesting = "YES" buildForRunning = "YES" buildForProfiling = "YES" buildForArchiving = "YES" buildForAnalyzing = "YES">
            {ref}
         </BuildActionEntry>
      </BuildActionEntries>
   </BuildAction>
   <TestAction buildConfiguration = "Debug" selectedDebuggerIdentifier = "Xcode.DebuggerFoundation.Debugger.LLDB" selectedLauncherIdentifier = "Xcode.DebuggerFoundation.Launcher.LLDB" shouldUseLaunchSchemeArgsEnv = "YES">
      <Testables>
      </Testables>
   </TestAction>
   <LaunchAction buildConfiguration = "Debug" selectedDebuggerIdentifier = "Xcode.DebuggerFoundation.Debugger.LLDB" selectedLauncherIdentifier = "Xcode.DebuggerFoundation.Launcher.LLDB" launchStyle = "0" useCustomWorkingDirectory = "NO" ignoresPersistentStateOnLaunch = "NO" debugDocumentVersioning = "YES" debugServiceExtension = "internal" allowLocationSimulation = "YES">
      <BuildableProductRunnable runnableDebuggingMode = "0">
         {ref}
      </BuildableProductRunnable>
   </LaunchAction>
   <ProfileAction buildConfiguration = "Release" shouldUseLaunchSchemeArgsEnv = "YES" savedToolIdentifier = "" useCustomWorkingDirectory = "NO" debugDocumentVersioning = "YES">
      <BuildableProductRunnable runnableDebuggingMode = "0">
         {ref}
      </BuildableProductRunnable>
   </ProfileAction>
   <AnalyzeAction buildConfiguration = "Debug">
   </AnalyzeAction>
   <ArchiveAction buildConfiguration = "Release" revealArchiveInOrganizer = "YES">
   </ArchiveAction>
</Scheme>
'''
    with open(os.path.join(schemes, f"{NAME}.xcscheme"), "w") as f:
        f.write(scheme)
    print(f"wrote {PROJECT}: {len(sources)} source files, {len(resources)} resources")


if __name__ == "__main__":
    main()
