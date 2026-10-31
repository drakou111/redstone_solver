import { defineConfig } from '@playwright/test';

// Runs the real desktop app in --serve mode (no window) and drives its page.
const PORT = 47811;
const TOKEN = 'e2e';
const EXE = process.env.REDSTONE_EXE ?? '../build/msvc/Release/redstone_solver.exe';

export default defineConfig({
  testDir: 'e2e',
  timeout: 120_000,
  use: {
    baseURL: `http://127.0.0.1:${PORT}/?token=${TOKEN}`,
    viewport: { width: 1600, height: 1000 },
  },
  webServer: {
    command: `"${EXE}" --serve --port ${PORT} --token ${TOKEN}`,
    url: `http://127.0.0.1:${PORT}/`,
    reuseExistingServer: false,
    timeout: 30_000,
  },
});
