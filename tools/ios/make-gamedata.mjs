// Builds the data folder the native host expects (native/host/importer.cpp)
// from an installed game folder and the v1.02 no-CD golf.exe, for bundling
// into the iPad app. The output holds game files: keep it out of git.
//
// usage: node tools/ios/make-gamedata.mjs <installed game dir> <no-CD golf.exe> <out dir>
import { createHash } from 'node:crypto';
import { cpSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { basename, join } from 'node:path';
import { applyNops, mapPE } from '../pages/pe-image.mjs';

const NOCD_SHA1 = '6f05a3ae3b5050ed82dc0010c03842a2e0ab166b';
// Installer and launcher files the game never opens
const SKIP = /^(autorun\.(exe|ico|inf)|setup\.bat|regsetup\.exe|unpack\.exe|logfile\.txt|descript\.ion)$/i;

const [gameDir, exePath, out] = process.argv.slice(2);
if (!out) {
  console.error('usage: node tools/ios/make-gamedata.mjs <installed game dir> <no-CD golf.exe> <out dir>');
  process.exit(2);
}
const exe = readFileSync(exePath);
const sha1 = createHash('sha1').update(exe).digest('hex');
if (sha1 !== NOCD_SHA1) throw new Error(`${exePath} is not the v1.02 no-CD golf.exe (sha1 ${sha1})`);

rmSync(out, { recursive: true, force: true });
mkdirSync(join(out, 'image'), { recursive: true });
cpSync(gameDir, join(out, 'game'), { recursive: true, filter: (src) => !SKIP.test(basename(src)) });

const nops = JSON.parse(readFileSync(new URL('../../web/pages/jgld-nops.json', import.meta.url)));
writeFileSync(join(out, 'image', 'golf.bin'), mapPE(exe));
writeFileSync(join(out, 'image', 'jgld.bin'), applyNops(mapPE(readFileSync(join(gameDir, 'jgld.dll'))), nops));
writeFileSync(join(out, 'installed.txt'), 'installed\n');
console.log('wrote', out);
