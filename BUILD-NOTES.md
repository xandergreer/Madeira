# Building Madeira on macOS — notes from a clean clone

Recorded while getting a clean clone of this repo to build on macOS 26 /
Xcode 26.6 for an iPhone 17 Pro (A19 Pro, iOS 26.5). The README covers the
shape of the build; these are the specifics that are not written down
anywhere, plus the places where a clean clone does not build as published.

Everything below is already applied in this working tree.

## Host prerequisites

```sh
brew install cmake ninja meson pkg-config autoconf automake libtool bison llvm gnutls sevenzip
xcodebuild -downloadPlatform iOS      # the SDK alone is not enough, see below
```

- **bison**: macOS ships 2.3; Wine needs >= 3.0. brew's is keg-only, so it must be
  forced onto PATH for `configure`: `PATH="/opt/homebrew/opt/bison/bin:$PATH"`.
- **llvm**: only for `llvm-objcopy`, which `build/wineserver/build.sh` needs.
- **gnutls**: needed on the HOST at Wine-configure time. See "gnutls" below —
  this one is easy to get wrong and fails silently.
- **iOS platform component**: `xcrun --sdk iphoneos` works without it, so the
  native pieces compile happily, but `xcodebuild` then reports
  "iOS 26.5 is not installed" and finds no destination. ~8.5 GB.

## Toolchains

```sh
mkdir -p toolchains
curl -L https://github.com/mstorsjo/llvm-mingw/releases/download/20260421/llvm-mingw-20260421-ucrt-macos-universal.tar.xz | tar -xJ -C toolchains/
git clone --depth 1 --branch llvmorg-15.0.7 https://github.com/llvm/llvm-project.git toolchains/llvm-project
git clone --depth 1 --branch VER-2-13-3 https://github.com/freetype/freetype.git research/freetype
python3 -m venv toolchains/pyenv && toolchains/pyenv/bin/pip install "setuptools<81" packaging
```

The venv exists because FEX's CMake runs `Scripts/aarch64_fit_native.py`, which
imports `pkg_resources`. setuptools >= 81 drops it, so pin below that.

## Build order

Order matters more than the README implies — several steps consume generated
output from earlier ones.

### 1. Wine, configured for the macOS host

```sh
cd wine && mkdir -p build-macos && cd build-macos
PATH="/opt/homebrew/opt/bison/bin:../../toolchains/llvm-mingw-20260421-ucrt-macos-universal/bin:$PATH" \
PKG_CONFIG_PATH="/opt/homebrew/opt/gnutls/lib/pkgconfig:$PKG_CONFIG_PATH" \
  ../configure --enable-win64 --without-x --disable-tests --enable-archs=aarch64,arm64ec
```

Nothing documents this invocation, but every unix-side script reads
`wine/build-macos/include/config.h`. `--enable-archs` requires llvm-mingw on
PATH or configure aborts with "aarch64 PE cross-compiler not found".

**gnutls must be visible here.** If configure prints "libgnutls development
files not found", `config.h` leaves `SONAME_LIBGNUTLS` undefined, and then
`wine/dlls/bcrypt/gnutls.c` and `secur32/schannel_gnutls.c` compile to
*empty objects* — no warning, no error. The failure surfaces much later as
undefined `_bcrypt_unix_call_funcs` / `_secur32_unix_call_funcs` at the final
app link. Verify before continuing:

```sh
grep SONAME_LIBGNUTLS wine/build-macos/include/config.h   # must be a #define
```

### 2. Generated IDL headers

`build/ntdll-unix/build.sh` compiles dwrite's unixlib against widl-generated
headers and expects them under `wine/build-arm64ec/include`. A clean clone has
neither the headers nor that directory. Generate them in the tree that exists
and point the expected path at it:

```sh
cd wine/build-macos
PATH="/opt/homebrew/opt/bison/bin:$PATH" \
  make -k -j8 $(grep -oE "^include/[A-Za-z0-9_]+\.h:" Makefile | sed 's/:$//' | sort -u | tr '\n' ' ')
cd .. && mkdir -p build-arm64ec && ln -sfn ../build-macos/include build-arm64ec/include
```

257 headers. They are arch-independent widl output, so one tree serves both.

### 3. Native pieces

