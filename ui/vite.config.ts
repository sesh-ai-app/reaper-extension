import fs from 'node:fs';
import react from '@vitejs/plugin-react';
import { defineConfig, type Plugin } from 'vite';

/**
 * Where Vite writes the notices of the npm packages compiled into the bundle, relative to
 * `outDir`. CMake installs it into `sesh_ai_licenses/ui/` and keeps it out of
 * `sesh_ai_ui/`, so the name is repeated in the "Install rules" section of
 * `CMakeLists.txt` and the two have to change together.
 *
 * Not Vite's default `.vite/license.md`. CI hands `dist` to the build legs through
 * `actions/upload-artifact`, which leaves out hidden files and directories unless told
 * otherwise, so a notices file under `.vite/` would be missing from every artifact.
 */
const BUNDLED_PACKAGE_NOTICES_FILE_NAME = 'third-party-licenses.md';

// The names Vite's licence output reads a package's licence text from.
const LICENSE_FILE_NAME_PATTERN = /^(licen[cs]e|copying)/i;

// Apache-2.0 section 4(d): a NOTICE file has to travel with the work as well as the licence.
const NOTICE_FILE_NAME_PATTERN = /^notice/i;

// The package a bundled module belongs to: the deepest `node_modules/<name>` or
// `node_modules/@scope/<name>` in its path, which is the package Vite credits it to.
// Captures the package directory and the package name.
const PACKAGE_DIRECTORY_PATTERN = /^(.*\/node_modules\/((?:@[^/]+\/)?[^/]+))\//;

/**
 * Fails the build when a bundled package carries a notice Vite's licence output would leave
 * out: a NOTICE file, which Vite never reads, or no licence file at all, which leaves Vite
 * only the SPDX identifier to print. No package bundled today does either, so this reproduces
 * nothing. It exists so the first one that does stops the build and names itself, instead of
 * shipping an archive whose notices are quietly incomplete.
 */
function requireEveryBundledNoticeReproduced(): Plugin {
	return {
		name: 'sesh-ai:require-every-bundled-notice-reproduced',
		apply: 'build',
		generateBundle(_outputOptions, bundle) {
			// Package directory to package name.
			const bundledPackages = new Map<string, string>();

			Object.values(bundle).forEach((output) => {
				if (output.type !== 'chunk') {
					return;
				}

				output.moduleIds.forEach((moduleId) => {
					const match = moduleId.startsWith('\0') ? null : PACKAGE_DIRECTORY_PATTERN.exec(moduleId);

					if (match?.[1] !== undefined && match[2] !== undefined) {
						bundledPackages.set(match[1], match[2]);
					}
				});
			});

			const problems: string[] = [];

			bundledPackages.forEach((packageName, packageDirectory) => {
				const fileNames = fs.readdirSync(packageDirectory);
				const noticeFileNames = fileNames.filter((fileName) => NOTICE_FILE_NAME_PATTERN.test(fileName));

				if (noticeFileNames.length > 0) {
					problems.push(`${packageName} ships ${noticeFileNames.join(', ')}, which Vite's licence output does not reproduce`);
				}

				if (!fileNames.some((fileName) => LICENSE_FILE_NAME_PATTERN.test(fileName))) {
					problems.push(`${packageName} has no LICENSE, LICENCE or COPYING file, so its notice would be only an SPDX identifier`);
				}
			});

			if (problems.length > 0) {
				this.error(
					`${BUNDLED_PACKAGE_NOTICES_FILE_NAME} would be missing a notice the bundle has to carry:\n  ${problems.sort().join('\n  ')}\n`
					+ 'Reproduce it in the notices the build emits (vite.config.ts), then build again.',
				);
			}
		},
	};
}

export default defineConfig({
	// CEF loads the built application from `file://`, never from an HTTP origin, so every
	// emitted asset URL must be relative. The default base of `/` would resolve script and
	// stylesheet references against the filesystem root and the panel would load blank.
	base: './',
	plugins: [react(), requireEveryBundledNoticeReproduced()],
	build: {
		// CMake packages this directory into the extension. It is generated per build and
		// deliberately absent from version control (requirement 27.2).
		outDir: 'dist',
		assetsDir: 'assets',
		emptyOutDir: true,
		sourcemap: true,
		// The minified bundle keeps none of its packages' licence comments, so their notices
		// travel in this file instead. Vite builds it from the modules Rollup put in the
		// bundle, which leaves out dev dependencies and anything tree-shaken away.
		license: { fileName: BUNDLED_PACKAGE_NOTICES_FILE_NAME },
	},
});
