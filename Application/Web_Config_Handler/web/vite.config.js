import { defineConfig } from 'vite';
import { viteSingleFile } from 'vite-plugin-singlefile';

export default defineConfig({
  plugins: [viteSingleFile()],
  build: {
    // Output to firmware/ — this file is embedded by CMake EMBED_TXTFILES
    // web/index.html stays as the permanent dev source and is NEVER overwritten
    outDir: 'firmware',
    emptyOutDir: true,
    // Inline everything — no separate JS/CSS chunks
    assetsInlineLimit: 100000000,
    rollupOptions: {
      output: {
        inlineDynamicImports: true,
      },
    },
  },
  server: {
    proxy: {
      // In dev mode, forward all /api/* calls to the mock server
      '/api': 'http://localhost:3001',
    },
  },
});