```sh
build/gnutls-ios/build.sh      # GMP -> nettle/hogweed -> GnuTLS, into toolchains/gnutls-ios
build/freetype-ios/build.sh
build/ntdll-unix/build.sh      # 30 files
build/win32u-unix/build.sh     # 46 files, merges libfreetype.a
build/wineserver/build-base.sh # see "wineserver" below
build/wineserver/build.sh      # needs llvm-objcopy on PATH
```

### 4. FEX

No recipe for this exists in the repo. Derived:

```sh
cd FEX
PATH="../toolchains/pyenv/bin:$PATH" cmake -S . -B build-ios -G Ninja \
  -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF -DBUILD_FEXCONFIG=False -DENABLE_LTO=False \
  -DTUNE_CPU=generic -DFEX_IOS_HOST_BUILD=True \
  -DCMAKE_C_FLAGS="-DFEX_IOS_HOST=1" -DCMAKE_CXX_FLAGS="-DFEX_IOS_HOST=1"
PATH="../toolchains/pyenv/bin:$PATH" ninja -C build-ios \
  FEXCore FEXCore_Base JemallocLibs cephes_128bit softfloat_3e fmt xxhash
```

Why each non-obvious flag:

- `CMAKE_SYSTEM_PROCESSOR=aarch64` — CMake leaves it empty when cross-compiling
  to iOS, and FEX aborts with "Unsupported processor type .".
- `TUNE_CPU=generic` — the default `native` runs `aarch64_fit_native.py` against
  `/proc/cpuinfo`, which does not exist on macOS; the empty result then fails
  `string(STRIP)` at CMakeLists.txt:510.
- `BUILD_TESTING=OFF` — otherwise the ASM unit tests demand a NASM assembler.
- `FEX_IOS_HOST=1` — used 141 times in the sources and defined by no
  CMakeLists. Without it `Core.cpp` does not compile. See the caveat below.

`build-ios` is not an arbitrary name: the Xcode project's LIBRARY_SEARCH_PATHS
point at `../FEX/build-ios/{FEXCore/Source,External/fmt,External/cephes,...}`.

### 5. LLVM 15 for iOS, then DXMT

Two stages, per `build/dxmt-ios/README.md`. Patch first:

```sh
sed -i '' '266s/MATCHES "Darwin"/MATCHES "Darwin|iOS"/' toolchains/llvm-project/llvm/cmake/modules/AddLLVM.cmake
```

Host tblgen, then the iOS libs reusing it:

```sh
cmake -S toolchains/llvm-project/llvm -B toolchains/llvm-host-build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DLLVM_TARGETS_TO_BUILD="AArch64"
ninja -C toolchains/llvm-host-build llvm-tblgen

cmake -S toolchains/llvm-project/llvm -B toolchains/llvm-ios-build -G Ninja \
  -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_TABLEGEN="$PWD/toolchains/llvm-host-build/bin/llvm-tblgen" \
  -DLLVM_BUILD_UTILS=Off -DLLVM_BUILD_TOOLS=Off -DLLVM_TARGETS_TO_BUILD="" \
  -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF \
  -DLLVM_ENABLE_TERMINFO=OFF -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF
ninja -C toolchains/llvm-ios-build      # LLVMHello.dylib fails; it is an
                                        # example plugin, ignore it
```

DXMT's airconv includes `air_*.h`, which are hexdumps of compiled Metal IR.
`build/dxmt-ios/build.sh` expects them in `shader-headers/` but nothing
generates them there — the rule lives in DXMT's meson build. Replicated:

```sh
mkdir -p build/dxmt-ios/shader-headers && cd build/dxmt-ios/shader-headers
for s in air_msad air_samplepos air_tessellation; do
  xcrun -sdk macosx metal -o $s.air -c ../../../research/dxmt/src/airconv/shaders/$s.metal \
    -std=metal3.1 --target=air64-apple-macos14.0
  xxd -n $s -i $s.air $s.h
done
```

Then:

```sh
build/dxmt-ios/build.sh
cd build/dxmt-ios && xcrun -sdk iphoneos libtool -static -o libdxmt_combined.a \
  obj/*.o ../../toolchains/llvm-ios-build/lib/*.a && cp libdxmt_combined.a ../../app/Madeira/
```

The PE side of DXMT (d3d11/dxgi/winemetal/d3d10core) is **already committed**
in `app/Madeira/aarch64-windows/`, so the meson cross build in that README is
not needed unless you are changing DXMT itself.

### 6. Host shims, then the app

```sh
build/ios-host-shims/build.sh   # MUST be re-run after any FEX rebuild
```

## The two places a clean clone does not build

