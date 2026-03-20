import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import { viteSingleFile } from 'vite-plugin-singlefile'

// Produces a single self-contained index.html with all JS/CSS inlined.
// The embed_web.py script gzips this and converts it to a C byte array
// that gets compiled directly into the firmware binary.
//
// Local preview:
//   1. Copy .env.example to .env.local and set your device IP
//   2. npm run dev  →  http://localhost:5173  (proxies /api/* to the device)
//   If the device is unreachable, the app falls back to built-in mock data.

export default defineConfig(({ command }) => ({
  plugins: [react(), ...(command === 'build' ? [viteSingleFile()] : [])],
  build: {
    outDir: 'dist',
    assetsInlineLimit: Infinity,
  },
  server: {
    proxy: process.env.VITE_DEVICE_IP ? {
      '/api': {
        target: `http://${process.env.VITE_DEVICE_IP}`,
        changeOrigin: true,
      },
    } : {},
  },
}))
