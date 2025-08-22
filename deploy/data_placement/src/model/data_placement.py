import math
import pickle
import random
import time
import psutil
import os.path
import pandas as pd
import pyomo.environ as po
import plotly.express as px
from typing import Dict, Generator, Literal, Tuple
from loguru import logger
from pyomo.opt import SolverStatus, TerminationCondition


class InfeasibleModel(Exception):
  pass

class SolverTimeout(Exception):
  pass

class SolverError(Exception):
  pass

class InvalidSolution(Exception):
  pass


class DataPlacementModel(object):

  def __init__(self, chain_table_type: Literal["EC", "CR"], num_nodes, group_size, num_groups=None, num_targets_per_disk=None, min_targets_per_disk=1, bibd_only=False, qlinearize=False, relax_lb=1, relax_ub=0):
    if num_targets_per_disk is None:
      num_nodes, num_groups, num_targets_per_disk, group_size = DataPlacementModel.find_params(num_nodes, group_size, min_r=min_targets_per_disk, bibd_only=bibd_only)
    self.chain_table_type = chain_table_type
    self.num_nodes = num_nodes
    self.group_size = group_size
    self.num_targets_per_disk = num_targets_per_disk
    self.num_groups = num_groups or self.num_targets_total // self.group_size
    self.bibd_only = bibd_only
    self.qlinearize = qlinearize
    self.relax_lb = relax_lb
    self.relax_ub = relax_ub

  def __repr__(self):
    v, b, r, k, λ = self.v, self.b, self.r, self.k, self.λ
    lb, ub = self.relax_lb, self.relax_ub
    return f"{self.__class__.__name__}-{v=},{b=},{r=},{k=},{λ=},{lb=},{ub=}"

  __str__ = __repr__

  @property
  def path_name(self):
    return str(self).translate(str.maketrans(' ,:=', '---_'))

  @property
  def v(self):
    return self.num_nodes

  @property
  def b(self):
    return self.num_groups

  @property
  def r(self):
    return self.num_targets_per_disk

  @property
  def k(self):
    return self.group_size

  @property
  def λ(self):
    return self.max_recovery_traffic_on_peer

  @property
  def num_targets_used(self):
    return self.num_groups * self.group_size

  @property
  def num_targets_total(self):
    return self.num_nodes * self.num_targets_per_disk

  @property
  def all_targets_used(self):
    return self.num_targets_used == self.num_targets_total

  @property
  def balanced_peer_traffic(self):
    return self.all_targets_used and self.sum_recovery_traffic_per_failure % (self.num_nodes-1) == 0

  @property
  def recovery_traffic_factor(self):
    return (self.group_size - 1) if self.chain_table_type == "EC" else 1

  @property
  def sum_recovery_traffic_per_failure(self):
    return self.num_targets_per_disk * self.recovery_traffic_factor

  @property
  def max_recovery_traffic_on_peer(self):
    return math.ceil(self.sum_recovery_traffic_per_failure / (self.num_nodes-1))

  @property
  def balanced_incomplete_block_design(self):
    return self.bibd_only and self.balanced_peer_traffic and self.relax_ub == 0

  @staticmethod
  def find_params(v, k, min_r=1, max_r=100, bibd_only=False):
    if bibd_only: min_r = max(min_r, k)
    for r in range(min_r, max_r):
      if v * r % k == 0 and r * (k - 1) >= v - 1:
        b = v * r // k
        if not bibd_only or r * (k - 1) % (v - 1) == 0:
          return v, b, r, k
    raise ValueError(f"cannot find valid params: {v=}, {k=}")

  def run(self, pyomo_solver=None, threads=psutil.cpu_count(logical=False), init_timelimit=1800, max_timelimit=3600*2, auto_relax=False, output_root="output", verbose=False, add_elapsed_time=None):
    init_relax_lb = self.relax_lb
    init_relax_ub = self.relax_ub
    timelimit = 0
    num_loops = self.max_recovery_traffic_on_peer*2
    os.makedirs(output_root, exist_ok=True)

    for loop in range(num_loops):
      try:
        logger.info(f"solving model with {pyomo_solver} #{loop}: {self}")
        if add_elapsed_time is not None:
          add_elapsed_time()
        timelimit = min(timelimit + init_timelimit, max_timelimit)
        instance = self.solve(pyomo_solver, threads, timelimit, output_root, verbose)
        if add_elapsed_time is not None:
          add_elapsed_time(f"solve model time (lb={self.relax_lb}, ub={self.relax_ub})")
      except (InfeasibleModel, SolverTimeout) as ex:
        logger.error(f"cannot find solution for current params: {ex}")
        if auto_relax:
          self.relax_lb = init_relax_lb + (loop+1) // 2
          self.relax_ub = init_relax_ub + (loop+2) // 2
          continue
        elif loop + 1 < num_loops:
          logger.critical(f"failed to find solution after {num_loops} attempts")
          raise ex
        else:
          raise ex
      else:
        output_path = os.path.join(output_root, self.path_name)
        os.makedirs(output_path, exist_ok=True)
        self.save_solution(instance, output_path)
        self.visualize_solution(instance, output_path)
        logger.success(f"saved solution to: {output_path}")
        return instance

  logger.catch(reraise=True, message="failed to solve model")
  def solve(self, pyomo_solver=None, threads=psutil.cpu_count(logical=False), timelimit=3600, output_path="output", verbose=False):
    if "highs" in pyomo_solver:
      self.qlinearize = True

    instance = self.build_model()
    if verbose: instance.pprint()

    try:
      results = self.solve_model(instance, pyomo_solver, threads, timelimit, output_path)
    except RuntimeError as ex:
      raise SolverError("unknown runtime error") from ex

    if (results.solver.status == SolverStatus.ok) and (results.solver.termination_condition == TerminationCondition.optimal):
        logger.success(f"optimal solution: {str(results.solver)}")
        if pyomo_solver is not None: instance.solutions.load_from(results)
    elif results.solver.termination_condition == TerminationCondition.infeasible:
        raise InfeasibleModel(f"infeasible: {str(results.solver)}")
    elif results.solver.termination_condition in (TerminationCondition.maxTimeLimit, TerminationCondition.maxIterations):
        raise SolverTimeout(f"timeout: {str(results.solver)}")
    else:
        raise SolverError(f"error: {str(results.solver)}")

    if verbose: self.print_solution(instance)
    try:
      self.check_solution(instance)
    except AssertionError as ex:
      raise InvalidSolution from ex
    return instance

  def build_model(self):
    logger.info(f"{self.num_nodes=} {self.num_targets_per_disk=} {self.group_size=} {self.num_groups=} {self.qlinearize=} {self.relax_lb=} {self.relax_ub=}")
    # v >= k
    assert self.num_nodes >= self.group_size, f"{self.num_nodes=} < {self.group_size=}"
    # Fisher's inequality
    if self.balanced_incomplete_block_design:
      # b >= v
      assert self.num_groups >= self.num_nodes, f"{self.num_groups=} < {self.num_nodes=}"
      # r >= k
      assert self.num_targets_per_disk >= self.group_size, f"{self.num_targets_per_disk=} < {self.group_size=}"

    logger.info(f"{self.sum_recovery_traffic_per_failure=} {self.max_recovery_traffic_on_peer=}")
    if self.sum_recovery_traffic_per_failure < self.num_nodes - 1:
      logger.warning(f"some disks do not share recovery traffic: {self.sum_recovery_traffic_per_failure=} < {self.num_nodes=} - 1")

    logger.info(f"{self.all_targets_used=} {self.balanced_peer_traffic=}")
    logger.info(f"{self.num_targets_used=} {self.num_targets_total=}")
    if self.num_targets_used < self.num_targets_total:
      logger.warning(f"some disks have unused targets: {self.num_targets_used=} < {self.num_targets_total=}")
    else:
      assert self.num_targets_used == self.num_targets_total, f"{self.num_targets_used=} > {self.num_targets_total=}"

    model = po.ConcreteModel()
    # index sets
    model.disks = po.RangeSet(1, self.num_nodes)
    model.target_idxs = po.RangeSet(1, self.num_targets_per_disk)
    model.targets = model.disks * model.target_idxs
    model.groups = po.RangeSet(1, self.num_groups)

    def disk_pairs_init(model):
      for disk in model.disks:
        for peer in model.disks:
          if peer > disk:
            yield (disk, peer)
    model.disk_pairs = po.Set(dimen=2, initialize=disk_pairs_init)

    # variables

    model.disk_used_by_group = po.Var(model.disks, model.groups, domain=po.Binary)
    if self.qlinearize:
      model.disk_in_same_group = po.Var(model.disk_pairs, model.groups, domain=po.Binary)

    # constraints

    def calc_disk_in_same_group(model, disk, peer, group):
      return model.disk_used_by_group[disk,group] * model.disk_used_by_group[peer,group]

    def define_disk_in_same_group_lower_bound(model, disk, peer, group):
      return model.disk_used_by_group[disk,group] + model.disk_used_by_group[peer,group] <= model.disk_in_same_group[disk,peer,group] + 1

    def define_disk_in_same_group_upper_bound1(model, disk, peer, group):
      return model.disk_in_same_group[disk,peer,group] <= model.disk_used_by_group[disk,group]

    def define_disk_in_same_group_upper_bound2(model, disk, peer, group):
      return model.disk_in_same_group[disk,peer,group] <= model.disk_used_by_group[peer,group]

    if self.qlinearize:
      model.define_disk_in_same_group_lower_bound_eqn = po.Constraint(model.disk_pairs, model.groups, rule=define_disk_in_same_group_lower_bound)
      model.define_disk_in_same_group_upper_bound1_eqn = po.Constraint(model.disk_pairs, model.groups, rule=define_disk_in_same_group_upper_bound1)
      model.define_disk_in_same_group_upper_bound2_eqn = po.Constraint(model.disk_pairs, model.groups, rule=define_disk_in_same_group_upper_bound2)

    def each_disk_has_limited_capcity(model, disk):
      if self.all_targets_used:
        return po.quicksum(model.disk_used_by_group[disk,group] for group in model.groups) == self.num_targets_per_disk
      else:
        return po.quicksum(model.disk_used_by_group[disk,group] for group in model.groups) <= self.num_targets_per_disk
    model.each_disk_has_limited_capcity_eqn = po.Constraint(model.disks, rule=each_disk_has_limited_capcity)

    def enough_disks_assigned_to_each_group(model, group):
      return po.quicksum(model.disk_used_by_group[disk,group] for disk in model.disks) == self.group_size
    model.enough_disks_assigned_to_each_group_eqn = po.Constraint(model.groups, rule=enough_disks_assigned_to_each_group)

    def calc_peer_recovery_traffic(model, disk, peer):
      if self.qlinearize:
        return po.quicksum(model.disk_in_same_group[disk,peer,group] for group in model.groups)
      else:
        return po.quicksum(calc_disk_in_same_group(model, disk, peer, group) for group in model.groups)

    def peer_recovery_traffic_upper_bound(model, disk, peer):
      if self.balanced_incomplete_block_design:
        return calc_peer_recovery_traffic(model, disk, peer) == self.max_recovery_traffic_on_peer
      else:
        return calc_peer_recovery_traffic(model, disk, peer) <= self.max_recovery_traffic_on_peer + self.relax_ub
    model.peer_recovery_traffic_upper_bound_eqn = po.Constraint(model.disk_pairs, rule=peer_recovery_traffic_upper_bound)

    def peer_recovery_traffic_lower_bound(model, disk, peer):
      return calc_peer_recovery_traffic(model, disk, peer) >= max(0, self.max_recovery_traffic_on_peer - self.relax_lb)

    if self.balanced_incomplete_block_design:
      logger.info(f"lower bound not needed for balanced incomplete block design (BIBD)")
    elif self.all_targets_used:
      logger.info(f"lower bound imposed on peer traffic: {self.relax_lb=} {self.qlinearize=} {self.all_targets_used=}")
      model.peer_recovery_traffic_lower_bound_eqn = po.Constraint(model.disk_pairs, rule=peer_recovery_traffic_lower_bound)
    else:
      logger.info(f"lower bound not imposed on peer traffic: {self.relax_lb=} {self.qlinearize=} {self.all_targets_used=}")

    def total_recovery_traffic(model):
      return po.summation(model.disk_in_same_group) * 2

    # model.obj = po.Objective(rule=total_recovery_traffic, sense=po.minimize)
    model.obj = po.Objective(expr=1)  # dummy objective
    return model

  def solve_model(self, instance, pyomo_solver, threads, timelimit, output_path):
    if pyomo_solver is not None:
      solver = po.SolverFactory(pyomo_solver)
      return solver.solve(instance, options={"threads": str(threads), "log_file": os.path.join(output_path, f"{pyomo_solver}.log")}, load_solutions=False, timelimit=timelimit, tee=True)
    else:
      raise ValueError(f"no solver specified")

  def get_peer_traffic(self, instance) -> Dict[Tuple[int,int], int]:
    peer_traffic_map = {}
    for disk in instance.disks:
      for peer in instance.disks:
        if disk == peer: continue
        peer_traffic_map[(disk, peer)] = sum(
          po.value(instance.disk_used_by_group[disk,group]) *
          po.value(instance.disk_used_by_group[peer,group])
          for group in instance.groups) * self.recovery_traffic_factor / (self.group_size - 1)
    return peer_traffic_map

  def get_incidence_matrix(self, instance) -> Dict[Tuple[int, int], bool]:
    incidence_matrix = {}
    for disk in instance.disks:
      for group in instance.groups:
        val = instance.disk_used_by_group[disk,group]
        if math.isclose(po.value(val), 1):
          incidence_matrix[(disk,group)] = True
    if self.all_targets_used:
      assert len(incidence_matrix) % self.num_nodes == 0, f"{len(incidence_matrix)=} % {self.num_nodes=}"
    assert len(incidence_matrix) % self.num_groups == 0, f"{len(incidence_matrix)=} % {self.num_groups=}"
    return incidence_matrix

  def check_solution(self, instance):
    has_peer_traffic_lower_bound = False
    for c in instance.component_objects(po.Constraint):
      if "peer_recovery_traffic_lower_bound_eqn" in str(c):
        has_peer_traffic_lower_bound = True

    peer_traffic_map = self.get_peer_traffic(instance)
    for (disk, peer), peer_traffic in peer_traffic_map.items():
      logger.debug(f"{disk},{peer}: {peer_traffic:.1f}")
      assert peer_traffic <= self.max_recovery_traffic_on_peer + self.relax_ub + 1e-5, f"{peer_traffic=} > {self.max_recovery_traffic_on_peer=} + {self.relax_ub}"
      if has_peer_traffic_lower_bound:
        assert peer_traffic >= max(0, self.max_recovery_traffic_on_peer - self.relax_lb) - 1e-5, f"{peer_traffic=} < {self.max_recovery_traffic_on_peer=} - {self.relax_lb}"

    min_peer_traffic = min(peer_traffic_map.values())
    max_peer_traffic = max(peer_traffic_map.values())
    total_traffic = sum(peer_traffic_map.values())
    max_total_traffic = self.num_nodes * self.sum_recovery_traffic_per_failure
    logger.info(f"{min_peer_traffic=:.1f} {max_peer_traffic=:.1f}")
    logger.info(f"{total_traffic=} {max_total_traffic=}")

    peer_traffic_diff = max_peer_traffic - min_peer_traffic
    if has_peer_traffic_lower_bound:
      assert peer_traffic_diff <= self.relax_ub + self.relax_lb + 1e-5, f"{peer_traffic_diff=}"
    if self.balanced_incomplete_block_design:
      assert math.isclose(peer_traffic_diff, 0.0, abs_tol=1e-9), f"{peer_traffic_diff=}"

    assert total_traffic <= max_total_traffic + 1e-5
    return total_traffic, min_peer_traffic, max_peer_traffic

  def print_solution(self, instance):
    for disk in instance.disks:
      for group in instance.groups:
        val = instance.disk_used_by_group[disk,group]
        if math.isclose(po.value(val), 1):
          logger.info(f"{val}: {po.value(val)}")

  def save_solution(self, instance, output_path: str="output"):
    incidence_matrix = self.get_incidence_matrix(instance)
    with open(os.path.join(output_path, "incidence_matrix.pickle"), "wb") as fout:
      pickle.dump(incidence_matrix, fout)

    peer_traffic_map = self.get_peer_traffic(instance)
    with open(os.path.join(output_path, "peer_traffic_map.pickle"), "wb") as fout:
      pickle.dump(peer_traffic_map, fout)

  def visualize_solution(self, instance, output_path: str="output", write_html=True):
    incidence_matrix = self.get_incidence_matrix(instance)
    disks, groups = zip(*incidence_matrix.keys())
    incidence_df = pd.DataFrame(zip(disks, groups), columns=["disk", "group"])

    peer_traffic_map = self.get_peer_traffic(instance)
    min_peer_traffic = min(peer_traffic_map.values())
    max_peer_traffic = max(peer_traffic_map.values())

    fig = px.scatter(
      incidence_df,
      x="disk",
      y="group",
      title=f"{self}, min/max peer traffic: {min_peer_traffic:.1f}/{max_peer_traffic:.1f}")
    fig.update_layout(
      xaxis_title="Nodes",
      yaxis_title="Groups",
      xaxis = dict(
        tickmode = 'array',
        tickvals = list(range(1, self.num_nodes+1)),
      ),
      yaxis = dict(
        tickmode = 'array',
        tickvals = list(range(1, self.num_groups+1)),
      ),
    )

    if write_html:
      fig.write_html(os.path.join(output_path, f"data_placement.html"), include_plotlyjs=True)
    return fig


