import importlib.util
from pathlib import Path
import unittest
from unittest.mock import Mock

spec=importlib.util.spec_from_file_location('publisher',Path(__file__).with_name('release_firmware.py'))
publisher=importlib.util.module_from_spec(spec);spec.loader.exec_module(publisher)


class PreflightTests(unittest.TestCase):
    def test_local_preflight_needs_no_remote_branch(self):
        lookup=Mock(side_effect=RuntimeError('422 branch not published yet'))
        publisher.check_source_revision('local','branch',lookup,verify_only=True)
        lookup.assert_not_called()

    def test_publication_requires_exact_remote_revision(self):
        lookup=Mock(return_value={'sha':'local'})
        publisher.check_source_revision('local','branch',lookup)
        lookup.assert_called_once_with('branch')
        with self.assertRaises(RuntimeError):publisher.check_source_revision('local','branch',lambda _:dict(sha='other'))


if __name__=='__main__':unittest.main()
