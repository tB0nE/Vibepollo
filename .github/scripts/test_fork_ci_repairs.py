"""Exercise fork gating and PR source preparation without builds or network access."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

import yaml

ROOT = Path(__file__).resolve().parents[2]


def workflow(name):
    return yaml.load((ROOT / '.github/workflows' / name).read_text(), Loader=yaml.BaseLoader)


class ForkPagesTests(unittest.TestCase):
    def test_upstream_default_and_explicit_fork_opt_in(self):
        jobs = workflow('update-pages.yml')['jobs']
        condition = jobs['build']['if']
        for repository, enabled, expected in [
            ('Nonary/Vibepollo', '', True),
            ('tB0nE/Vibepollo', '', False),
            ('tB0nE/Vibepollo', 'false', False),
            ('tB0nE/Vibepollo', 'true', True),
        ]:
            with self.subTest(repository=repository, enabled=enabled):
                expression = condition.replace('github.repository', repr(repository))
                expression = expression.replace('vars.ENABLE_GITHUB_PAGES', repr(enabled)).replace('||', 'or')
                self.assertEqual(eval(expression, {'__builtins__': {}}), expected)
        self.assertEqual(jobs['deploy']['needs'], 'build')
        self.assertEqual(jobs['deploy']['permissions']['pages'], 'write')


@unittest.skipUnless(shutil.which('git') and shutil.which('bash'), 'Requires Git and Bash')
class ArchPrSourceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='fork-ci-source-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.checkout = self.root / 'checkout'
        self.checkout.mkdir()
        self.git('init', '-q', '-b', 'main')
        self.git('config', 'user.name', 'CI fixture')
        self.git('config', 'user.email', 'fixture@example.invalid')
        self.origin = 'https://github.com/tB0nE/Vibepollo.git'
        self.git('remote', 'add', 'origin', self.origin)
        (self.checkout / 'scripts').mkdir()
        shutil.copy2(ROOT / 'scripts/seed-linux-package-source.sh', self.checkout / 'scripts/seed-linux-package-source.sh')
        (self.checkout / 'base').write_text('base\n')
        self.git('add', '.')
        self.git('commit', '-qm', 'base')
        self.base = self.git('rev-parse', 'HEAD').strip()
        self.git('switch', '-qc', 'feature')
        (self.checkout / 'feature').write_text('feature\n')
        self.git('add', '.')
        self.git('commit', '-qm', 'feature')
        self.git('switch', '-q', 'main')
        (self.checkout / 'main').write_text('main\n')
        self.git('add', '.')
        self.git('commit', '-qm', 'main')
        self.git('merge', '-q', '--no-ff', 'feature', '-m', 'synthetic PR merge')
        self.commit = self.git('rev-parse', 'HEAD').strip()
        self.git('checkout', '-q', '--detach')
        self.git('branch', '-f', 'main', self.base)
        self.git('branch', '-D', 'feature')
        # HEAD alone holds the merge, as in an Actions PR checkout.
        self.assertFalse(self.git('for-each-ref', '--format=%(refname)', '--contains', self.commit, 'refs/heads').strip())
        self.pkg = self.root / 'pkg'
        self.pkg.mkdir()
        self.bin = self.root / 'bin'
        self.bin.mkdir()
        sudo = self.bin / 'sudo'
        sudo.write_text('#!/bin/bash\nif [[ "$1" == "-u" ]]; then shift 2; fi\nexec "$@"\n')
        sudo.chmod(0o755)
        text = next(s['run'] for s in workflow('ci-archlinux.yml')['jobs']['build_archlinux']['steps'] if s.get('name') == 'Build PKGBUILD')
        self.preparation = text[text.index('package_options=(-si)'):text.index('sudo -u builder env')]

    def git(self, *args):
        return subprocess.check_output(['git', '-C', str(self.checkout), *args], text=True, stderr=subprocess.DEVNULL)

    def prepare(self, event, validation=False, dirty=False):
        if dirty:
            (self.checkout / 'base').write_text('uncommitted\n')
        env = dict(os.environ, PATH=str(self.bin) + os.pathsep + os.environ['PATH'],
                   GITHUB_WORKSPACE=str(self.checkout), COMMIT=self.commit,
                   CLONE_URL=self.origin, VALIDATION_ONLY='true' if validation else 'false',
                   GITHUB_EVENT_NAME=event)
        return subprocess.run(['bash', '-euo', 'pipefail', '-c', self.preparation + '\nprintf "%s\\n" "${package_options[*]}"'],
                              cwd=self.pkg, env=env, capture_output=True, text=True)

    def test_pr_without_validation_flag_uses_exact_detached_source(self):
        result = self.prepare('pull_request')
        self.assertEqual(result.returncode, 0, result.stderr)
        mirror = self.pkg / 'vibepollo'
        actual = subprocess.check_output(['git', '--git-dir=' + str(mirror), 'rev-parse', 'HEAD'], text=True).strip()
        self.assertEqual(actual, self.commit)
        self.assertIn('-s --holdver', result.stdout)
        self.assertFalse((mirror / 'objects/info/alternates').exists())

    def test_dispatch_validation_still_seeds_source(self):
        result = self.prepare('workflow_dispatch', validation=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((self.pkg / 'vibepollo').is_dir())
        self.assertIn('-s --holdver', result.stdout)

    def test_regular_push_preserves_normal_packaging(self):
        result = self.prepare('push')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse((self.pkg / 'vibepollo').exists())
        self.assertEqual(result.stdout.strip(), '-si')

    def test_dirty_pr_source_is_rejected(self):
        result = self.prepare('pull_request', dirty=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.pkg / 'vibepollo').exists())


if __name__ == '__main__':
    unittest.main()
