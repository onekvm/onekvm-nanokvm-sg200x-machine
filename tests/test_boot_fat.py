"""Exercise boot FAT policy without mounting or modifying a host device."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class BootFatTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.work = Path(self.temp.name)
        self.loader = self.work / 'boot.img'
        self.disk = self.work / 'disk'
        self.loader.touch()
        self.disk.touch()
        self.mountinfo = self.work / 'mountinfo'
        self.mountinfo.touch()
        for name, dev in [('boot.img', '179:1'), ('disk', '179:0')]:
            path = self.work / 'sys' / name
            path.mkdir(parents=True)
            (path / 'dev').write_text(dev + '\n')
        # Only replace the block-node/mount fixtures; execute the actual policy.
        self.source = self.work / 'boot-fat.sh'
        self.source.write_text((ROOT / 'firstboot/boot-fat.sh').read_text()
                               .replace('[ -b "$loader" ]', '[ -f "$loader" ]')
                               .replace('[ -b "$disk" ]', '[ -f "$disk" ]')
                               .replace('/sys/class/block', str(self.work / 'sys'))
                               .replace('/proc/self/mountinfo', str(self.mountinfo)))
        self.bin = self.work / 'bin'
        self.bin.mkdir()
        checker = self.bin / 'fsck.fat'
        checker.write_text('''#!/usr/bin/env python3
import os, pathlib, sys
p = pathlib.Path(os.environ['FAT_TRACE'])
calls = p.read_text().splitlines() if p.exists() else []
p.write_text('\\n'.join(calls + [' '.join(sys.argv[1:])]) + '\\n')
raise SystemExit(int(os.environ['FAT_RESULTS'].split(',')[len(calls)]))
''')
        checker.chmod(0o755)

    def run_check(self, results='0', repeat=False):
        return subprocess.run(['sh', '-eu', '-c', '''
. "$1"
# Like /init, the shell knows PATH but has not exported it to timeout.
unset PATH
PATH=$FAT_TEST_PATH
loader=$2; disk=$3
onekvm_boot_fat_check "$4"
if [ "$5" = repeat ]; then onekvm_boot_fat_check "$4"; fi
''', 'test', str(self.source), str(self.loader), str(self.disk),
                               str(self.work / 'state'), 'repeat' if repeat else 'once'],
                              env=dict(os.environ, PATH=str(self.bin) + ':' + os.environ['PATH'],
                                       FAT_TEST_PATH=str(self.bin) + ':' + os.environ['PATH'],
                                       FAT_TRACE=str(self.work / 'trace'), FAT_RESULTS=results),
                              capture_output=True, text=True)

    def assert_result(self, result, calls):
        self.assertEqual((self.work / 'state/status').read_text().strip(), result)
        path = self.work / 'trace'
        actual = [line.split()[0] for line in path.read_text().splitlines()] if path.exists() else []
        self.assertEqual(actual, calls)

    def test_clean_is_read_only_and_checked_once(self):
        self.assertEqual(self.run_check(repeat=True).returncode, 0)
        self.assert_result('clean', ['-n'])

    def test_repair_once_and_verify(self):
        self.assertEqual(self.run_check('1,1,0').returncode, 0)
        self.assert_result('repaired', ['-n', '-a', '-n'])

    def test_failures_continue_without_repeated_repairs(self):
        for results, calls in [('2', ['-n']), ('124', ['-n']),
                               ('1,2', ['-n', '-a']), ('1,1,1', ['-n', '-a', '-n'])]:
            with self.subTest(results=results):
                shutil.rmtree(self.work / 'state', ignore_errors=True)
                (self.work / 'trace').unlink(missing_ok=True)
                self.assertEqual(self.run_check(results, repeat=True).returncode, 0)
                self.assert_result('failed', calls)

    def test_mounted_partition_alias_or_disk_is_never_checked(self):
        for dev in ['179:1', '179:0']:
            with self.subTest(dev=dev):
                shutil.rmtree(self.work / 'state', ignore_errors=True)
                self.mountinfo.write_text(f'42 1 {dev} / /boot rw - vfat /dev/alias rw\n')
                self.assertEqual(self.run_check().returncode, 0)
                self.assert_result('failed', [])

    @unittest.skipUnless(os.environ.get('FAT_REAL_MKFS') and os.environ.get('FAT_REAL_FSCK'),
                         'set FAT_REAL_MKFS and FAT_REAL_FSCK to dosfstools executables')
    def test_real_orphan_cluster_is_repaired_and_clean_image_is_unchanged(self):
        self.loader.write_bytes(bytes(16 * 1024 * 1024))
        subprocess.run([os.environ['FAT_REAL_MKFS'], '-F', '16', str(self.loader)], check=True,
                       stdout=subprocess.DEVNULL)
        checker = self.bin / 'fsck.fat'
        checker.unlink()
        checker.symlink_to(os.environ['FAT_REAL_FSCK'])
        before = self.loader.read_bytes()
        self.assertEqual(self.run_check().returncode, 0)
        self.assertEqual(self.loader.read_bytes(), before)
        self.assert_result('clean', [])
        shutil.rmtree(self.work / 'state')
        # Allocate an unreferenced cluster in both FAT16 copies.
        damaged = bytearray(before)
        sector = int.from_bytes(damaged[11:13], 'little')
        reserved = int.from_bytes(damaged[14:16], 'little')
        fat_size = int.from_bytes(damaged[22:24], 'little')
        for copy in range(damaged[16]):
            offset = (reserved + copy * fat_size) * sector + 3 * 2
            damaged[offset:offset + 2] = b'\xff\xff'
        self.loader.write_bytes(damaged)
        self.assertEqual(self.run_check().returncode, 0)
        self.assert_result('repaired', [])
        subprocess.run([os.environ['FAT_REAL_FSCK'], '-n', str(self.loader)], check=True,
                       stdout=subprocess.DEVNULL)


if __name__ == '__main__':
    unittest.main()
