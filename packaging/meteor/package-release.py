#!/usr/bin/env python3
"""Stage the portable Meteor host. Run inside its Ubuntu 22.04 builder."""
from pathlib import Path
import hashlib,json,os,re,shutil,subprocess,sys
ROOT=Path(__file__).resolve().parents[2]
build=ROOT/'build-meteor-release'
version=os.environ.get('METEOR_HOST_VERSION','0.1.0-preview.1')
if not re.fullmatch(r'[0-9A-Za-z.-]+',version):raise SystemExit('Invalid release version')
out=ROOT/'.build-cache/meteor-host-release'
stage=out/('Meteor-Vibepollo-'+version+'-linux-x86_64')
if stage.exists():shutil.rmtree(stage)
(stage/'bin').mkdir(parents=True);(stage/'lib').mkdir();(stage/'share/doc/meteor-vibepollo').mkdir(parents=True)
binary=build/'vibepollo'
if not binary.is_file():raise SystemExit('Build vibepollo first')
shutil.copy2(binary,stage/'bin/vibepollo')
# Relative asset paths in SUNSHINE_BUILD_METEOR resolve from the executable.
shutil.copytree(build/'assets',stage/'share/vibepollo')
wrapper=stage/'bin/meteor-vibepollo'
wrapper.write_text('#!/bin/sh\nset -eu\nROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"\nexec "$ROOT/bin/vibepollo" "$@"\n')
wrapper.chmod(0o755)
# Graphics and device libraries must come from the host driver stack. Never
# ship vendor drivers or a second glibc/loader in the portable package.
external=re.compile(r'^(?:ld-linux|lib(?:c\.|m\.|mvec\.|dl\.|pthread\.|rt\.|resolv\.|nss_|cuda\.|nvidia|GL|EGL|OpenGL|vulkan\.|va(?:-|\.)|drm|gbm\.|wayland|X11|xcb|pipewire|pulse|gio-|glib-|gobject-|gmodule-|systemd\.|udev\.|dbus-))')
queue=[binary];copied={};packages=set();requirements=set()
while queue:
 path=queue.pop()
 output=subprocess.check_output(['ldd',str(path)],text=True)
 if 'not found' in output:raise SystemExit('Unresolved library in '+str(path)+'\n'+output)
 for line in output.splitlines():
  match=re.match(r'\s*(\S+) => (/\S+)',line)
  if not match:continue
  name,source=match.groups()
  if external.match(name):requirements.add(name);continue
  if name in copied:continue
  src=Path(source).resolve();target=stage/'lib'/name
  shutil.copy2(src,target);copied[name]=src;queue.append(target)
  query=subprocess.run(['dpkg-query','-S',str(src)],text=True,capture_output=True)
  for row in query.stdout.splitlines():
   if ': ' in row:packages.add(row.split(': ',1)[0])
for path in [stage/'bin/vibepollo',*[stage/'lib'/name for name in copied]]:
 subprocess.run(['patchelf','--set-rpath','$ORIGIN/../lib' if path.parent.name=='bin' else '$ORIGIN',str(path)],check=True)
subprocess.run(['strip','--strip-unneeded',str(stage/'bin/vibepollo')],check=True)
doc=stage/'share/doc/meteor-vibepollo'
shutil.copy2(ROOT/'LICENSE',doc/'LICENSE')
shutil.copy2(ROOT/'METEOR_DEVELOPMENT.md',doc/'DEVELOPMENT_BASELINE.md')
license_dir=doc/'libraries';license_dir.mkdir()
versions=[]
for package in sorted(packages):
 copyright=Path('/usr/share/doc')/package.split(':')[0]/'copyright'
 if copyright.is_file():shutil.copy2(copyright,license_dir/(package.replace(':','-')+'-copyright.txt'))
 versions.append(subprocess.check_output(['dpkg-query','-W','-f','${Package}\t${Version}\n',package],text=True))
(doc/'baseline-packages.txt').write_text(''.join(versions))
(doc/'host-driver-libraries.txt').write_text('\n'.join(sorted(requirements))+'\n')
# Include license texts for the source dependencies linked into this build.
source_licenses=doc/'source-dependencies';source_licenses.mkdir()
for base in [ROOT/'third-party',build/'_deps/boost-src',build/'_deps/json-src']:
 if not base.exists():continue
 for path in base.rglob('*'):
  if path.is_file() and path.name.lower() in ['license','license.txt','license.md','copying','copying.txt','copying.lgplv2.1','copyright','notice','notice.txt','boost_software_license.txt']:
   if '.git' in path.parts or 'node_modules' in path.parts:continue
   relative=path.relative_to(ROOT) if path.is_relative_to(ROOT) else path.name
   target=source_licenses/str(relative).replace('/','__');shutil.copy2(path,target)
(stage/'README.txt').write_text('''Meteor-Vibepollo for Nightfall — Linux preview

This is the user-local host managed by Nightfall Meteor, not a system service.
Run bin/meteor-vibepollo through Meteor. No sudo, setcap, kernel module, display
reconfiguration or machine-service installation is performed by this archive.
Capture: X11/NvFBC, Wayland/KWin and portal; encoding: NVIDIA CUDA/NVENC,
VA-API and PyroWave, subject to the actual host driver/capture compatibility.
The complete capture/encode path still requires hardware acceptance testing.
A Vulkan video encoder backend is not included in this first package.

Requires glibc 2.35 or newer and host graphics/device libraries listed in
share/doc/meteor-vibepollo/host-driver-libraries.txt. Input permissions and a
working desktop/audio session are required. NVIDIA driver and AMD/Intel Mesa
packages are supplied by your distribution; this package does not install them.

Source, build scripts and corresponding source archive:
https://github.com/tB0nE/Vibepollo
Licenses and baseline dependency versions: share/doc/meteor-vibepollo/.
Do not use the older development CMake build's hardcoded assets or library paths.
''')
manifest={'name':'Meteor-Vibepollo for Nightfall','version':version,'source_commit':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
 'source_dirty':bool(subprocess.check_output(['git','status','--porcelain'],cwd=ROOT,text=True)),
 'platform':'linux-x86_64','glibc_baseline':'2.35','binary':'bin/meteor-vibepollo',
 'files':{str(p.relative_to(stage)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(stage.rglob('*')) if p.is_file()}}
(stage/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
# Prove every packaged ELF stays within the stated glibc baseline.
for p in [stage/'bin/vibepollo',*[stage/'lib'/name for name in copied]]:
 table=subprocess.check_output(['objdump','-T',str(p)],text=True)
 needed={tuple(map(int,s.split('.'))) for s in re.findall(r'GLIBC_([0-9.]+)',table)}
 if needed and max(needed)>(2,35):raise SystemExit('glibc baseline exceeded: '+str(p)+' '+str(max(needed)))
archive=out/(stage.name+'.tar.zst')
subprocess.run(['tar','--zstd','-cf',str(archive),'-C',str(stage),'.'],check=True)
digest=hashlib.sha256(archive.read_bytes()).hexdigest()
(archive.with_suffix(archive.suffix+'.sha256')).write_text(digest+'  '+archive.name+'\n')
print(json.dumps({'archive':str(archive),'size':archive.stat().st_size,'sha256':digest,'stage':str(stage)},indent=2))