class RebalanceTrafficModel(DataPlacementModel):

  def __init__(self, existing_incidence_matrix, chain_table_type: Literal["EC", "CR"], num_nodes, group_size, num_groups=None, num_targets_per_disk=None, min_targets_per_disk=1, bibd_only=False, qlinearize=False, relax_lb=1, relax_ub=0):
    self.existing_incidence_matrix = existing_incidence_matrix
    self.existing_disks, self.existing_groups = zip(*existing_incidence_matrix.keys())
    num_existing_targets_per_disk = math.ceil(self.total_existing_targets / self.num_existing_disk)
    min_targets_per_disk = max(min_targets_per_disk, num_existing_targets_per_disk)
    if num_targets_per_disk is None:
      num_nodes, num_groups, num_targets_per_disk, group_size = DataPlacementModel.find_params(num_nodes, group_size, min_r=min_targets_per_disk, bibd_only=bibd_only)
    else:
      assert num_targets_per_disk >= min_targets_per_disk
    super().__init__(chain_table_type, num_nodes, group_size, num_groups, num_targets_per_disk, min_targets_per_disk, bibd_only, qlinearize, relax_lb, relax_ub)

  @property
  def num_existing_disk(self):
    return max(self.existing_disks)

  @property
  def num_existing_groups(self):
    return max(self.existing_groups)

  @property
  def total_existing_targets(self):
    return len(self.existing_disks)

  @property
  def existing_group_size(self):
    assert self.total_existing_targets % self.num_existing_groups == 0, f"{self.total_existing_targets=} % {self.num_existing_groups=}"
    return self.total_existing_targets // self.num_existing_groups

  def build_model(self):
    max_existing_targets_per_disk = math.ceil(self.total_existing_targets / self.num_nodes)
    logger.info(f"{self.num_existing_disk=} {self.num_existing_groups=} {self.total_existing_targets=} {max_existing_targets_per_disk=}")

    assert self.num_nodes >= self.num_existing_disk, f"{self.num_nodes=} < {self.num_existing_disk=}"
    assert self.num_groups >= self.num_existing_groups, f"{self.num_groups=} < {self.num_existing_groups=}"
    assert self.group_size == self.existing_group_size, f"{self.group_size=} != {self.existing_group_size=}"
    assert self.num_targets_per_disk >= max_existing_targets_per_disk, f"{self.num_targets_per_disk=} >= {max_existing_targets_per_disk=}"

    model = super().build_model()

    def existing_targets_evenly_distributed_to_disks(model, disk):
      return po.quicksum(model.disk_used_by_group[disk,group] for group in model.groups if group <= self.num_existing_groups) <= max_existing_targets_per_disk
    model.existing_targets_evenly_distributed_to_disks_eqn = po.Constraint(model.disks, rule=existing_targets_evenly_distributed_to_disks)

    def num_existing_targets_not_moved(model):
      return po.quicksum(model.disk_used_by_group[disk,group] for disk in model.disks for group in model.groups if (disk,group) in self.existing_incidence_matrix)

    def total_rebalance_traffic(model):
      return self.total_existing_targets - num_existing_targets_not_moved(model)

    model.obj = po.Objective(expr=total_rebalance_traffic, sense=po.minimize)
    return model

  def visualize_solution(self, instance, output_path = "output", write_html=True):
    incidence_matrix = self.get_incidence_matrix(instance)
    disks, groups = zip(*incidence_matrix.keys())
    incidence_df = pd.DataFrame(zip(disks, groups, [g > self.num_existing_groups for g in groups]), columns=["disk", "group", "new"])

    peer_traffic_map = self.get_peer_traffic(instance)
    min_peer_traffic = min(peer_traffic_map.values())
    max_peer_traffic = max(peer_traffic_map.values())

    fig = px.scatter(
      incidence_df,
      x="disk",
      y="group",
      color="new",
      title=f"{self}, min/max peer traffic: {min_peer_traffic:.1f}/{max_peer_traffic:.1f}, rebalance traffic: {po.value(instance.obj.expr)}")
    fig.update_layout(
      xaxis_title="Nodes",
      yaxis_title="Groups",
      xaxis = dict(
        tickmode = 'array',
        tickvals = list(range(1, self.num_nodes+1)),
      ),
      yaxis = dict(
        tickmode = 'array',
        tickvals = list(range(1, self.num_groups+1)),
      ),
    )

    if write_html:
      fig.write_html(os.path.join(output_path, f"{self.path_name}.html"), include_plotlyjs=True)
    return fig


