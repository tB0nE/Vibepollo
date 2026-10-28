#!/usr/bin/env python3
"""Archive actual host sources and initialized dependencies, excluding local state."""
from pathlib import Path
import hashlib,json,os,subprocess,tarfile,io
ROOT=Path(__file__).resolve().parents[2]
version=os.environ.get('METEOR_HOST_VERSION','0.1.0-preview.1')
out=ROOT/'.build-cache/meteor-host-release';out.mkdir(parents=True,exist_ok=True)
archive=out/f'Meteor-Vibepollo-{version}-sources.tar.zst'
files=[];revisions={};omitted=[]
def collect(repo,prefix):
 revisions[str(prefix)]=subprocess.check_output(['git','rev-parse','HEAD'],cwd=repo,text=True).strip()
 raw=subprocess.check_output(['git','ls-files','--stage','-z'],cwd=repo)
 for row in raw.split(b'\0'):
  if not row:continue
  meta,name=row.split(b'\t',1);name=os.fsdecode(name);path=repo/name;target=prefix/name
  if meta.startswith(b'160000 '):
   if (path/'.git').exists():collect(path,target)
   else:omitted.append({'path':str(target),'revision':meta.decode().split()[1]})
  elif path.is_file() or path.is_symlink():files.append((path,str(target)))
 # Only our new release scripts may be untracked; never include arbitrary local files.
 if repo==ROOT:
  for row in subprocess.check_output(['git','ls-files','--others','--exclude-standard','-z','--','packaging/meteor'],cwd=repo).split(b'\0'):
   if row:
    name=os.fsdecode(row);files.append((repo/name,str(prefix/name)))
collect(ROOT,Path('Meteor-Vibepollo-source'))
manifest={'source_revision':revisions['Meteor-Vibepollo-source'],'repositories':revisions,
 'omitted_optional_submodules':omitted,'files':{},'build_inputs':{}}
with archive.open('wb') as output:
 process=subprocess.Popen(['zstd','-T2','-6','-c'],stdin=subprocess.PIPE,stdout=output)
 with tarfile.open(fileobj=process.stdin,mode='w|') as tar:
  for path,target in files:
   tar.add(path,arcname=target,recursive=False)
   if path.is_file() and not path.is_symlink():manifest['files'][target]=hashlib.sha256(path.read_bytes()).hexdigest()
  for name in ['boost','json']:
   source=ROOT/'build-meteor-release/_deps'/f'{name}-src'
   if not source.is_dir():raise RuntimeError('Missing build input '+str(source))
   def filter_source(info):
    if any(p in ['.git','node_modules','__pycache__'] for p in Path(info.name).parts):return None
    return info
   tar.add(source,arcname=f'Meteor-Vibepollo-source/build-inputs/{name}',filter=filter_source)
   manifest['build_inputs'][name]='Included full FetchContent source tree; version/hash pinned in CMake.'
  data=(json.dumps(manifest,indent=2)+'\n').encode();info=tarfile.TarInfo('Meteor-Vibepollo-source/SOURCE_MANIFEST.json');info.size=len(data)
  tar.addfile(info,io.BytesIO(data))
 process.stdin.close()
 if process.wait():raise RuntimeError('Source compression failed')
with archive.open('rb') as f:digest=hashlib.file_digest(f,'sha256').hexdigest()
(archive.with_suffix(archive.suffix+'.sha256')).write_text(digest+'  '+archive.name+'\n')
print(json.dumps({'archive':str(archive),'size':archive.stat().st_size,'sha256':digest,'source_files':len(files),'repositories':len(revisions)},indent=2))
