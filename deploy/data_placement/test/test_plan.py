from smallpond.test_fabric import TestFabric
from src.model.data_placement_job import search_data_placement_plans

class TestPlan(TestFabric):

  def test_search_data_placement_plans(self):
    for pyomo_solver in ["appsi_highs"]:
      with self.subTest(pyomo_solver=pyomo_solver):
        plan = search_data_placement_plans(chain_table_type="EC", num_nodes=[10], group_size=[5, 9], solver_threads=16, pyomo_solver=pyomo_solver)
        self.execute_plan(plan, num_executors=1)