### wineserver

`build/wineserver/build.sh` never built the library — it compiles ~19 files and
swaps them into a pre-existing archive with `ar r`. That base archive was
gitignored and never published, so a clean clone stops at
"ERROR: No base libwineserver.a found"; the other ~25 objects existed only
inside the unpublished file.

`build/wineserver/build-base.sh` (added here) rebuilds it: it reads the file
list from `wine/server/Makefile.in` so it tracks the submodule rather than
hardcoding a list, and compiles all 44 with build.sh's own CC_FLAGS so the
objects stay ABI- and macro-compatible with the ones swapped in afterwards.
All 44 compile for iOS unmodified. `ptrace.c`/`procfs.c`/`mach.c` are all in
SOURCES and all guarded internally, so the inapplicable ones yield near-empty
objects rather than errors.

Verification after `build.sh` runs: 47 objects, the 10 `ws_`-prefixed collision
renames present, no unrenamed definitions left.

### FEXCore host/PE symbol boundary

FEXCore's sources are shared between the ARM64EC PE module (llvm-mingw ->
`xtajit64.dll`) and the iOS host library the app links. Some iOS code in
`Core.cpp` and `ArchHelpers/Arm64.cpp` references symbols defined only on the
PE side, in `Source/Windows/ARM64EC/{IosJitAlias,Module}.cpp`:

    IosFfsBypassLog  IosJitReverseTranslate  IosMonoResolveRW
    ios_fex_mono_bridge_armed  ios_fex_mono_take_pending
    ios_fex_mono_captured_count  ios_fex_mono_count_activated
    ios_fex_mono_count_helper  rpm_cas_snapshot_take

The references sit OUTSIDE the `#ifdef FEX_IOS_HOST` guards that their `extern`
declarations sit inside — Core.cpp's guard closes at 1398 and the FFS-bypass
and cb-entry reporters follow it unguarded; likewise the rpm-cas probe near
1901 and Arm64.cpp's IosMonoResolveRW fallback at 382/413. In the PE build they
resolve. In the host build they do not, and there is no configuration of the
published tree where the host library both compiles and links.

`build/ios-host-shims/ios_host_shims.c` closes the link host-side. All but one
are diagnostic reads that now report nothing; the real counters are still
written inside the PE module, which is where the work happens.

**The exception is `IosMonoResolveRW`**, which is functional: Arm64.cpp asks it
for a writable alias of a code address once the fixed JIT pool
(`WINE_IOS_JIT_RX`) has missed, and treats 0 as "no alias found"
(`if (Rw) { ...; return Rw; }`) — so the stub falls back to the behaviour that
predates that fallback (ml656) rather than to anything undefined. If a game
shows atomics-related misbehaviour, suspect this first. Whether the host-side
FEXCore is meant to see that alias table at all is a question for upstream.

Two other upstream edits were needed, both applied in the submodules:

- `FEXCore/Source/Utils/ArchHelpers/Arm64.cpp` ~707: a diagnostic reporter
  called Win32 `VirtualQuery`/`MEMORY_BASIC_INFORMATION` unguarded. Now behind
  `#ifdef _WIN32`, keeping the full region breakdown on the PE side and an
  address-only report on the host.
- `pipe2()` does not exist on iOS; wine's `process_ios.c`/`server_ios.c` call it
  directly. Implemented on `pipe()` + `FD_CLOEXEC` in the shim file.

## Signing

The app needs `get-task-allow`, which only a **development** provisioning
profile carries. That entitlement is what lets StikDebug attach a debugger, and
per `EntitlementChecker.swift` the debugger attach is the entire JIT mechanism
on iOS — `com.apple.security.cs.allow-jit` is macOS-only and never granted
here. An enterprise/ad-hoc resign strips `get-task-allow`, producing an app
that installs and launches and then cannot execute one x86 instruction.

The project pins `DEVELOPMENT_TEAM = UT49TA9TA4` and
`PRODUCT_BUNDLE_IDENTIFIER = com.willfaust.mythicemu`; both must be changed to
your own before the app will provision.

---

# Vulkan / MoltenVK work (in progress)

Goal: give wined3d a working backend so DX9 titles render, which is also the
prerequisite for any later D3D12-via-vkd3d work. Before this, `d3d9.dll` and
`wined3d.dll` shipped but had no backend at all — win32u was built with
`-USONAME_LIBEGL -USONAME_LIBVULKAN`, so every graphics path except DXMT's
D3D11→Metal fell through to stubs.

