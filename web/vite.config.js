import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import { viteSingleFile } from 'vite-plugin-singlefile'

// Produces a single self-contained index.html with all JS/CSS inlined.
// The embed_web.py script gzips this and converts it to a C byte array
// that gets compiled directly into the firmware binary.
export default defineConfig({
  plugins: [react(), viteSingleFile()],
  build: {
    outDir: 'dist',
    assetsInlineLimit: Infinity,
  },
})
