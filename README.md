# Hybrid Cloud Social Network Analyzer

A distributed Social Network Analysis (SNA) system that intelligently splits large graph computations across two machines — **Baadal VM (IIT Delhi)** and **GCP VM (us-central1)** — connected via a secure **Tailscale VPN** overlay network. The system automatically detects if a graph is too large for local memory and bursts the computation to the cloud, using clever batch message transfers to minimize network latency.

---

## Table of Contents

- [What This Project Does](#what-this-project-does)
- [System Architecture](#system-architecture)
- [How the Two Machines Communicate](#how-the-two-machines-communicate)
- [The Latency Problem and How We Solved It](#the-latency-problem-and-how-we-solved-it)
- [File Structure](#file-structure)
- [How Each Component Works](#how-each-component-works)
  - [controller.py](#1-controllerpy--the-brain)
  - [ingest.cpp](#2-ingestcpp--the-partitioner)
  - [worker.cpp](#3-workercpp--distributed-bfs)
  - [degree.cpp](#4-degreecpp--distributed-degree-centrality)
  - [run_hybrid.sh](#5-run_hybridsh--the-pipeline)
- [Graph Data Format](#graph-data-format)
- [Datasets](#datasets)
- [Prerequisites](#prerequisites)
- [Installation & Setup](#installation--setup)
- [Running the Project](#running-the-project)
- [Understanding the Output](#understanding-the-output)
- [Key Concepts Explained](#key-concepts-explained)

---

## What This Project Does

This system takes a graph (social network) as input and computes:

1. **Breadth First Search (BFS)** — finds the shortest path distance from a source node to every other node in the graph
2. **Degree Centrality** — finds the most connected nodes in the graph (who has the most connections?)

What makes it special is **hybrid cloud execution**:
- The controller first checks if the graph fits in Baadal's RAM
- If it does → runs everything locally on Baadal
- If it doesn't → automatically splits the graph and bursts part of the work to GCP

All of this happens with a **single command**.

---

## System Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                        run_hybrid.sh                        │
│                    (Orchestration Layer)                     │
└─────────────────┬───────────────────────────────────────────┘
                  │
                  ▼
┌─────────────────────────────────────────────────────────────┐
│                       controller.py                         │
│         Scans graph → estimates RAM → decides split %       │
└─────────────────┬───────────────────────────────────────────┘
                  │
                  ▼
┌─────────────────────────────────────────────────────────────┐
│                        ingest.cpp                           │
│    Partitions edges → baadal_partition.txt + gcp_partition  │
└────────────┬────────────────────────────────────────────────┘
             │
     ┌───────┴────────┐
     ▼                ▼
┌─────────┐      ┌─────────┐
│  Baadal │      │   GCP   │       ← Connected via Tailscale VPN
│ Worker 1│◄────►│ Worker 2│       ← Communicate via Redis (batch transfers)
│ (BFS /  │      │ (BFS /  │
│ Degree) │      │ Degree) │
└─────────┘      └─────────┘
```

---

## How the Two Machines Communicate

- **Network Layer**: [Tailscale](https://tailscale.com/) VPN creates a private mesh network between Baadal and GCP. Both machines get stable Tailscale IPs that work regardless of firewalls or NAT.
- **Message Broker**: [Redis](https://redis.io/) running on the GCP VM acts as the shared message bus. Both workers push/pull messages to/from Redis queues.
- **File Transfer**: `scp` over Tailscale is used to copy the GCP partition file from Baadal to GCP before computation starts.
- **Remote Execution**: `ssh` over Tailscale is used by `run_hybrid.sh` to start workers on GCP remotely.

```
Baadal VM (100.98.36.23)  ←──Tailscale──►  GCP VM (100.98.11.34)
                                                  │
                                             Redis :6379
                                          (message broker)
```

---

## The Latency Problem and How We Solved It

This is one of the most important design decisions in the project.

### The Problem

Baadal (IIT Delhi) and GCP (us-central1) are physically far apart. Every message that travels over Tailscale between the two machines incurs a **network round trip latency cost**. In a naive distributed system, you would send one Redis message per node — meaning for a graph with 50,000 nodes, you would have **50,000 individual Tailscale round trips**. This would make the system unbearably slow over a WAN connection.

### The Solution — Batch Transfers with Local Processing

We solve this with two techniques applied across both algorithms:

**1. Bulk LPUSH — Send Everything in One Shot (Degree Centrality)**

Instead of sending one message per node, we build the entire local degree map in memory first, then push all messages to Redis in a **single bulk LPUSH command**:

```
Naive approach:    LPUSH queue node1:degree1   ← 1 round trip
                   LPUSH queue node2:degree2   ← 1 round trip
                   LPUSH queue node3:degree3   ← 1 round trip
                   ... (N round trips total for N nodes)

Our approach:      LPUSH queue node1:deg1 node2:deg2 node3:deg3 ...
                                                     ← 1 round trip total
```

**2. LRANGE — Pull Everything Locally at Once (Both BFS and Degree)**

The receiving worker fetches all pending messages in one single `LRANGE 0 -1` command — pulling the entire dataset into local memory at once. All processing then happens locally without any further network calls:

```
Naive approach:    RPOP queue  → process  ← 1 round trip
                   RPOP queue  → process  ← 1 round trip
                   RPOP queue  → process  ← 1 round trip
                   ... (N round trips to drain the queue)

Our approach:      LRANGE queue 0 -1      ← 1 round trip, get everything
                   process locally in memory (zero network calls)
```

**The result:** No matter if the graph has 1,000 or 1,000,000 nodes, the entire degree exchange always costs exactly **2 Tailscale round trips** (one bulk push + one bulk pull) instead of 2N. For BFS, each superstep costs only **one Tailscale round trip** for reading all cross-partition messages, regardless of how many were sent.

This design — accumulate remotely, fetch once, process locally — is what makes the system practical over a geographically distributed WAN connection.

---

## File Structure

```
hybrid_sna/
│
├── controller.py          # Analyzes graph size, decides split percentage
├── ingest.cpp             # Partitions graph into two files based on split %
├── ingest                 # Compiled binary of ingest.cpp
├── worker.cpp             # Distributed BFS using BSP model over Redis
├── worker                 # Compiled binary of worker.cpp
├── degree.cpp             # Distributed Degree Centrality over Redis
├── degree                 # Compiled binary of degree.cpp
├── run_hybrid.sh          # Master pipeline script — runs everything
├── proxy.sh               # IITD network proxy login keepalive script
│
├── sample_graph.txt       # Small test graph
├── medium_graph.txt       # Medium sized graph
├── large_graph.txt        # Large graph for cloud burst testing
├── facebook_clean.txt     # Real Facebook social network dataset
├── facebook_combined.txt  # Combined Facebook dataset
├── youtube_clean.txt      # Real YouTube social network dataset
│
├── baadal_partition.txt   # Generated: edges assigned to Baadal worker
└── gcp_partition.txt      # Generated: edges assigned to GCP worker
```

---

## How Each Component Works

### 1. `controller.py` — The Brain

This is the decision-making component. It runs before anything else.

**What it does:**
- Scans the entire graph file in O(N) time
- Counts the number of unique vertices (V) and edges (E)
- Estimates the RAM needed to hold this graph in C++ data structures:
  - `unordered_map` overhead: ~32 bytes per vertex
  - `vector` overhead: ~4 bytes per edge
  - Safety multiplier of 3x (because C++ vectors double capacity dynamically)
- Checks how much RAM is currently free on Baadal (via `psutil`)
- Reserves 20% of available RAM for the OS and Redis
- **Decision**:
  - If estimated RAM ≤ safe available RAM → local execution, no bursting needed (split = 100%)
  - If estimated RAM > safe available RAM → triggers hybrid cloud burst, calculates exact split %

**Formula:**
```
estimated_bytes = (V * 32 + E * 4) * 3.0
baadal_percentage = (safe_available_bytes / estimated_bytes) * 100
```

**Output:** Prints the split percentage in a format that `run_hybrid.sh` parses automatically using `grep` and `grep -oP`.

---

### 2. `ingest.cpp` — The Partitioner

Takes the full graph and splits it into two partition files.

**What it does:**
- Reads every edge `u v` from the dataset
- For each edge, hashes the source vertex `u` using a multiplicative hash:
  ```cpp
  unsigned int mixed_hash = (unsigned int)u * 2654435761;
  int hash_val = mixed_hash % 100;
  ```
- If `hash_val < baadal_percentage` → writes edge to `baadal_partition.txt`
- Otherwise → writes edge to `gcp_partition.txt`

**Key property:** Every edge appears in exactly one partition file — no duplication. The same hash function is used consistently in `worker.cpp` and `degree.cpp` to ensure all three components always agree on which worker owns which node.

**Usage:**
```bash
./ingest <graph_file> <baadal_percentage>
# Example:
./ingest large_graph.txt 65
```

---

### 3. `worker.cpp` — Distributed BFS

Implements Breadth First Search across two machines using the **Bulk Synchronous Parallel (BSP)** computation model.

**BSP Model — 4 phases per superstep:**

```
┌─────────────────────────────────────────────────┐
│  SUPERSTEP N                                    │
│                                                 │
│  Phase 1: COMPUTE                               │
│    Each worker explores its local frontier      │
│    Local neighbors → add to next frontier       │
│    Remote neighbors → push to Redis queue       │
│                                                 │
│  Phase 2: BARRIER SYNC                          │
│    Both workers INCR a shared Redis counter     │
│    Wait until counter == 2 (both arrived)       │
│                                                 │
│  Phase 3: READ MESSAGES (Batch Local Pull)      │
│    LRANGE pulls ALL pending messages at once    │
│    Entire batch pulled into local memory        │
│    Processed locally — zero further network     │
│    calls during message processing              │
│                                                 │
│  Phase 4: TERMINATION CHECK                     │
│    If neither worker did any work → STOP        │
│    Otherwise → advance to next superstep        │
└─────────────────────────────────────────────────┘
```

**Node ownership** — same hash as `ingest.cpp`:
```cpp
unsigned int mixed_hash = (unsigned int)vertex_id * 2654435761;
int percentage = mixed_hash % 100;
// Worker 1 (Baadal) owns nodes where percentage < BAADAL_PERCENTAGE
// Worker 2 (GCP)    owns nodes where percentage >= BAADAL_PERCENTAGE
```

**Redis queues used:**
- `queue_baadal` — messages sent to Baadal worker
- `queue_gcp` — messages sent to GCP worker
- `sync_step_N` — barrier counter for superstep N
- `active_step_N` — activity counter for termination check

**Usage:**
```bash
./worker <worker_id> <partition_file> <baadal_percentage>
# Worker 1 on Baadal:
./worker 1 baadal_partition.txt 70
# Worker 2 on GCP:
./worker 2 gcp_partition.txt 70
```

BFS always starts from source node 1. Worker 1 connects to Redis on GCP (`100.98.11.34:6379`) and clears all state before starting.

---

### 4. `degree.cpp` — Distributed Degree Centrality

Computes the degree (number of connections) of every node across both machines and merges results with minimal network overhead.

**Phase 1 — Ready Barrier:**
Both workers connect to Redis and increment a `degree_ready` counter. Neither proceeds until both have checked in (counter == 2). This ensures no worker starts computing before the other is ready.

**Phase 2 — Local Degree Count:**
Each worker reads its own partition file and counts degrees for both endpoints of every edge:
```
For edge u-v:
  local_degree[u]++
  local_degree[v]++
```
This works because `ingest.cpp` guarantees each edge is in exactly one partition — so no edge is double-counted locally. Cross-partition nodes (nodes whose edges are split across both machines) will have incomplete counts at this stage, corrected in Phase 4.

**Phase 3 — Batch Exchange (Key Latency Optimization):**
All local degree counts are packed into a single bulk `LPUSH` command and sent in one Tailscale round trip:
```cpp
// Build entire message list in memory first
for (auto& pair : local_degree) {
    msg_strs.push_back(to_string(pair.first) + ":" + to_string(pair.second));
}
// Send everything in ONE Redis command = ONE Tailscale round trip
redisCommandArgv(redis, argv_r.size(), argv_r.data(), argvlen.data());
```
A done signal is then set so the other worker knows the transfer is complete.

**Phase 4 — Batch Local Pull and Merge:**
Each worker fetches the other's entire degree map in one single `LRANGE 0 -1` command — pulling everything into local memory at once, then merging with zero further network calls:
```cpp
// ONE Tailscale round trip to get all remote data
redisCommand(redis, "LRANGE %s 0 -1", my_queue.c_str());

// Process entirely in local memory — zero network calls
for each message:
    local_degree[node] += received_degree;
```
Total Tailscale cost for the entire degree exchange = **2 round trips**, regardless of graph size.

**Phase 5 — Results (Worker 1 only):**
Worker 1 sorts all nodes by degree descending and prints the top 20 most connected nodes with normalized degree centrality, plus max, min, and average degree statistics.

**Normalized degree centrality formula:**
```
normalized_degree(v) = degree(v) / (N - 1)
```
Where N is the total number of nodes. This gives a value between 0 and 1.

**Usage:**
```bash
./degree <worker_id> <partition_file> <baadal_percentage>
# Worker 1 on Baadal:
./degree 1 baadal_partition.txt 70
# Worker 2 on GCP:
./degree 2 gcp_partition.txt 70
```

---

### 5. `run_hybrid.sh` — The Pipeline

The single command that orchestrates everything end to end.

**What it does step by step:**

```
Step 1:  Validate input graph file exists
Step 2:  Run controller.py → extract SPLIT percentage
Step 3:  Run ingest → create baadal_partition.txt + gcp_partition.txt
Step 4:  scp gcp_partition.txt to GCP via Tailscale
Step 5:  Flush Redis on GCP
Step 6:  Start Worker 1 (BFS) on Baadal in background
Step 7:  Wait 5 seconds for Worker 1 to initialize and flush Redis
Step 8:  Start Worker 2 (BFS) on GCP via ssh (nohup, logs to /tmp/worker2.log)
Step 9:  Wait for Worker 1 BFS to finish
Step 10: Fetch and display Worker 2 BFS log from GCP
Step 11: Start Worker 1 (Degree) on Baadal in background
Step 12: Wait 2 seconds, then start Worker 2 (Degree) on GCP via ssh
Step 13: Wait for Worker 1 Degree to finish
Step 14: Fetch and display Worker 2 Degree log from GCP
```

**The `$SPLIT` variable flows through the entire pipeline:**
```
controller.py → $SPLIT → ingest → worker (argv[3]) → degree (argv[3])
```

All four components always agree on the same percentage — there is no hardcoded value anywhere in the pipeline.

If `controller.py` determines no bursting is needed (graph fits locally), `run_hybrid.sh` forces a 70:30 demo split so the distributed pipeline still runs for demonstration purposes.

**Worker startup order matters:** Worker 1 (Baadal) always starts first because it is responsible for flushing Redis state from any previous run. Worker 2 starts after a short delay to ensure it connects to a clean Redis instance.

---

## Graph Data Format

Graphs are stored as edge lists — one edge per line, space separated:

```
1 2
1 3
2 4
3 4
4 5
```

Each line `u v` means there is an undirected edge between node `u` and node `v`. Node IDs must be integers. The graph is treated as undirected — every edge contributes to the degree of both endpoints.

---

## Datasets

| File | Description |
|---|---|
| `sample_graph.txt` | Small synthetic graph for quick testing |
| `medium_graph.txt` | Medium synthetic graph |
| `large_graph.txt` | Large synthetic graph — triggers cloud burst |
| `facebook_clean.txt` | Real Facebook social network (SNAP dataset) |
| `facebook_combined.txt` | Combined Facebook ego networks |
| `youtube_clean.txt` | Real YouTube social network (SNAP dataset) |

Real datasets sourced from [SNAP (Stanford Network Analysis Project)](https://snap.stanford.edu/data/).

---

## Prerequisites

### On Both Machines (Baadal + GCP)

- **Tailscale** installed and both machines joined to the same Tailnet
- **Redis** installed on GCP and bound to `0.0.0.0` (not just localhost)
- **hiredis** C++ Redis client library
- **g++** compiler with C++11 support
- **Python 3** with `psutil` library (Baadal only)

### Install dependencies

**Redis (on GCP):**
```bash
sudo apt install redis-server
# Edit /etc/redis/redis.conf — change "bind 127.0.0.1" to "bind 0.0.0.0"
sudo systemctl restart redis
```

**hiredis (on both machines):**
```bash
sudo apt install libhiredis-dev
```

**psutil (on Baadal):**
```bash
pip3 install psutil
```

---

## Installation & Setup

### 1. Clone the repository on both machines

```bash
git clone https://github.com/rajfauzdar/hybrid_sna.git
cd hybrid_sna
```

### 2. Compile all C++ binaries on Baadal

```bash
g++ -O2 -o ingest ingest.cpp
g++ -O2 -o worker worker.cpp -lhiredis
g++ -O2 -o degree degree.cpp -lhiredis
```

### 3. Compile on GCP

```bash
ssh razzfauzdar@<GCP_TAILSCALE_IP> "cd ~/hybrid_sna && g++ -O2 -o worker worker.cpp -lhiredis && g++ -O2 -o degree degree.cpp -lhiredis"
```

### 4. Make the pipeline script executable

```bash
chmod +x run_hybrid.sh
```

### 5. Configure GCP IP and user in `run_hybrid.sh`

```bash
GCP_IP="100.98.11.34"       # Your GCP Tailscale IP
GCP_USER="razzfauzdar"      # Your GCP username
```

### 6. Set up SSH key-based auth to GCP (so pipeline runs without password prompts)

```bash
ssh-keygen -t rsa           # skip if you already have a key
ssh-copy-id razzfauzdar@100.98.11.34
```

---

## Running the Project

### Full automated pipeline (recommended)

```bash
./run_hybrid.sh <graph_file>

# Examples:
./run_hybrid.sh sample_graph.txt
./run_hybrid.sh facebook_clean.txt
./run_hybrid.sh large_graph.txt
```

### Running components individually

```bash
# Step 1: Analyze and partition
python3 controller.py large_graph.txt
./ingest large_graph.txt 70

# Step 2: Copy partition to GCP
scp gcp_partition.txt razzfauzdar@100.98.11.34:~/hybrid_sna/

# Step 3: BFS — start worker 1 first (it flushes Redis)
./worker 1 baadal_partition.txt 70 &
ssh razzfauzdar@100.98.11.34 "cd ~/hybrid_sna && ./worker 2 gcp_partition.txt 70"

# Step 4: Degree — start worker 1 first (it flushes Redis)
./degree 1 baadal_partition.txt 70 &
ssh razzfauzdar@100.98.11.34 "cd ~/hybrid_sna && ./degree 2 gcp_partition.txt 70"
```

---

## Understanding the Output

### Controller output
```
Scanning dataset: large_graph.txt...
Graph Profile: 50000 Vertices, 200000 Edges.
Estimated RAM requirement: 548.96 MB
Baadal VM Safe Available RAM: 412.30 MB
Status: MEMORY EXHAUSTION PREDICTED. Triggering Hybrid Cloud Burst!
Executing C++ Partitioner with a 75:25 split...
```

### BFS output (Worker 1)
```
[Superstep 0] Source initialized locally.
[Superstep 0] Waiting at synchronization barrier...
[Superstep 1] Waiting at synchronization barrier...
...
Global BFS Termination reached at Superstep 8!
```

### Degree Centrality output (Worker 1)
```
========================================
   DEGREE CENTRALITY RESULTS
   Total nodes analyzed: 50000
========================================

Top 20 Most Connected Nodes:
----------------------------------------
  Rank 1:  Node 1234 | Degree: 891 | Normalized: 0.01782
  Rank 2:  Node 5678 | Degree: 743 | Normalized: 0.01486
  ...

----------------------------------------
Max degree: 891
Min degree: 1
Avg degree: 8.0
========================================
```

---

## Key Concepts Explained

**Bulk Synchronous Parallel (BSP):** A parallel computation model where all workers compute simultaneously, then synchronize at a barrier, then exchange messages — all in lockstep. This prevents race conditions without complex locking.

**Graph Partitioning:** Dividing a graph's edges across machines so each machine only stores a fraction of the data. The hash-based approach ensures deterministic, consistent assignment — the same node always goes to the same worker across all components.

**Cloud Bursting:** Automatically offloading computation to cloud resources when local capacity is exceeded. The controller makes this decision dynamically based on actual available RAM at runtime, not a fixed threshold.

**Batch Transfer Optimization:** Grouping all messages into a single network call instead of sending them one by one, then pulling all responses into local memory at once for local processing. Critical for WAN deployments where each round trip has significant latency cost. Reduces N network round trips to a constant 2.

**Degree Centrality:** A measure of how connected a node is. `degree(v) / (N-1)` gives a normalized value between 0 and 1. High degree centrality means an influential or well-connected node in the network.

**Tailscale:** A zero-config VPN that creates a secure private network between machines regardless of their physical location or network setup. Used here so Baadal (IITD private network) and GCP can communicate directly over stable private IPs without any firewall or port-forwarding configuration.
