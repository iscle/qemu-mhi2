#!/usr/bin/env python3
"""Exercise ZIP dependency preparation and source provenance without network."""
import configparser
import contextlib
import io
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import build_audi


def git(path, *args):
    return subprocess.check_output(['git', '-C', str(path), *args], text=True,
                                   stderr=subprocess.DEVNULL).strip()


class SourceBuildChecks(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root/'source-zip'
        self.source.mkdir()
        self.cache = self.root/'cache'
        upstream = self.root/'dependency'
        upstream.mkdir()
        git(upstream, 'init')
        git(upstream, 'config', 'user.name', 'Fixture')
        git(upstream, 'config', 'user.email', 'fixture@example.invalid')
        (upstream/'README').write_text('Pinned dependency\n')
        git(upstream, 'add', 'README')
        git(upstream, 'commit', '-m', 'Dependency fixture')
        self.revision = git(upstream, 'rev-parse', 'HEAD')
        subprojects = self.source/'subprojects'
        subprojects.mkdir()
        self.names = ('keycodemapdb', 'berkeley-softfloat-3', 'berkeley-testfloat-3')
        for name in self.names:
            overlay = subprojects/'packagefiles'/name
            overlay.mkdir(parents=True)
            (overlay/'meson.build').write_text('# Build overlay\n')
            spec = configparser.ConfigParser()
            spec['wrap-git'] = dict(url=upstream.as_uri(), revision=self.revision,
                                   patch_directory=name)
            with (subprojects/(name+'.wrap')).open('w') as stream:
                spec.write(stream)
        self.patch = patch.object(build_audi, 'REPO', self.source)
        self.patch.start()
        self.addCleanup(self.patch.stop)

    def prepare(self):
        with contextlib.redirect_stdout(io.StringIO()):
            build_audi.build_dependencies(self.cache)

    def test_zip_dependencies_are_pinned_and_reusable(self):
        self.prepare()
        self.prepare()
        self.assertFalse((self.source/'.git').exists())
        for name in self.names:
            destination = self.source/'subprojects'/name
            self.assertEqual((destination/'README').read_text(), 'Pinned dependency\n')
            self.assertEqual((destination/'meson.build').read_text(), '# Build overlay\n')
            self.assertFalse((destination/'.git').exists())
            self.assertEqual(git(self.cache/name, 'rev-parse', 'HEAD'), self.revision)
            self.assertEqual(git(self.cache/name, 'rev-parse', '--is-shallow-repository'), 'true')

    def test_existing_dependency_edits_are_not_overwritten(self):
        self.prepare()
        target = self.source/'subprojects/keycodemapdb/README'
        target.write_text('Local edit\n')
        with self.assertRaisesRegex(ValueError, 'differs from pinned source'):
            self.prepare()
        self.assertEqual(target.read_text(), 'Local edit\n')

    def test_modified_cache_is_rejected(self):
        self.prepare()
        (self.cache/'keycodemapdb/README').write_text('Modified cache\n')
        with self.assertRaisesRegex(ValueError, 'cache has local changes'):
            self.prepare()

    def test_archive_provenance_does_not_use_enclosing_git_repo(self):
        git(self.root, 'init')
        scripts = self.source/'tools/mhi2'
        scripts.mkdir(parents=True)
        version = scripts/'source-version.txt'
        with patch.object(build_audi, 'SCRIPTS', scripts):
            self.assertIsNone(build_audi.source_revision())
            version.write_text('$Format:%H$\n')
            self.assertIsNone(build_audi.source_revision())
            version.write_text(self.revision+'\n')
            self.assertEqual(build_audi.source_revision(), self.revision)


if __name__ == '__main__':
    unittest.main()
