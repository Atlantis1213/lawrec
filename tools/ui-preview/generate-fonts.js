const fs = require('fs');
const path = require('path');
const { spawnSync } = require('child_process');
if (!fs.existsSync('/.dockerenv')) throw new Error('Use tools/update-ui-fonts.sh (Docker only)');
const root = process.env.LAWREC_PROJECT_ROOT;
const sdk = process.env.K230_SDK_ROOT;
const ui = path.join(root, 'little/src/ui/src');
const chars = new Set();
for (const file of fs.readdirSync(ui)) {
    if (!file.endsWith('.c')) continue;
    const text = fs.readFileSync(path.join(ui, file), 'utf8');
    for (const ch of text.match(/[\u3400-\u9fff\u3000-\u303f\uff00-\uffef]/gu) || []) chars.add(ch);
}
const symbols = [...chars].sort().join('');
const font = path.join(sdk, 'output/k230_canmv_lckfb_defconfig/little/buildroot-ext/build/lawrec/thirdlib/lvgl/scripts/built_in_font/SimSun.woff');
for (const size of [16, 20]) {
    const name = `lawrec_font_cn_${size}.c`;
    const output = path.join(root, 'out/font-tools', name);
    const result = spawnSync(path.join(root, 'out/font-tools/node_modules/.bin/lv_font_conv'), [
        '--no-compress', '--no-prefilter', '--bpp', '4', '--size', String(size),
        '--font', font, '-r', '0x20-0x7F', '--symbols', symbols,
        '--format', 'lvgl', '-o', output, '--force-fast-kern-format'
    ], {stdio: 'inherit'});
    if (result.status !== 0) process.exit(result.status || 1);
    // Replace generated files atomically without requiring their previous owner.
    fs.chmodSync(output, 0o644);
    fs.renameSync(output, path.join(ui, name));
}
console.log(`Generated 16/20px UI subsets: ${chars.size} non-ASCII glyphs plus ASCII.`);