## Done

**MoltenVK linked in.** `toolchains/MoltenVK/` (v1.4.2 release tarball,
`MoltenVK.xcframework/ios-arm64/libMoltenVK.a`, 6.8 MB).

`build/win32u-unix/vulkan_ios.c` is a per-file override in the same style as
`freetype_ios.c`: it re-defines `SONAME_LIBVULKAN`, rewrites dlopen/dlsym onto
the static library, and includes upstream `vulkan.c`. This turned out far
smaller than the gnutls equivalent because win32u resolves only TWO symbols by
`dlsym` — `vkGetInstanceProcAddr` and `vkGetDeviceProcAddr` — and reaches
everything else through the former. No generated symbol table needed; the two
targets are referenced by `__asm__` alias so the file needs no Vulkan headers.

`build/win32u-unix/build.sh` merges `libMoltenVK.a` into `libwin32u_unix.a`
exactly as it already does for freetype, so the Xcode project needs no new
library entry.

Verified: `libwin32u_unix.a` grew 3.3 MB → 11 MB and defines `_vkCreateInstance`
/ `_vkGetInstanceProcAddr` / `_vkGetDeviceProcAddr`; the linked app binary
carries 409 Vulkan/MoltenVK symbols and grew 38.7 MB → 43.8 MB. The app links
with no undefined symbols and needs no extra frameworks.

**winevulkan.dll (aarch64) built.** It was not built at all before. Wine's own
build produces it once the tree is configured:

```sh
cd wine/build-macos
make dlls/winevulkan/aarch64-windows/winevulkan.dll
```

Installed to `app/Madeira/aarch64-windows/`.

**winevulkan.dll (arm64ec) built** — and this needed a second Wine tree, for a
reason worth writing down.

`build-macos` is configured `--enable-archs=aarch64,arm64ec`, and it does
compile arm64ec *objects* (25k+ references in its Makefile). But it emits
**zero** arm64ec `.dll` LINK targets — `dlls/winevulkan/arm64ec-windows/` gets
`loader.o` and `loader_thunks.o` and nothing else. With the two archs listed
together, aarch64 is primary and arm64ec never gets linked.

Configuring a separate tree with arm64ec ALONE produces 1212 arm64ec DLL
targets:

```sh
cd wine && mkdir -p build-arm64ec && cd build-arm64ec
PATH="/opt/homebrew/opt/bison/bin:../../toolchains/llvm-mingw-.../bin:$PATH" \
  ../configure --enable-win64 --without-x --disable-tests --enable-archs=arm64ec
make dlls/winevulkan/arm64ec-windows/winevulkan.dll
```

This is almost certainly how the committed `arm64ec-windows/` bundle was
produced: its wined3d/d3d11/ntdll are genuinely different binaries from the
aarch64 ones, not copies.

Generate the IDL headers in this tree too — `build/ntdll-unix/build.sh` reads
`wine/build-arm64ec/include` for dwrite's widl output, so a real configure here
replaces the symlink suggested earlier and satisfies that dependency properly.

**Which bundle matters:** `WineProcessBridge.m:608` picks the whole DLL bundle
at launch — `arm64ec-windows` for x86-64 guests (i.e. actual games),
`aarch64-windows` for native ARM64 tests like `cube.exe`. Games therefore need
the arm64ec build. ARM64EC PEs report as "x86-64" to `file(1)`, matching the
other modules in that bundle; the aarch64 ones report "Aarch64".

Verified: app builds Release clean at 278 MB with winevulkan.dll in both
bundles, and `build/ntdll-unix/build.sh` still passes 30/30 against the real
build-arm64ec tree.

## Next

1. ~~arm64ec winevulkan~~ — done, see above. Original note follows for context: `build-macos` has no `arm64ec-windows` rule for
   winevulkan (nor for wined3d), yet the app ships arm64ec builds of wined3d,
   d3d11 and ntdll that are genuinely different binaries from their aarch64
   counterparts — so they came from a SECOND wine tree configured for that arch.
   That is the `wine/build-arm64ec` referenced by `build/ntdll-unix/build.sh`.
   It currently exists here only as a symlink supplying generated headers;
   replace it with a real configure before building arm64ec modules.
   x86-64 guests run under EC, so they will want the arm64ec variants.

