import { defineConfig } from 'vite';
import { readFileSync } from 'node:fs';

const { version } = JSON.parse(readFileSync(new URL('./neutralino.config.json', import.meta.url), 'utf8'));

export default defineConfig({
  base: './',
  // GV_REPO (owner/repo, set by CI) is the default GitHub repository for the update check
  define: { __APP_VERSION__: JSON.stringify(version), __UPDATE_REPO__: JSON.stringify(process.env.GV_REPO || '') },
  build: {
    outDir: 'resources',
    emptyOutDir: true,
    target: 'chrome110',
    chunkSizeWarningLimit: 2000
  }
});
