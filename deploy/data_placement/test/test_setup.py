from collections import Counter
import glob
import os.path
import pytest

from src.model.data_placement import DataPlacementModel
from src.setup.gen_chain_table import generate_chains


@pytest.mark.parametrize('num_nodes, num_disks_per_node, num_targets_per_disk, num_replicas', [(5, 10, 6, 2), (10, 10, 9, 3)])
def test_generate_cr_chains(num_nodes: int, num_disks_per_node: int, num_targets_per_disk: int, num_replicas: int):
  model = DataPlacementModel(
    chain_table_type="CR",
    num_nodes=num_nodes,
    num_targets_per_disk=num_targets_per_disk,
    group_size=num_replicas,
    qlinearize=True,
    relax_lb=1,
    relax_ub=1,
  )
  instance = model.run(pyomo_solver="appsi_highs", max_timelimit=15, auto_relax=True)

  generate_chains(
    chain_table_type="CR",
    node_id_begin=1,
    node_id_end=num_nodes,
    num_disks_per_node=num_disks_per_node,
    num_targets_per_disk=num_targets_per_disk,
    target_id_prefix=1,
    chain_id_prefix=9,
    incidence_matrix=model.get_incidence_matrix(instance))


@pytest.mark.parametrize('num_nodes, num_disks_per_node, num_targets_per_disk, ec_group_size', [(20, 10, 6, 12), (25, 10, 12, 20)])
def test_generate_ec_chains(num_nodes: int, num_disks_per_node: int, num_targets_per_disk: int, ec_group_size: int):
  model = DataPlacementModel(
    chain_table_type="EC",
    num_nodes=num_nodes,
    num_targets_per_disk=num_targets_per_disk,
    group_size=ec_group_size,
    qlinearize=True,
    relax_lb=1,
    relax_ub=1,
  )
  instance = model.run(pyomo_solver="appsi_highs", max_timelimit=15, auto_relax=True)

  generate_chains(
    chain_table_type="EC",
    node_id_begin=1,
    node_id_end=num_nodes,
    num_disks_per_node=num_disks_per_node,
    num_targets_per_disk=num_targets_per_disk,
    target_id_prefix=1,
    chain_id_prefix=9,
    incidence_matrix=model.get_incidence_matrix(instance))