2. **Vulkan surface on iOS.** MoltenVK needs a `CAMetalLayer` via
   `VK_EXT_metal_surface`. `app/Madeira/IOSDisplayShim.m` already impersonates
   `macdrv_functions` and vends a per-window `CAMetalLayer`
   (`winios_metal_layer_for_hwnd`) because DXMT needed exactly that — so this
   is likely far less work than it first appears. Wine's macOS driver creates
   Vulkan surfaces from the same interface.

3. **wined3d adapter_vk.** Wine 11.4 ships both `adapter_gl.c` and
   `adapter_vk.c`; once winevulkan works, DX9 should route through the Vulkan
   adapter.

## Note on Portal specifically

DX9 is only half of it. Portal (and the rest of the Source catalogue) is
32-bit x86, and this build has no 32-bit guest path: only `xtajit64.dll`
ships, with no `xtajit.dll`, no `wow64*.dll` and no i386 module directory.
FEX does carry a `Source/Windows/WOW64` module, but it is selected by
`elseif (ARCHITECTURE_arm64)` — mutually exclusive with the ARM64EC build this
project uses — and every iOS-Madeira commit targets ARM64EC, so none of the
iOS bring-up work (JIT alias tables, mono bridge, signal handling) exists for
it. That is a separate and much larger workstream than the graphics one.

## Step 4 — Vulkan surfaces on iOS (implemented, not yet runtime-exercised)

`build/win32u-unix/vulkan_driver_ios.c` (160 lines) is the iOS counterpart to
`wine/dlls/winemac.drv/vulkan.c`. Wine's Vulkan driver interface is only four
functions (`wine/include/wine/vulkan_driver.h:359`):

    p_vulkan_surface_create
    p_get_physical_device_presentation_support
    p_map_instance_extensions
    p_map_device_extensions

winemac.drv creates a Cocoa view, wraps it in a metal view, and passes the
resulting CAMetalLayer to `vkCreateMetalSurfaceEXT`. iOS has no Cocoa views —
but Madeira's compositor already keeps a CAMetalLayer per HWND so DXMT can
present into it (`winios_metal_layer_for_hwnd` in `app/Madeira/Winios/Winios.m`).
So the iOS driver skips the device/view dance and hands that existing layer
straight to MoltenVK.

The `client_surface` hooks are deliberately inert: the layer belongs to the
compositor, so destroying a Vulkan surface must not tear down something DXMT
may still be using, and presentation goes through the swapchain rather than
win32u.

`map_instance_extensions` advertises `VK_KHR_win32_surface` and
`VK_EXT_metal_surface` as implying each other, exactly as winemac.drv does, so
guests asking for the Win32 surface extension get the Metal one. There is no
`VK_MVK_macos_surface` fallback — that extension is macOS-only.

Wiring: `driver_ios.c` gains `winios_user_driver.pVulkanInit = winios_VulkanInit`
alongside the other driver slots; `build/win32u-unix/build.sh` compiles the new
file explicitly (it is not an upstream win32u source, so the source loop misses
it — the `ar` step globs `obj/*.o`, so compiling it is enough to archive it).

Verified: compiles clean, `_winios_VulkanInit` is defined and referenced by
driver_ios.o, the app links with no undefined symbols, and the weak
`winios_metal_layer_for_hwnd` reference binds (Winios.o exports it as `T`;
`nm -u` on the final binary shows zero unresolved references to it).

NOT verified at runtime. Nothing exercises this path yet — the cube smoke test
is DXMT/D3D11 and never asks Vulkan for a surface.

## Runtime status as of first device run (iPhone 17 Pro, A19 Pro, iOS 26.5)

The stack works. From `Documents/madeira-log.txt`:

- `CS_DEBUGGED flag: SET` — StikDebug attach works on A19 Pro
- 896 MB JIT pool allocated, RX at 0x1201dc000, RW alias at 0x7000000000
- x64 DX11 cube renders; 0 unhandled faults across 5803 lines
- **MoltenVK is live**: v1.4.2, 153 extensions supported, VkInstance created,
  GPU Family Apple 10, Metal Shading Language 4.0, 8192 MB
- Only 3 instance extensions enabled (the capability-query trio), no surface
  or swapchain yet — as expected, see above
- **None of the host shims fired**: zero hits on caspal128, atomic-anon
  (IosMonoResolveRW), rpm-cas, ffs-bypass or cb-entry. The stubs cost nothing
  in this workload.