def main():
  import psutil
  import argparse

  parser = argparse.ArgumentParser(prog="model.py", description="3FS data placement")
  parser.add_argument("-pyomo", "--pyomo_solver", default="appsi_highs", choices=["appsi_highs", "cbc",  "scip"], help="Solver used by Pyomo")
  parser.add_argument("-type", "--chain_table_type",  type=str, required=True, choices=["CR", "EC"], help="CR - Chain Replication; EC - Erasure Coding")
  parser.add_argument("-j", "--solver_threads", type=int, default=psutil.cpu_count(logical=False)//2, help="Number of solver threads")
  parser.add_argument("-v", "--num_nodes", type=int, required=True, help="Number of storage nodes")
  parser.add_argument("-r", "--num_targets_per_disk", type=int, default=None, help="Number of storage targets on each disk")
  parser.add_argument("-min_r", "--min_targets_per_disk", type=int, default=1, help="Min number of storage targets on each disk")
  parser.add_argument("-k", "--replication_factor", "--group_size", dest="group_size", type=int, default=3, help="Replication factor or erasure coding group size")
  parser.add_argument("-b", "--num_groups", type=int, default=None, help="Number of chains or EC groups")
  parser.add_argument("-ql", "--qlinearize", action="store_true", help="Enable linearization of quadratic equations")
  parser.add_argument("-lb", "--relax_lb", type=int, default=1, help="Relax the lower bound of peer recovery traffic")
  parser.add_argument("-ub", "--relax_ub", type=int, default=0, help="Relax the upper bound of peer recovery traffic")
  parser.add_argument("-relax", "--auto_relax", action="store_true", help="Auto relax the lower/upper bound of peer recovery traffic when timeout")
  parser.add_argument("-bibd", "--bibd_only", action="store_true", help="Only create balanced incomplete block design (BIBD)")
  parser.add_argument("-t", "--init_timelimit", type=int, default=1800, help="Initial timeout for solver")
  parser.add_argument("-T", "--max_timelimit", type=int, default=3600*2, help="Max timeout for solver")
  parser.add_argument("-o", "--output_path", default="output", help="Path of output files")
  parser.add_argument("-m", "--existing_incidence_matrix", default=None, help="Existing incidence matrix for rebalance traffic model")
  parser.add_argument("-V", "--verbose", action="store_true", help="Show verbose output")
  args = parser.parse_args()

  if args.existing_incidence_matrix is None:
    DataPlacementModel(
      args.chain_table_type,
      args.num_nodes,
      args.group_size,
      args.num_groups,
      args.num_targets_per_disk,
      args.min_targets_per_disk,
      args.bibd_only,
      args.qlinearize,
      args.relax_lb,
      args.relax_ub,
    ).run(
      args.pyomo_solver,
      args.solver_threads,
      args.init_timelimit,
      args.max_timelimit,
      args.auto_relax,
      args.output_path,
      args.verbose)
  else:
    with open(args.existing_incidence_matrix, "rb") as fin:
      existing_incidence_matrix = pickle.load(fin)
    RebalanceTrafficModel(
      existing_incidence_matrix,
      args.chain_table_type,
      args.num_nodes,
      args.group_size,
      args.num_groups,
      args.num_targets_per_disk,
      args.min_targets_per_disk,
      args.bibd_only,
      args.qlinearize,
      args.relax_lb,
      args.relax_ub,
    ).run(
      args.pyomo_solver,
      args.solver_threads,
      args.init_timelimit,
      args.max_timelimit,
      args.auto_relax,
      args.output_path,
      args.verbose)


if __name__ == "__main__":
  main()
