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

To download the pinned distribution into `third_party/cef` (ignored by git):

```sh
cmake -S . -B build -DSESH_AI_DOWNLOAD_CEF=ON
```

The archive is verified against the SHA1 published in the
[CEF build index](https://cef-builds.spotifycdn.com/index.json) before it is
extracted. Those digests live in `CMakeLists.txt`, one per platform; bumping
`SESH_AI_CEF_VERSION` means replacing them from the index.

To do it by hand instead, download the `minimal` archive for your platform from
[the CEF builds page](https://cef-builds.spotifycdn.com/index.html), extract it,
and point `-DSESH_AI_CEF_ROOT` at the result.

Without a distribution, configuration reports that the UI host cannot be built
and continues — there is no UI host yet, and blocking the test suite on a
hundreds-of-megabytes download nobody needs yet would be the wrong trade. CI
passes `-DSESH_AI_REQUIRE_CEF=ON`, which turns the same situation into a
configuration failure, so a CEF-less build can never ship by accident.

CEF is BSD-licensed; the Chromium binaries it wraps carry their own notices in
`LICENSE.txt` and `CREDITS.html` inside the distribution. Both have to be
included in what ships to producers.

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

The three `REQUIRE` options exist so that everything configuration is willing to
skip on a developer machine is a failure in CI:

```sh
cmake -S . -B build \
  -DSESH_AI_VCPKG_ROOT=/path/to/vcpkg \
  -DSESH_AI_DOWNLOAD_REAPER_SDK=ON -DSESH_AI_REQUIRE_REAPER_SDK=ON \
  -DSESH_AI_DOWNLOAD_CEF=ON -DSESH_AI_REQUIRE_CEF=ON \
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
macOS and Linux CEF legs in both workflows, reads CEF's own minimum off the
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