Entitlements as signed: `increased-memory-limit: true`,
`extended-virtual-addressing: false` (the app suggests GetMoreRam for it),
`allow-jit: false` (expected — macOS-only, the JIT rides on CS_DEBUGGED).

Two operational notes: `Documents/fex-jit-dump.bin` is written at 896 MB every
run and is not behind any flag I could find, and the log showed two threads
parked 69 s on one address (`[waiters] parked=4 over60s=2`) even in a healthy
run.

## Step 5 (next) — make wined3d actually use the Vulkan adapter

Wine 11.4's wined3d ships `adapter_gl.c` and `adapter_vk.c`. Selecting the
Vulkan one is a registry setting in the prefix:

    HKCU\Software\Wine\Direct3D  "renderer" = "vulkan"

Nothing exercised `adapter_vk` in the first run, so this is untested. A DX9
title (or any wined3d app) is what will finally put the surface path above
under load.

## Step 5 + housekeeping

**The 896 MB dump per run — cause found and fixed.** It is not unconditional:
`build/ntdll-unix/signal_arm64_ios.c` writes it one-shot on the first
*unhandled* exec fault, to allow offline disassembly of FEX codegen. What made
it fire on every healthy run is the JIT detach handshake — `StikJITHelper`
deliberately attaches, waits for `CS_DEBUGGED`, allocates the pool and then
detaches by executing `BRK #0xf00d` with `x16=0` (CMD_DETACH). StikDebug
detaches in response, so nothing is left to service or step past that BRK and
it surfaces as an unhandled Mach exception (`type=6`, EXC_BREAKPOINT), at
`jit26_detach+0x4`.

The dump is now skipped for `EXC_BREAKPOINT`. The only BRK that reaches the
unhandled path is our own JIT protocol, and this dump is for exec faults.

Worth remembering: `CS_DEBUGGED` persists across the detach, which is the whole
trick — JIT pages stay executable without StikDebug remaining attached (its own
script notes it otherwise burns ~27 s CPU per 60 s and gets killed by the
`0x8BADF00D` scene-update watchdog).

**Selecting wined3d's Vulkan adapter.** No registry edit needed — wined3d honours
`WINE_D3D_CONFIG` (`wined3d_main.c:341`), which takes precedence over registry
keys. `WineProcessBridge.m` now reads `Documents/madeira-d3d.txt` and exports it,
mirroring the existing `madeira-args.txt` pattern, so it is changeable without a
rebuild:

    echo 'renderer=vulkan' > Documents/madeira-d3d.txt

Only affects wined3d (d3d9/d3d8/ddraw). d3d11 goes through DXMT and is
untouched.

**A test that actually exercises the surface path.** `build/x64-tests/d3d9tri-x64.c`
is a minimal D3D9 app (create device, clear to a cycling colour, Present 600
frames) built as an x86-64 PE, so it runs under FEX/EC like a real game. It is
the only way to reach
`d3d9 -> wined3d -> adapter_vk -> winevulkan -> win32u -> winios_vulkan_surface_create
-> vkCreateMetalSurfaceEXT -> MoltenVK -> CAMetalLayer`; the existing cube test
is d3d11/DXMT and never touches Vulkan. Each step prints to stdout so a failure
localises itself in the log. Launch button: "x64 D3D9 clear" in ContentView.

`build/x64-tests/build.sh` had the original author's absolute paths hardcoded
(`/Users/willfaust/...`); it is now repo-relative and takes `EXTRA_LIBS`:

    EXTRA_LIBS="-ld3d9 -luser32 -lgdi32" ./build.sh d3d9tri-x64

### Expected outcomes when running it

- `Direct3DCreate9 FAILED` — d3d9.dll is not loading at all; nothing to do with Vulkan.
- `CreateDevice FAILED hr=...` — this is where the swapchain, and so the Vulkan
  surface, is created. Cross-check the log for `winios_vulkan_surface_create`
  and `vkCreateMetalSurfaceEXT`.
- Colour on screen — the whole chain works, and DX9 titles become a real target.

### First D3D9 run — results

`WINE_D3D_CONFIG` works: the log shows
`err:winediag:wined3d_dll_init Using the Vulkan renderer.` — wined3d loaded,
read the config file and selected `adapter_vk`.

The test then failed at `Direct3DCreate9 FAILED -- no d3d9 at all`, before any
surface work. Cause: **`vulkan-1.dll` was missing from both bundles.**
`wined3d/adapter_vk.c:97` does `LoadLibraryA("vulkan-1.dll")` — wined3d reaches
Vulkan through the loader stub, NOT through `winevulkan.dll` directly, so
building winevulkan alone was not enough.

