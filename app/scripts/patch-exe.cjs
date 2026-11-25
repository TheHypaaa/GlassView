// Sets the app icon and version info on the Windows Neutralino host before resources are embedded.
// Usage: node scripts/patch-exe.cjs <in.exe> <out.exe> <icon.ico> [version]   (default: neutralino.config.json)
const fs = require('fs');
const ResEdit = require('resedit');

const [, , input, output, iconPath, verArg] = process.argv;
const version = verArg || JSON.parse(fs.readFileSync(require('path').join(__dirname, '..', 'neutralino.config.json'), 'utf8')).version;
const [v1, v2, v3] = version.split('.').map((x) => parseInt(x, 10) || 0);
const exe = ResEdit.NtExecutable.from(fs.readFileSync(input), { ignoreCert: true });
const res = ResEdit.NtExecutableResource.from(exe);

const iconFile = ResEdit.Data.IconFile.from(fs.readFileSync(iconPath));
const existing = ResEdit.Resource.IconGroupEntry.fromEntries(res.entries);
const groupId = existing.length ? existing[0].id : 1;
const lang = existing.length ? existing[0].lang : 1033;
ResEdit.Resource.IconGroupEntry.replaceIconsForResource(
  res.entries, groupId, lang, iconFile.icons.map((i) => i.data)
);

const vi = ResEdit.Resource.VersionInfo.fromEntries(res.entries)[0] || ResEdit.Resource.VersionInfo.createEmpty();
vi.setFileVersion(v1, v2, v3, 0, 1033);
vi.setProductVersion(v1, v2, v3, 0, 1033);
vi.setStringValues({ lang: 1033, codepage: 1200 }, {
  FileDescription: 'GlassView',
  ProductName: 'GlassView',
  OriginalFilename: 'GlassView.exe',
  InternalName: 'GlassView',
  CompanyName: '',
  LegalCopyright: ''
});
vi.outputToResourceEntries(res.entries);

res.outputResource(exe);
fs.writeFileSync(output, Buffer.from(exe.generate()));
console.log('patched', output, 'version', version, 'icon group', groupId);
