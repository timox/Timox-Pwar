const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const assert = require('node:assert/strict');
const manifest = require('./import-manifest.json');
for (const item of manifest.files) {
    if (!item.targetSha256) continue;
    const hash = crypto.createHash('sha256').update(fs.readFileSync(path.join(__dirname, item.target))).digest('hex');
    assert.equal(hash, item.targetSha256.toLowerCase(), item.target);
}
console.log(`Verified ${manifest.files.length} imported/maintained source entries.`);