Built and installed for both arches:

```sh
make -C wine/build-macos   dlls/vulkan-1/aarch64-windows/vulkan-1.dll
make -C wine/build-arm64ec dlls/vulkan-1/arm64ec-windows/vulkan-1.dll
```

(Both land at 983040 bytes but are genuinely different binaries — the arm64ec
one reports machine type x86-64, as ARM64EC PEs do.)

Also confirmed from this run: the earlier "crash" on hitting the D3D9 button was
NOT the test — it was the known `BAD POOL` placement bug (`ml595`/`ml596`), where
the debugger's un-hinted `_M` allocation put the RX pool at 0x7000000000 inside
the reserved guest 64G window and returned the same hole on all three retries.
Deterministic within a launch; a relaunch cleared it.

## Getting d3d9 to initialise — four nested blockers

Each fix revealed the next. Final state: `Direct3DCreate9` succeeds and the
adapter enumerates; `CreateDevice` still fails.

**1. winevulkan's unixlib was never dispatched.** winevulkan.dll does NOT reach
Vulkan through win32u — it has its own unixlib called via `__wine_unix_call`
(`loader.c`: `__wine_init_unix_call()` + `UNIX_CALL`). iOS has no .so files, so
unixlibs are statically linked and matched by name in `virtual_ios.c`'s module
table (the same mechanism as bcrypt/secur32/crypt32/dwrite/ws2_32), and
`winevulkan` was absent. The PE loaded fine and every UNIX_CALL failed
silently. Fixed by compiling `dlls/winevulkan/vulkan.c` + `vulkan_thunks.c`
(the latter carries `__wine_unix_call_funcs`) into libntdll_unix.a as
`winevulkan_unix_call_funcs` and adding the table branch. ntdll now builds
32/32 rather than 30/30.

**2. vulkan-1.dll was missing.** `wined3d/adapter_vk.c:97` does
`LoadLibraryA("vulkan-1.dll")` — wined3d reaches Vulkan through the loader
stub, not winevulkan directly. Built for both arches.

**3. Silent failure hid both of the above.** win32u's `vulkan_init` jumps to
`failed:` when the host reports 0 instance extensions, but the ERR there is
guarded by `if (res)` and res is VK_SUCCESS on that path — so Vulkan was
disabled with no message at all. An explicit ERR is now emitted.

**4. `is_service_process()` is hardcoded TRUE on iOS** (`winstation_ios.c:415`)
so `get_desktop_window()` takes the force=1 server path and never spawns
explorer.exe. `sysparams_ios.c` reused that same predicate to skip
display-device enumeration entirely, leaving a bare virtual monitor with no GPU
and no source. `find_source_by_name(L"\\.\DISPLAY1")` therefore always failed,
`d3dkmt_open_adapter_from_gdi_display_name` returned STATUS_UNSUCCESSFUL, and
wined3d could not create an output. DXMT never needs a source, so nothing
noticed. Decoupled behind `MADEIRA_REAL_DISPLAY_DEVICES=1` so the default
(working) d3d11/DXMT path is unchanged.

