from click.testing import CliRunner

from hf3fs_utils.cli import cli


def test_rmtree_help():
    result = CliRunner().invoke(cli, ["rmtree", "--help"])
    assert result.exit_code == 0, result.output
    assert "--expire [1h|3h|8h|1d|3d|7d]" in result.output


def test_rmtree_invalid_expire(tmp_path):
    result = CliRunner().invoke(cli, ["rmtree", str(tmp_path), "--expire", "2h"])
    assert result.exit_code == 2, result.output
    assert "2h is invalid, valid options are [1h|3h|8h|1d|3d|7d]" in result.output
