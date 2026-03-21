import { defineConfig } from 'vite';
import { viteSingleFile } from 'vite-plugin-singlefile';

export default defineConfig({
  plugins: [viteSingleFile()],
  build: {
    // Output to dist/ folder (Vite default location)
    outDir: 'dist',
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
