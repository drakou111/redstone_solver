import { expect, type Page, test } from '@playwright/test';

// Sweeps the pointer over the 3D view until the hover card matches, then
// returns that screen position (the scene has no DOM nodes to target).
async function findInScene(page: Page, matches: (text: string) => boolean): Promise<{ x: number; y: number }> {
  const canvas = page.locator('.canvas-wrap canvas');
  // The canvas starts at a default size until it measures its container.
  await expect.poll(async () => (await canvas.boundingBox())?.width ?? 0).toBeGreaterThan(600);
  const box = await canvas.boundingBox();
  if (!box) throw new Error('no canvas');
  for (let fy = 0.2; fy <= 0.85; fy += 0.04) {
    for (let fx = 0.2; fx <= 0.8; fx += 0.03) {
      const x = box.x + box.width * fx;
      const y = box.y + box.height * fy;
      await page.mouse.move(x, y);
      const info = page.locator('.hover-info');
      if ((await info.count()) > 0 && matches(await info.innerText())) return { x, y };
    }
  }
  throw new Error('nothing matching under the pointer');
}

async function solveExample(page: Page, example: string) {
  await page.goto('');
  await page.locator('select.examples').selectOption({ label: example });
  await expect(page.locator('.notice')).toContainText(example);
  await page.getByRole('button', { name: 'Solve' }).click();
  await expect(page.locator('.status .good')).toBeVisible({ timeout: 60_000 });
}

test('adds an input by clicking in the scene', async ({ page }) => {
  await page.goto('');
  await expect(page.locator('.port-name.in')).toHaveCount(2);
  await page.keyboard.press('i');
  const spot = await findInScene(page, (text) => text.includes('free for the solver'));
  await page.mouse.click(spot.x, spot.y, { button: 'right' });
  await expect(page.locator('.port-name.in')).toHaveCount(3);
  await page.screenshot({ path: 'test-results/add-input.png' });
});

test('toggles an input by clicking it in the circuit', async ({ page }) => {
  await solveExample(page, 'delay_buffer.txt');
  await expect(page.locator('.output-value')).toHaveText('OUT1 = 0');
  const spot = await findInScene(page, (text) => text.includes('IN1'));
  await page.mouse.click(spot.x, spot.y);
  await expect(page.locator('.output-value')).toHaveText('OUT1 = 15');
});

test('removing a block from the circuit re-checks it', async ({ page }) => {
  await solveExample(page, 'delay_buffer.txt');
  const spot = await findInScene(page, (text) => /\n(solid|repeater|glass)/.test(text));
  await page.mouse.click(spot.x, spot.y);
  await expect(page.locator('.edit-badge.fail')).toBeVisible({ timeout: 30_000 });
  await page.screenshot({ path: 'test-results/edited.png' });
});

test('cancel stops a long solve promptly', async ({ page }) => {
  await page.goto('');
  // Several more inputs and a large volume make a solve that runs for long.
  await page.locator('.volume input.number').first().fill('8');
  await page.getByRole('button', { name: 'Solve' }).click();
  await expect(page.getByRole('button', { name: 'Cancel' })).toBeVisible();
  await page.waitForTimeout(3000);
  const pressed = Date.now();
  await page.getByRole('button', { name: 'Cancel' }).click();
  await expect(page.getByRole('button', { name: 'Solve' })).toBeVisible({ timeout: 10_000 });
  expect(Date.now() - pressed).toBeLessThan(5000);
});

test('a port the solver places gets an area drawn in the scene and solves', async ({ page }) => {
  await page.goto('');
  await page.locator('select.examples').selectOption({ label: 'not_gate.txt' });
  await expect(page.locator('.notice')).toContainText('not_gate.txt');
  const firstInput = page.locator('.port').first();
  await firstInput.locator('select').nth(1).selectOption('free');
  await expect(firstInput.locator('.region-button')).toContainText('1 cell');
  await expect(page.locator('.tools button.active')).toContainText('Port area');

  const spot = await findInScene(page, (text) => text.includes('free for the solver'));
  await page.mouse.click(spot.x, spot.y, { button: 'right' });
  await expect(firstInput.locator('.region-button')).toContainText('2 cells');
  await findInScene(page, (text) => text.includes('allowed for IN1'));

  await page.getByRole('button', { name: 'Solve' }).click();
  await expect(page.locator('.status .good')).toBeVisible({ timeout: 60_000 });
  await page.screenshot({ path: 'test-results/free-port.png' });
});
