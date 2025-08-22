# How to generate chain tables

Suppose we are going to setup a small 3FS cluster:
- 3 replicas for each chunk
- 5 storage nodes: `10001 ... 10005`
- 16 SSDs attached to each node
- 6 storage targets on each SSD

## Install dependencies

The data placement problem is formulated using [Pyomo](https://www.pyomo.org/) and solved with the [HiGHS](https://highs.dev/) solver. Install them and other dependencies:

```bash
$ cd deploy/data_placement
$ pip install -r requirements.txt
```

## Generate chain table

First generate a solution of the data placement problem.

```bash
$ cd deploy/data_placement
$ python src/model/data_placement.py -ql -relax -type CR --num_nodes 5 --replication_factor 3 --min_targets_per_disk 6 --init_timelimit 600

...

2025-02-24 14:25:13.623 | SUCCESS  | __main__:solve:165 - optimal solution: 
- Status: ok
  Termination condition: optimal
  Termination message: TerminationCondition.optimal

2025-02-24 14:25:13.624 | DEBUG    | __main__:check_solution:322 - 1,2: 1.5
2025-02-24 14:25:13.624 | DEBUG    | __main__:check_solution:322 - 1,3: 1.5
2025-02-24 14:25:13.624 | DEBUG    | __main__:check_solution:322 - 1,4: 1.5
2025-02-24 14:25:13.624 | DEBUG    | __main__:check_solution:322 - 1,5: 1.5
2025-02-24 14:25:13.624 | DEBUG    | __main__:check_solution:322 - 2,1: 1.5
2025-02-24 14:25:13.624 | DEBUG    | __main__:check_solution:322 - 2,3: 1.5
2025-02-24 14:25:13.624 | DEBUG    | __main__:check_solution:322 - 2,4: 1.5
2025-02-24 14:25:13.624 | DEBUG    | __main__:check_solution:322 - 2,5: 1.5
2025-02-24 14:25:13.624 | DEBUG    | __main__:check_solution:322 - 3,1: 1.5
2025-02-24 14:25:13.624 | DEBUG    | __main__:check_solution:322 - 3,2: 1.5
2025-02-24 14:25:13.624 | DEBUG    | __main__:check_solution:322 - 3,4: 1.5
2025-02-24 14:25:13.624 | DEBUG    | __main__:check_solution:322 - 3,5: 1.5
2025-02-24 14:25:13.624 | DEBUG    | __main__:check_solution:322 - 4,1: 1.5
2025-02-24 14:25:13.624 | DEBUG    | __main__:check_solution:322 - 4,2: 1.5
2025-02-24 14:25:13.624 | DEBUG    | __main__:check_solution:322 - 4,3: 1.5
2025-02-24 14:25:13.624 | DEBUG    | __main__:check_solution:322 - 4,5: 1.5
2025-02-24 14:25:13.624 | DEBUG    | __main__:check_solution:322 - 5,1: 1.5
2025-02-24 14:25:13.624 | DEBUG    | __main__:check_solution:322 - 5,2: 1.5
2025-02-24 14:25:13.624 | DEBUG    | __main__:check_solution:322 - 5,3: 1.5
2025-02-24 14:25:13.624 | DEBUG    | __main__:check_solution:322 - 5,4: 1.5
2025-02-24 14:25:13.624 | INFO     | __main__:check_solution:331 - min_peer_traffic=1.5 max_peer_traffic=1.5
2025-02-24 14:25:13.624 | INFO     | __main__:check_solution:332 - total_traffic=30.0 max_total_traffic=30
2025-02-24 14:25:14.147 | SUCCESS  | __main__:run:147 - saved solution to: output/DataPlacementModel-v_5-b_10-r_6-k_3-λ_2-lb_1-ub_1
```

Note that some combinations of `--num_nodes` and `--replication_factor` may have no solution.

Then generate commands to create/remove storage targets and the chain table.

```bash
$ python src/setup/gen_chain_table.py --chain_table_type CR --node_id_begin 10001 --node_id_end 10005 --num_disks_per_node 16 --num_targets_per_disk 6 --incidence_matrix_path output/DataPlacementModel-v_5-b_10-r_6-k_3-λ_2-lb_1-ub_1/incidence_matrix.pickle

$ ls -1 output/
DataPlacementModel-v_5-b_10-r_6-k_3-λ_2-lb_1-ub_1
appsi_highs.log
create_target_cmd.txt
generated_chain_table.csv
generated_chains.csv
remove_target_cmd.txt
```
