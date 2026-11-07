# Dependencies

Four C++ dependencies, acquired three different ways. The split is not
arbitrary — it follows what each dependency actually is.

| Dependency | Version | Acquired by |
|---|---|---|
| nlohmann/json | 3.12.0 | vcpkg manifest |
| nlohmann/json-schema-validator | 2.4.0 | vcpkg manifest |
| Catch2 | 3.16.0 | vcpkg manifest, or fetched from source |
| socket.io-client-cpp | 3.1.0+3b7be7e (unreleased `master`, 2025-08-28) | fetched from source, pinned commit |
| REAPER extension SDK | REAPER 7.80 headers | located, or cloned on request |
| CEF | 154.0.28+chromium-154.0.8037.58 | located, or downloaded on request |

Every version is pinned exactly. Nothing here resolves to "whatever is newest".

## vcpkg

`vcpkg.json` is the manifest. It carries a `builtin-baseline` — a fixed vcpkg
registry commit — plus `overrides` that pin each port to one version and port
version, so two machines configuring the same commit get the same libraries.

CMake needs the vcpkg toolchain file before `project()` runs. Give it one of:

```sh
cmake -S . -B build -DSESH_AI_VCPKG_ROOT=/path/to/vcpkg
```

```sh
export VCPKG_ROOT=/path/to/vcpkg
cmake -S . -B build
```

```sh
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
```

With a toolchain in play, vcpkg installs the manifest into `vcpkg_installed/` at
configure time. To get vcpkg itself:

```sh
git clone https://github.com/microsoft/vcpkg.git
./vcpkg/bootstrap-vcpkg.sh      # bootstrap-vcpkg.bat on Windows
```

**Configuring without vcpkg works.** Dependencies are then looked up on the
system, and configuration reports what it did and did not find rather than
failing. This matters for the test suite: a contributor with nothing installed
can still build and run it, because the REAPER-independent logic is compiled into
the test binary directly and needs none of these.

What it does not allow is building the extension itself. The shared library target
is created only when every prerequisite is present — sources under `src/`,
nlohmann/json, nlohmann/json-schema-validator, and the REAPER SDK — and
configuration lists whatever is missing and carries on without it. CI passes
`-DSESH_AI_REQUIRE_EXTENSION_LIBRARY=ON`, which turns the same situation into a
configuration failure, so a build with no library is impossible to ship by
accident and perfectly possible to work in.

CEF is the exception. It is acquired only once everything above is present, and
then a distribution that cannot be found or downloaded is a configuration error,
not a reason to skip the library. See the CEF section below.

## socket.io-client-cpp is not in the manifest

