import react from '@vitejs/plugin-react';
import { defineConfig } from 'vite';

export default defineConfig({
	// CEF loads the built application from `file://`, never from an HTTP origin, so every
	// emitted asset URL must be relative. The default base of `/` would resolve script and
	// stylesheet references against the filesystem root and the panel would load blank.
	base: './',
	plugins: [react()],
	build: {
		// CMake packages this directory into the extension. It is generated per build and
		// deliberately absent from version control (requirement 27.2).
		outDir: 'dist',
		assetsDir: 'assets',
		emptyOutDir: true,
		sourcemap: true,
	},
});
