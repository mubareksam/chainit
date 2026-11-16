import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

export default defineConfig({
  plugins: [react()],
  publicDir: 'app/public',
  build: {
    outDir: 'app/dist',
    emptyOutDir: true,
  },
  server: {
    host: '0.0.0.0',
    port: 5173,
    allowedHosts: true,
    proxy: {
      '/api': 'http://127.0.0.1:4173',
      '/dav': 'http://127.0.0.1:4173',
      '/ws': {
        target: 'ws://127.0.0.1:4173',
        ws: true,
      },
    },
  },
});
