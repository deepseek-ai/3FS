import copy
import glob
import os.path
import importlib
import shutil
import tempfile
import pytest
from src.model.data_placement import DataPlacementModel, RebalanceTrafficModel


placement_params = [
  # simple cases for replication group
  {
    "chain_table_type": "EC",
    "num_nodes": 5,
    "num_targets_per_disk": 6,
    "group_size": 2,
  },
  {
    "chain_table_type": "EC",
    "num_nodes": 5,
    "num_targets_per_disk": 6,
    "group_size": 3,
  },
  # not all targets used: num_nodes * num_targets_per_disk % group_size != 0
  {
    "chain_table_type": "EC",
    "num_nodes": 7,
    "num_targets_per_disk": 5,
    "group_size": 4,
  },
  # always evenly distributed: num_targets_per_disk * (group_size-1) % (num_nodes-1) == 0
  {
    "chain_table_type": "EC",
    "num_nodes": 8,
    "num_targets_per_disk": 6,
    "group_size": 5,
  },
  # all targets used & evenly distributed
  {
    "chain_table_type": "EC",
    "num_nodes": 10,
    "num_targets_per_disk": 9,
    "group_size": 5,
  },
]
qlinearize = [False, True]
relax_lb = [1, 2]

@pytest.mark.parametrize('qlinearize', qlinearize[1:])
@pytest.mark.parametrize('relax_lb', relax_lb)
@pytest.mark.parametrize('placement_params', placement_params)
@pytest.mark.skipif(importlib.util.find_spec("highspy") is None, reason="cannot find solver")
def test_solve_placement_model_with_highs(placement_params, qlinearize, relax_lb):
  DataPlacementModel(
    **placement_params,
    qlinearize=qlinearize,
    relax_lb=relax_lb,
  ).run(pyomo_solver="appsi_highs")

@pytest.mark.parametrize('chain_table_type, num_nodes, group_size', [("CR", 25, 3), ("EC", 25, 20)])
@pytest.mark.skipif(importlib.util.find_spec("highspy") is None, reason="cannot find solver")
def test_solve_placement_model_v25(chain_table_type, num_nodes, group_size):
  model = DataPlacementModel(
    chain_table_type=chain_table_type,
    num_nodes=num_nodes,
    group_size=group_size,
    qlinearize=True,
    relax_lb=1,
    relax_ub=1,
  )
  model.run(pyomo_solver="appsi_highs", max_timelimit=30, auto_relax=True)

@pytest.mark.parametrize('placement_params', placement_params)
@pytest.mark.skipif(importlib.util.find_spec("highspy") is None, reason="cannot find solver")
def test_solve_rebalance_model(placement_params):
  model = DataPlacementModel(
    **placement_params,
    qlinearize=True,
    relax_lb=1,
    relax_ub=1,
  )
  instance = model.run(pyomo_solver="appsi_highs")

  placement_params = copy.deepcopy(placement_params)
  placement_params["num_nodes"] *= 2
  placement_params.pop("num_targets_per_disk")
  RebalanceTrafficModel(
    existing_incidence_matrix=model.get_incidence_matrix(instance),
    **placement_params,
    qlinearize=True,
    relax_lb=2,
    relax_ub=1,
  ).run(pyomo_solver="appsi_highs", max_timelimit=15, auto_relax=True)
