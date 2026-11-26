"""Exercise diagnostic FAT mounts; no host block devices are opened."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class BootErrorMountTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.work = Path(self.temp.name)
        self.loader = self.work / 'mmcblk2p1'
        self.loader.touch()
        self.boot = self.work / 'boot'
        self.boot.mkdir()
        self.mountinfo = self.work / 'mountinfo'
        self.mountinfo.touch()
        sysfs = self.work / 'sys/mmcblk2p1'
        sysfs.mkdir(parents=True)
        (sysfs / 'dev').write_text('179:17\n')
        source = (ROOT / 'firstboot/15-nanokvm-firstboot').read_text()
        # Replace only block-node and mount-table fixtures, keeping real policy.
        self.source = self.work / 'firstboot'
        self.source.write_text(source.replace('[ -b "${loader:-}" ]',
                                              '[ -f "${loader:-}" ]')
                               .replace('/proc/self/mountinfo', str(self.mountinfo)))
        self.bin = self.work / 'bin'
        self.bin.mkdir()
        for name in ['mount', 'umount']:
            tool = self.bin / name
            tool.write_text('''#!/bin/sh
printf '%s %s\n' "${0##*/}" "$*" >>"$BOOT_ERROR_TRACE"
if [ "${0##*/}" = mount ] && [ "${BOOT_ERROR_MOUNT_FAIL:-0}" = 1 ]; then
    echo 'test FAT is unavailable' >&2
    exit 1
fi
''')
            tool.chmod(0o755)

    def run_save(self, mounted=None, fail=False):
        if mounted:
            self.mountinfo.write_text(
                f'42 1 179:17 / {self.boot} {mounted} - vfat /dev/alias {mounted}\n')
        return subprocess.run(['bash', '-eu', '-c', '''
. "$1"
loader=$2
migration_dir=$3
# Like /init, PATH is visible to the shell but absent from timeout's environment.
unset PATH
PATH=$BOOT_ERROR_TEST_PATH
nanokvm_boot_error_open
nanokvm_write_boot_error "$boot_error_mount" 'mount integration test'
nanokvm_boot_error_close
''', 'test', str(self.source), str(self.loader), str(self.work / 'migration')],
                              env=dict(os.environ,
                                       BOOT_ERROR_TEST_PATH=str(self.bin) + ':' + os.environ['PATH'],
                                       BOOT_ERROR_TRACE=str(self.work / 'trace'),
                                       ONEKVM_STORAGE_SYSFS=str(self.work / 'sys'),
                                       BOOT_ERROR_MOUNT_FAIL='1' if fail else '0'),
                              capture_output=True, text=True)

    def test_unmounted_fat_uses_resolved_mount_with_unexported_path(self):
        result = self.run_save()
        self.assertEqual(result.returncode, 0, result.stderr)
        trace = (self.work / 'trace').read_text().splitlines()
        self.assertTrue(trace[0].startswith('mount -t vfat '), trace)
        self.assertTrue(trace[1].startswith('umount '), trace)
        reason = self.work / 'migration/onekvm-boot-error-mnt/onekvm-boot-error/try-1/reason.txt'
        self.assertIn('reason: mount integration test', reason.read_text())

    def test_readonly_existing_fat_is_restored(self):
        result = self.run_save(mounted='ro,relatime')
        self.assertEqual(result.returncode, 0, result.stderr)
        trace = (self.work / 'trace').read_text().splitlines()
        self.assertEqual(trace, [f'mount -o remount,rw -- {self.boot}',
                                 f'mount -o remount,ro -- {self.boot}'])
        self.assertTrue((self.boot / 'onekvm-boot-error/try-1/reason.txt').is_file())

    def test_writable_existing_fat_needs_no_mount_changes(self):
        result = self.run_save(mounted='rw,relatime')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse((self.work / 'trace').exists())
        self.assertTrue((self.boot / 'onekvm-boot-error/try-1/reason.txt').is_file())

    def test_mount_failure_preserves_the_reason_on_stderr(self):
        result = self.run_save(fail=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('test FAT is unavailable', result.stderr)
        self.assertFalse((self.work / 'migration/onekvm-boot-error-mnt').exists())


if __name__ == '__main__':
    unittest.main()
