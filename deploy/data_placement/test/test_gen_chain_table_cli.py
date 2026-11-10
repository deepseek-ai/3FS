import pathlib
import pickle
import subprocess
import sys
import tempfile
import unittest


SCRIPT = pathlib.Path(__file__).resolve().parents[1] / "src/setup/gen_chain_table.py"


class GenerateChainTableCliTests(unittest.TestCase):
  def setUp(self):
    self.temp = tempfile.TemporaryDirectory(prefix="3fs-chain-table-")
    self.addCleanup(self.temp.cleanup)
    self.root = pathlib.Path(self.temp.name)
    self.matrix = self.root / "incidence_matrix.pickle"
    # One three-replica chain, with one target on each of three nodes.
    with self.matrix.open("wb") as output:
      pickle.dump({(1, 1): True, (2, 1): True, (3, 1): True}, output)

  def generate(self, *extra_args):
    return subprocess.run(
        [sys.executable, "-I", str(SCRIPT),
         "--chain_table_type", "CR", "--node_id_begin", "1", "--node_id_end", "3",
         "--num_disks_per_node", "1", "--num_targets_per_disk", "1",
         "--target_id_prefix", "1", "--chain_id_prefix", "9",
         "--incidence_matrix_path", str(self.matrix), *map(str, extra_args)],
        cwd=self.root, capture_output=True, text=True, timeout=30)

  def assert_generated_files(self, directory):
    self.assertEqual(
        (directory / "generated_chains.csv").read_text(),
        "ChainId,TargetId,TargetId,TargetId\n"
        "900100001,100000100101,100000200101,100000300101\n")
    self.assertEqual(
        (directory / "generated_chain_table.csv").read_text(),
        "ChainId\n900100001\n")
    self.assertEqual(
        (directory / "create_target_cmd.txt").read_text(),
        "create-target --node-id 1 --disk-index 0 --target-id 100000100101 --chain-id 900100001  --use-new-chunk-engine\n"
        "create-target --node-id 2 --disk-index 0 --target-id 100000200101 --chain-id 900100001  --use-new-chunk-engine\n"
        "create-target --node-id 3 --disk-index 0 --target-id 100000300101 --chain-id 900100001  --use-new-chunk-engine\n")
    self.assertEqual(
        (directory / "remove_target_cmd.txt").read_text(),
        "offline-target --node-id 1 --target-id 100000100101\n"
        "remove-target --node-id 1 --target-id 100000100101\n"
        "offline-target --node-id 2 --target-id 100000200101\n"
        "remove-target --node-id 2 --target-id 100000200101\n"
        "offline-target --node-id 3 --target-id 100000300101\n"
        "remove-target --node-id 3 --target-id 100000300101\n")

  def test_creates_default_output_directory(self):
    result = self.generate()
    self.assertEqual(result.returncode, 0, result.stderr)
    self.assert_generated_files(self.root / "output")

  def test_creates_nested_output_directory(self):
    directory = self.root / "new" / "nested" / "output"
    result = self.generate("--output_path", directory)
    self.assertEqual(result.returncode, 0, result.stderr)
    self.assert_generated_files(directory)

  def test_existing_output_directory_is_supported(self):
    directory = self.root / "existing"
    directory.mkdir()
    unrelated = directory / "unrelated.txt"
    unrelated.write_text("keep me")
    result = self.generate("--output_path", directory)
    self.assertEqual(result.returncode, 0, result.stderr)
    self.assert_generated_files(directory)
    self.assertEqual(unrelated.read_text(), "keep me")

  def test_output_path_that_is_a_file_is_rejected(self):
    path = self.root / "file"
    path.write_text("keep me")
    result = self.generate("--output_path", path)
    self.assertNotEqual(result.returncode, 0)
    self.assertEqual(path.read_text(), "keep me")
    self.assertFalse((self.root / "generated_chains.csv").exists())