It used to be reachable as the `socket-io-client` vcpkg port, and the design
records it that way. That is no longer true. The port was removed from the vcpkg
registry along with `websocketpp`, the transport layer it depends on, which
upstream stopped maintaining. Neither port exists at the baseline pinned in
`vcpkg.json`, and vcpkg fails manifest resolution with an unhelpful error when a
manifest names a port its baseline has dropped
([vcpkg #45778](https://github.com/microsoft/vcpkg/issues/45778),
[vcpkg #44640](https://github.com/microsoft/vcpkg/issues/44640)).

Three ways out were considered:

1. **Pin the baseline to an older vcpkg commit** that still has the port. Rejected
   — it would drag every other dependency back to 2023 to solve a problem with
   one of them.
2. **Vendor overlay ports** for `socket-io-client` and `websocketpp` in this
   repository. Viable, and the option to revisit if the source fetch proves
   awkward. It means maintaining two portfiles for libraries vcpkg decided not
   to carry.
3. **Fetch from source at a pinned commit.** Chosen. The upstream repository
   vendors its own `websocketpp` and `asio` as submodules, so this acquires the
   whole transport layer as one pinned unit.

Declaring downloads nothing; the fetch happens when there are transport sources to
link against. The TLS-enabled target additionally needs OpenSSL, which the
transport work will add.

### The pin is an untagged `master` commit

CMake pins commit `3b7be7e4173b5bdeed393966e3274f65d513a280` — the tip of
`master` as of 2025-08-28, which is also the last commit upstream has pushed. The
full 40-character SHA, never a branch name and never a tag: a branch pin is not
reproducible and fails the OSSF Scorecard Pinned-Dependencies check. The version
variable reads `3.1.0+3b7be7e (unreleased master)`, because a build that is not
`3.1.0` should not say it is.

**Tag `3.1.0` cannot satisfy requirement 3.1.** The requirement puts the access
token, `clientType`, and `protocolVersion` in the Socket.IO handshake `auth`, and
the server reads `socket.handshake.auth` in `readHandshakeAuth` with no fallback
to the query string or the headers. At `3.1.0` — released 2021-10-12 and still the
newest tag upstream has cut — `sio::client::connect` has three overloads,
`(uri)`, `(uri, query)`, and `(uri, query, http_extra_headers)`, and none of them
takes an auth message. `query` reaches the server as `handshake.query` and
`http_extra_headers` as `handshake.headers`, so neither is somewhere the server
looks; a token in the query string would also be a token in the server's access
log. The overloads taking `const message::ptr& auth` landed six weeks after the
tag, in
[upstream #335](https://github.com/socketio/socket.io-client-cpp/pull/335)
(2021-11-23), and no release has been cut in the years since. Moving the pin past
the newest tag is the only way to reach the overload without forking the library,
and that is the trade taken — forking was the alternative and was rejected.

The overload the extension calls, read from `src/sio_client.h` at the pinned
commit rather than inferred from a changelog:

```cpp
void connect(const std::string& uri, const std::map<std::string,std::string>& query,
             const std::map<std::string,std::string>& http_extra_headers, const message::ptr& auth);
```

### The supply-chain position

Stated plainly, because it is accepted rather than overlooked:

- Upstream's last commit is 2025-08-28 and its last release is 2021-10-12. The
  repository is MIT licensed and not archived, but it is close to dormant.
- `websocketpp`, vendored as a submodule and pinned at `56123c8` — the 0.8.2
  release merge of 2020-04-19 — has had no functional development since April
  2020. Its only 2025 commits were a `cmake_minimum_required` bump for CMake 4
  compatibility. It is the transport layer under every byte the extension sends.
- Both `socket-io-client` and `websocketpp` were removed from the vcpkg registry,
  which is why this section exists at all.

The cost: a defect or vulnerability in the WebSocket or TLS handling will
probably not be fixed upstream, so it gets fixed or routed around here. The
buy: one pinned unit that vendors its own `websocketpp`, `asio`, and `rapidjson`,
needs no Boost, and compiles with C++11.

### Dependabot cannot watch this dependency

Dependabot has supported vcpkg manifests since
[August 2025](https://github.blog/changelog/2025-08-12-dependabot-version-updates-now-support-vcpkg/),
so it can watch the three ports in `vcpkg.json`. It cannot watch this one. There
is no CMake `FetchContent` support —
[dependabot-core #7451](https://github.com/dependabot/dependabot-core/issues/7451)
is open — and the umbrella "add support for C++ projects" request,
[#2027](https://github.com/dependabot/dependabot-core/issues/2027), was closed on
2026-01-31 on the grounds that vcpkg now exists and CMake and Conan are tracked
separately. A `FetchContent` commit pin is therefore invisible to it, as are the
CEF distribution and the REAPER SDK, for the same reason.

Monitoring this pin has to be something else: a scheduled CI job that compares
the pinned SHA against upstream `master` and the tag list and opens an issue when
either moves. **Follow-up, not built here** — task 21.1's CI workflow is where it
belongs.

The decision and the alternatives that were turned down are recorded as ADR 0021
in the monorepo.

### Deferred alternative: `jfayot/sioxx`

[`jfayot/sioxx`](https://github.com/jfayot/sioxx) — a C++17 Socket.IO client on
Boost.Asio, Boost.Beast, and nlohmann/json, with namespace auth in a tagged
release — was evaluated and deferred, because it needs Boost 1.74+, OpenSSL, and
CMake 3.28, sits on a v0.x line with a single maintainer (v0.3.0 at the time of
writing), and its long-polling fallback would have to be disabled for requirement
3.1. The swap surface is one translation unit behind the `SocketConnection` seam,
so whoever meets the `websocketpp` problem next can take this up without
disturbing the rest of the transport.

## REAPER extension SDK

The plugin boundary itself: `reaper_plugin.h` for the registration table REAPER
hands the extension at load, and `reaper_plugin_functions.h` for the C API the
tool implementations call. Cockos distributes it freely as a header-only
mini-SDK, published at
[justinfrankel/reaper-sdk](https://github.com/justinfrankel/reaper-sdk), so it is
located rather than resolved from a registry.

It takes two repositories, because the headers need each other. On macOS and
Linux `reaper_plugin.h` includes `"../WDL/swell/swell.h"` — a path relative to
its own location — so a usable tree is the merged layout the SDK's own README
describes:

```
<root>/sdk/     from justinfrankel/reaper-sdk
<root>/WDL/     the inner WDL directory of justinfrankel/WDL
```

On Windows `reaper_plugin.h` reaches for `<windows.h>` instead and WDL is not
needed, but the acquisition produces the same tree on all three platforms so a
developer moving between them does not meet a different layout.

Search order:

1. `-DSESH_AI_REAPER_SDK_ROOT=<path>`
2. the `REAPER_SDK_ROOT` environment variable
3. `third_party/reaper-sdk` in this repository

A directory counts when both headers are present, plus swell on the platforms
that need it — checking the headers rather than the directory means a
half-finished clone reports as missing instead of failing later in a compile.

To clone the pinned commits into `third_party/reaper-sdk` (ignored by git):

```sh
cmake -S . -B build -DSESH_AI_DOWNLOAD_REAPER_SDK=ON
```

Both commits are pinned in `CMakeLists.txt`. Git history is stripped after the
clone, because what is wanted is a pinned header tree rather than a checkout to
develop in; the result is about 13 MB, essentially all of it WDL. Nothing large
is committed — `third_party/` is ignored and the SDK arrives at configure time or
not at all. The download is opt-in even though it is cheap, because a build that
reaches the network without being asked is a worse default than one that explains
what it is missing. CI passes `-DSESH_AI_REQUIRE_REAPER_SDK=ON`.

**On macOS and Linux the build also needs Perl.** The panel's dialog resource,
`src/entry/panel_window.rc`, is compiled directly on Windows. Elsewhere WDL's
`swell/swell_resgen.pl` turns it into C++ at build time, and the library compiles
WDL's SWELL module stub with `SWELL_PROVIDED_BY_APP` so the dialog is created
through the SWELL functions REAPER provides. macOS ships `/usr/bin/perl`, Ubuntu
ships `perl-base`, and all four CI runner images have Perl, so nothing installs
it. WDL also includes a PHP version of the generator, which is not used because
PHP is missing from the `macos-15` and `ubuntu-22.04` images. The "panel's dialog
resource, and SWELL" section of `CMakeLists.txt` has the detail.

**No header outside a `*_reaper_*.cpp` includes the SDK.** Each component that
needs REAPER declares the narrowest interface it needs — `TimerRegistrar` is two
methods — implements it in one translation unit that includes the SDK, and the
suite substitutes a fake. There is no single `ReaperApi` interface enumerating
everything REAPER can do.

That is enforced structurally rather than by convention: the SDK include path is
added to the shared library target only, never to `sesh_ai_tests`, so a header
that reaches the SDK cannot be included from a test and the build says so. It is
also marked `SYSTEM`, because the SDK's inline virtuals produce about sixty-five
`-Wall -Wextra` warnings per translation unit, and warnings nobody can fix train
people to ignore warnings.

## CEF

CEF is a per-platform binary distribution measured in hundreds of megabytes, not
a package in a registry, so CMake locates it rather than resolving it. Search
order:

1. `-DSESH_AI_CEF_ROOT=<path>`
2. the `CEF_ROOT` environment variable
3. `third_party/cef/cef_binary_<version>_<platform>_minimal` in this repository

A directory counts as a distribution when it contains `cmake/FindCEF.cmake`, the
module CEF ships for embedders. Once found, `find_package(CEF)` runs and the
wrapper in `libcef_dll/` is compiled from source with the same settings as the
code consuming it.

The shared library is always built against CEF. When none of the three places
holds a distribution, configuration downloads the pinned one into
`third_party/cef` (ignored by git), so a plain configure with vcpkg is enough:

```sh
cmake -S . -B build
```

The download happens only in a configure that is about to create the library,
once every other prerequisite above is present. A configure without vcpkg, which
builds only the Catch2 suite, never downloads CEF and never configures it, even
when a distribution is already on disk. `-DSESH_AI_DOWNLOAD_CEF=OFF` forbids the
download, and then one of the three places above has to hold a distribution
already.

The archive is verified against the SHA1 published in the
[CEF build index](https://cef-builds.spotifycdn.com/index.json) before it is
extracted. Those digests live in `CMakeLists.txt`, one per platform; bumping
`SESH_AI_CEF_VERSION` means replacing them from the index.

To do it by hand instead, download the `minimal` archive for your platform from
[the CEF builds page](https://cef-builds.spotifycdn.com/index.html), extract it,
and point `-DSESH_AI_CEF_ROOT` at the result.

CEF is required for the extension. When the library would be created and no
distribution can be located, downloaded or extracted, configuration stops with a
message saying how to supply one. There is no option to build the library
without it, because an extension built that way would load in REAPER and never
be able to show its panel. The Catch2 suite is the one target that needs neither
CEF nor vcpkg; see "Catch2" below.

CEF is BSD-licensed, in the distribution's `LICENSE.txt`, and the Chromium
binaries it wraps carry their own notices in `CREDITS.html`. Both ship, in
`sesh_ai_licenses/cef/`; see "What ships, and its notices" below.

### Locales

The distribution carries 55 Chromium locales. The release ships eight, which
cover the seven the chat panel speaks plus Chromium's own fallback: `en-US`,
`de`, `ja`, `pt-BR`, `es`, `es-419`, `fr` and `it`. `en-US` is required
because Chromium falls back to it and loads no locale at all without it, and
`es-419` is there because Chromium resolves Latin American Spanish to it rather
than to `es`. On Windows and Linux they are `locales/<name>.pak`; on macOS they
are `<name>.lproj` directories inside the framework, trimmed before it is
signed. The mapping and the Chromium sources it was checked against are in
`SESH_AI_CEF_KEPT_LOCALES` in `CMakeLists.txt`. Bumping the CEF pin means
checking those names again; a name the distribution lacks stops the build.

## What ships, and its notices

`cmake --install build --prefix <dir>` produces the tree a producer unpacks into
REAPER's `UserPlugins`, and the release workflow archives exactly that. The
notices travel in `sesh_ai_licenses/`, never loose beside the binary:
`UserPlugins` is shared by every extension installed, and `LICENSE` or
`CREDITS.html` from another extension would overwrite ours, or ours theirs.

| File | Covers | Why it ships |
|---|---|---|
| `sesh_ai_licenses/LICENSE` | this repository | MIT; the notice accompanies copies |
| `sesh_ai_licenses/cef/LICENSE.txt` | CEF, including `libcef_dll_wrapper` | BSD-3; the wrapper is statically linked into the extension and the helpers, and the CEF binaries ship |
| `sesh_ai_licenses/cef/CREDITS.html` | Chromium and its third-party code | inside the CEF binaries that ship |
| `sesh_ai_licenses/nlohmann-json/copyright` | nlohmann/json | MIT; compiled into the extension |
| `sesh_ai_licenses/json-schema-validator/copyright` | nlohmann/json-schema-validator | MIT; statically linked into the extension |
| `sesh_ai_licenses/ui/third-party-licenses.md` | the npm packages in the chat panel's bundle: React, i18next, react-markdown and what they pull in | MIT and ISC; compiled into the minified script, which keeps none of their notices |

The two vcpkg notices are the `copyright` files vcpkg installs per port under
`vcpkg_installed/<triplet>/share/<port>/`, installed as they are rather than
restated, so they are always the text of the version linked. An install
configured without vcpkg has no such files and stops rather than shipping the
binary without them.

The UI's notices are written by `npm run build`, through Vite's `build.license`
option, to `ui/dist/third-party-licenses.md`: each package's name, version,
SPDX identifier and the text of its `LICENSE`. Vite lists the packages Rollup
put in the bundle, so the dev dependencies (Vite, Vitest, ESLint, TypeScript)
and what tree-shaking removed are not in it; that is 57 packages today, out of
94 non-dev entries in `ui/package-lock.json`. The rest are type-only packages
and modules no bundled code reaches, such as `debug`, which `micromark` imports
only in its development build. The install puts the file in
`sesh_ai_licenses/ui/` and leaves it out of `sesh_ai_ui/`, and a `ui/dist`
without it stops the install, as a missing `index.html` does.

Vite reads only `LICENSE`, `LICENCE` and `COPYING`. Apache-2.0 also requires a
package's `NOTICE` file to be reproduced, and Vite never reads one. No bundled
package is Apache-2.0 or ships a `NOTICE` today; the lockfile's Apache-2.0
packages are TypeScript and dependencies of ESLint, Vitest, jsdom and Testing
Library, none of them bundled. A
check in `ui/vite.config.ts` fails the UI build when a bundled package ships a
`NOTICE` file, or has no licence file and would be listed with only its
identifier, so a dependency change that adds either has to reproduce it before
the build passes.

Linked in, and needing no notice of its own:

- **WDL's SWELL module stub** (`swell-modstub.mm` on macOS,
  `swell-modstub-generic.cpp` on Linux) and the **REAPER SDK headers** are under
  Cockos's zlib-style licence, which asks for nothing in a binary distribution.

Not in what ships:

- **Catch2** links only into `sesh_ai_tests`, which is not installed.
- **socket.io-client-cpp** is declared but not yet fetched or linked; nothing in
  the extension calls it. When the transport work links it, its MIT notice and
  those of the `websocketpp`, `asio` and `rapidjson` it vendors have to join the
  table above, and OpenSSL's with them.
- **`SECURITY.md` and this file.** Both are about the repository, not the
  installed extension, and both would be more generic names in a shared
  directory.
- **The Windows import library** of the extension, and CEF's `libcef.lib`.
  Both are link-time only.
- **The Vite source map**, which only a developer with `ui/` in front of them
  has a use for.

## Catch2

Preferred from vcpkg. When no toolchain provides it, CMake fetches release
`v3.16.0` from source and verifies the archive against its SHA256. Turn that off
with `-DSESH_AI_FETCH_CATCH2=OFF`, in which case the test target is not created
and configuration says so.

Build and run the suite:

```sh
cmake -S . -B build
cmake --build build --target sesh_ai_tests
ctest --test-dir build --output-on-failure
```

Each Catch2 test case is registered with ctest individually, so a failure names
the case rather than the binary.

The suite does not link the shared library and has neither the REAPER SDK nor CEF
on its include path. That is deliberate — see the SDK section above.

## CI configuration

The two `REQUIRE` options exist so that everything configuration is willing to
skip on a developer machine is a failure in CI. CEF needs no option: its download
is on by default, and a library that cannot get it already stops configuration.
Every leg of both workflows configures this way, so every leg builds against
CEF. `ci.yml` exports `VCPKG_ROOT` instead of passing `-DSESH_AI_VCPKG_ROOT`,
for the Windows path reason its comment gives, and `release.yml` adds the vcpkg
triplet and, on macOS, the architecture.

```sh
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DSESH_AI_VCPKG_ROOT=/path/to/vcpkg \
  -DSESH_AI_DOWNLOAD_REAPER_SDK=ON -DSESH_AI_REQUIRE_REAPER_SDK=ON \
  -DSESH_AI_REQUIRE_EXTENSION_LIBRARY=ON
```

## Supported platforms

| Platform | Floor | Set by |
|---|---|---|
| macOS, arm64 and x86_64 | **13.0 Ventura** | `CMAKE_OSX_DEPLOYMENT_TARGET` in `CMakeLists.txt` |
| Windows, x64 | **Windows 10 22H2** | `_WIN32_WINNT=0x0A00` on the library target |
| Linux, x64 | **glibc 2.35** — Ubuntu 22.04 LTS, Debian 12 | the image the Linux build leg links on |

Declared in the build system rather than inherited from a CI runner image, which
is the whole point of having them written down. With nothing set, the binary's
compatibility is whatever the build machine's SDK or glibc happened to be, so the
floor silently tracks GitHub's image rollout — and those move: `ubuntu-latest`
migrates from Ubuntu 24.04 to 26.04 between 19 October and 19 November 2026. Every
runner label in `.github/workflows/` is pinned for that reason.

**macOS 13 is set by CEF, not chosen.** The pinned distribution is Chromium 154,
and Chromium 151 dropped macOS 12 Monterey, so Ventura is the oldest CEF permits.
Going lower would mean four Chromium milestones back and six re-derived digests.

**glibc has no flag.** A shared object requires whatever version the machine that
linked it provided, so the only way to promise 2.35 is to link on 2.35 — which is
why the Linux legs build on `ubuntu-22.04` rather than on a newer image.

`scripts/check-cef-platform-floor.sh` is what keeps these honest. It runs on the
macOS and Linux legs in both workflows, reads CEF's own minimum off the
downloaded distribution — `minos` from the framework's `LC_BUILD_VERSION`, and the
highest versioned glibc symbol in `libcef.so` — and fails the build when either
exceeds the floor above. CEF sets the real minimum on both platforms, and without
the check a mismatch produces a library that compiles, links, and then fails to
load on exactly the machines the floor existed for.

### Windows producers need the Visual C++ redistributable

The release workflow builds Windows with the `x64-windows-static-md` triplet:
vcpkg's static libraries against the **dynamic** CRT. So `json-schema-validator`
is linked in rather than shipped as a DLL, and CEF is forced to `/MD` so one CRT
is in play throughout — but the CRT itself is not in the archive.

That makes the Microsoft Visual C++ Redistributable for Visual Studio 2015–2022
(x64) an install-time requirement, independent of the Windows version floor. Most
machines running REAPER already have it, since REAPER's own installer and most
plugins need it. It is documented here rather than solved with a build flag:
`x64-windows-static` would avoid it and give two CRTs in one process, which is a
worse problem than a prerequisite.

Unverified, like everything else about the Windows leg — no Windows machine here
has configured this build.