After all four: Vulkan is fully live on-device — winevulkan dispatched,
`winios_VulkanInit` registering the driver, MoltenVK 1.4.2 creating a
VkInstance with **VK_KHR_surface + VK_EXT_metal_surface** enabled, wined3d
selecting adapter_vk, `Direct3DCreate9 ok, adapters=1`, adapter reported as
"Radeon (TM) RX 480 Graphics" (the iOS driver's placeholder PCI id).

### Current blocker: wined3d_cs_create

    err:d3d:wined3d_cs_create Failed to get wined3d module handle.

`cs.c:3704` does
`GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, wined3d_cs_run, ...)`
— it asks which module contains one of its own functions. On this port PE code
is COPIED INTO THE JIT POOL and executed there (`[jit-pool] image
0x71fdf60000+0x370000 (wined3d.dll) -> pool 0x11f2b0000`), so the address falls
inside the pool, which is not a registered module, and the lookup fails.

This is a port-level limitation, not a Vulkan one: any module doing an
address->module lookup on its own code hits it. The port already has the needed
primitive — `ios_jit_reverse_translate_addr()` in `loader_ios.c`, installed as
the `p_ios_jit_reverse_translate_addr` hook — which maps a pool-copy address
back to its PE VA. Wiring it into the FROM_ADDRESS lookup means touching PE
ntdll (`LdrGetDllHandleEx` / `RtlPcToFileHeader`), i.e. rebuilding
`arm64ec-windows/ntdll.dll`, which the build-arm64ec tree now makes possible.

### Debugging aids added

- `Documents/madeira-winedebug.txt` — overrides WINEDEBUG verbatim. Wine only
  prints err/fixme by default, which hid every WARN in the chain above. Being
  file-driven, channels can be retuned with no rebuild; note the env is read
  once per process, so force-quit between changes.
- `Documents/madeira-env.txt` — general KEY=VALUE env overrides, so new knobs
  don't each need their own file.
- `build/x64-tests/d3d9tri-x64.c` — the D3D9 smoke test, and the only thing
  that exercises this path at all.

---

# D3D9 RENDERS ON DEVICE (2026-09-13)

    [d3d9tri] CreateDevice ok, dev=00000070393D2720
    [d3d9tri] first frame presented
    [d3d9tri] 300 frames presented

Zero Vulkan errors. Full chain: d3d9 -> wined3d adapter_vk -> winevulkan ->
win32u -> winios_vulkan_surface_create -> vkCreateMetalSurfaceEXT -> MoltenVK
-> CAMetalLayer -> Metal, on an iPhone 17 Pro (A19 Pro, iOS 26.5).

## Required runtime config

`Documents/madeira-d3d.txt`:

    renderer=vulkan,csmt=0

`Documents/madeira-env.txt`:

    MADEIRA_REAL_DISPLAY_DEVICES=1

**csmt=0 is mandatory, not tuning.** With the multithreaded command stream on,
`wined3d_cs_create` spawns a thread running `wined3d_cs_run` as emulated x64
across EC thunks, and the process dies with STATUS_ACCESS_VIOLATION
(0xc0000005) on a null deref inside `d3d9_CreateDevice`. Single-threaded
wined3d avoids the thread entirely. Costs performance; the alternative is not
working at all.

## The last two fixes

**Display enumeration** — `is_service_process()` is hardcoded TRUE on iOS so
`get_desktop_window()` never spawns explorer.exe, but `sysparams_ios.c` reused
that predicate to skip display-device enumeration, leaving no GPU and no
source. wined3d needs a source to create an output. Decoupled behind
`MADEIRA_REAL_DISPLAY_DEVICES=1`; verified non-regressive (the DX11 cube still
renders with it on).

**Metal layer resolution** — the first version of
`winios_vulkan_surface_create` called `winios_metal_layer_for_hwnd()`, which is
only the DESKTOP-mode path and returns NULL for games, giving
`VK_ERROR_INCOMPATIBLE_DRIVER`. `IOSDisplayShim.m` picks by mode: per-window
layer in desktop mode, fullscreen singleton (`g_layer`) in game mode. Added
`madeira_metal_layer_for_hwnd()` there mirroring that rule, and the Vulkan
driver now calls it.

## Debugging technique worth reusing

Crash addresses land in the JIT pool and the unwinder reports "no module".
To symbolise:

1. Map the pool address back to a module using the log's
   `[jit-pool] image <pe>+<size> (<name>) -> pool <pool>` lines: the PE VA is
   `pe + (addr - pool)`.
2. Rebuild that DLL from `wine/build-arm64ec`. Every module tested so far
   (ntdll, d3d9, kernelbase) rebuilds **byte-identical** in `.text` to the
   committed binary, so RVAs are trustworthy and the rebuild carries DWARF.
3. `llvm-symbolizer --obj=<rebuilt dll> <ImageBase + RVA>`.

That turned "pc=0x1233dc0dc, no module" into
"`$iexit_thunk$cdecl$i8$i8` in kernelbase, called from `d3d9_CreateDevice`".

## What this does and does not mean for Portal

DX9 was half the problem and it is now solved. The other half is unchanged:
Portal is 32-bit x86, and this build has no 32-bit guest path -- only
`xtajit64.dll`, no `xtajit.dll`, no `wow64*.dll`, no i386 module directory.
FEX carries `Source/Windows/WOW64` but it is selected by
`elseif (ARCHITECTURE_arm64)`, mutually exclusive with the ARM64EC build this
project uses, and none of the iOS bring-up work (JIT alias tables, mono bridge,
signal handling) exists for it.

64-bit DX9 titles, however, are now a real target where they were impossible
this morning.
