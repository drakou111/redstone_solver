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
