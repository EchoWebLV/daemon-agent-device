import { execSync } from 'child_process';
import { existsSync, mkdirSync } from 'fs';

mkdirSync('public/icons', { recursive: true });

const source = 'public/icons/daemon.png';

if (!existsSync(source)) {
  console.error(`Source logo not found at ${source}`);
  process.exit(1);
}

for (const size of [16, 48, 128]) {
  const out = `public/icons/icon${size}.png`;
  execSync(`sips -z ${size} ${size} "${source}" --out "${out}"`, { stdio: 'pipe' });
  console.log(`Generated icon${size}.png (${size}x${size})`);
}
