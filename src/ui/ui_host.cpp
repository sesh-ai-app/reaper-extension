// The UI Host's compile-time gate.
//
// Everything the UI Host does is in the header, because none of it needs CEF and all of
// it needs testing — the same split, for the same reason, as `stream_presenter.h` and
// its translation unit. What cannot be in the header is a translation unit that includes
// it and nothing else: that is what proves the header is self-contained in the library
// build, where the test target's own includes are not there to cover for it.
//
// The static assertions below are the structural half of requirement 15.4, stated where
// the library build will see them. The suite asserts the same facts with STATIC_REQUIRE,
// which is the only way to test for code that must not compile; these are here so that a
// change to the payload type which quietly re-admits a pointer fails the library build
// too, on a machine that never runs the suite.
//
// This file pulls neither CEF nor nlohmann/json, so it costs nothing to compile and does
// not make the library target depend on a CEF distribution being present.

#include <ui/ui_host.h>

#include <string>
#include <type_traits>

namespace sesh_ai::ui {

	namespace {

		// Stands in for a REAPER handle: declared, never defined, only ever held as a
		// pointer — which is exactly what `MediaTrack`, `ReaProject`, and
		// `TrackEnvelope` are.
		class opaque_reaper_handle;

		// The one way in.
		static_assert(
			is_bridge_transmittable_v<std::string>,
			"the bridge must carry serialized text"
		);

		// And no REAPER handle, by any spelling, ever reaches JavaScript
		// (requirement 15.4).
		static_assert(
			!is_bridge_transmittable_v<opaque_reaper_handle*>,
			"a REAPER handle must not be constructible into a bridge payload"
		);

		static_assert(
			!is_bridge_transmittable_v<void*>,
			"an erased pointer must not be constructible into a bridge payload"
		);

		static_assert(
			!std::is_constructible_v<BridgeMessage, std::string, opaque_reaper_handle*>,
			"a REAPER handle must not be constructible into a bridge message"
		);

		// The two URL types are obtainable only through their validating factories, which
		// is what keeps the `file://` asset path and the sign-in navigation from being
		// conflated. A constructor that became public would make both checks below fail.
		static_assert(
			!std::is_constructible_v<LocalAssetUrl, std::string>,
			"a local asset url must come from build_local_asset_url"
		);

		static_assert(
			!std::is_constructible_v<RemoteNavigationUrl, std::string>,
			"a sign-in navigation url must come from build_sign_in_navigation_url"
		);

		// Neither converts to the other, so a validated local path cannot be handed to
		// the navigation and a validated remote URL cannot be loaded as an asset.
		static_assert(
			!std::is_constructible_v<RemoteNavigationUrl, LocalAssetUrl>,
			"a local asset url must not convert to a navigation url"
		);

		static_assert(
			!std::is_constructible_v<LocalAssetUrl, RemoteNavigationUrl>,
			"a navigation url must not convert to a local asset url"
		);

	}

}
