import { expect, test } from '@playwright/test';

test('loads the default spec with textures', async ({ page }) => {
  await page.goto('');
  await expect(page.locator('.port-name.in').first()).toHaveText('IN1');
  await expect(page.locator('canvas')).toBeVisible();
  await expect(page.getByText('Minecraft textures')).toBeVisible();
  await page.waitForTimeout(800);
  await page.screenshot({ path: 'test-results/default.png' });
});

test('solves the delay buffer and simulates it', async ({ page }) => {
  await page.goto('');
  await page.locator('select.examples').selectOption({ label: 'delay_buffer.txt' });
  await expect(page.locator('.notice')).toContainText('delay_buffer.txt');
  await page.getByRole('button', { name: 'Solve' }).click();
  await expect(page.locator('.status .good')).toBeVisible({ timeout: 60_000 });
  await expect(page.locator('.output-value')).toHaveText('OUT1 = 0');

  // Turning the input on reaches the output once settled.
  await page.locator('.input-slider input[type=range]').fill('15');
  await expect(page.locator('.output-value')).toHaveText('OUT1 = 15');
  await page.waitForTimeout(800);
  await page.screenshot({ path: 'test-results/solved.png' });
});

test('opens a sketched full adder and solves it from the sketch', async ({ page }) => {
  await page.goto('');
  await page.locator('select.examples').selectOption({ label: 'full_adder_flat_sketch.txt' });
  await expect(page.locator('.notice')).toContainText('full_adder_flat_sketch.txt');
  await page.getByLabel('first circuit only').check();
  await page.getByRole('button', { name: 'Solve' }).click();
  await expect(page.locator('.status .good')).toContainText('45 blocks', { timeout: 60_000 });
});

test('checks a sketch and marks changed solver options', async ({ page }) => {
  await page.goto('');
  await page.locator('select.examples').selectOption({ label: 'full_adder_flat_sketch.txt' });
  await page.getByRole('button', { name: 'Sketch (45)' }).click();
  await page.getByRole('button', { name: 'Check', exact: true }).click();
  await expect(page.locator('.popover.sketch')).toContainText('The sketch works.', { timeout: 30_000 });

  const options = page.getByRole('button', { name: 'Options' });
  await expect(options).not.toHaveClass(/changed/);
  await options.click();
  await page.getByLabel('Only as drawn').check();
  await expect(options).toHaveClass(/changed/);
});

test('shrinks a solved circuit into a smaller box', async ({ page }) => {
  await page.goto('');
  await page.locator('select.examples').selectOption({ label: 'not_gate.txt' });
  await expect(page.locator('.notice')).toContainText('not_gate.txt');
  await page.getByLabel('first circuit only').check();
  await page.getByRole('button', { name: 'Solve' }).click();
  await expect(page.locator('.status .good')).toBeVisible({ timeout: 60_000 });
  await page.getByRole('button', { name: 'Shrink' }).click();
  await expect(page.locator('.status')).toContainText(/Shrunk|No smaller box/, { timeout: 120_000 });
});
