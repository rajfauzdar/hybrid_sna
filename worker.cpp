#include <iostream>
#include <fstream>
#include <vector>
#include <unordered_map>
#include <string>
#include <queue>
#include <unistd.h> // For usleep (pause function)
#include <hiredis/hiredis.h>

using namespace std;

// --- Global State ---
unordered_map<int, vector<int>> adj_list;
unordered_map<int, int> distances;
const int BAADAL_PERCENTAGE = 70; 
const int INF = 99999999;

// Worker Config
int worker_id;
string my_queue, other_queue;

// --- Hashing & Routing ---
bool is_local(int vertex_id) {
    unsigned int mixed_hash = (unsigned int)vertex_id * 2654435761;
    int percentage = mixed_hash % 100;
    
    // If Worker 1 (Baadal), we own < 70. If Worker 2 (GCP), we own >= 70.
    if (worker_id == 1) return percentage < BAADAL_PERCENTAGE;
    else return percentage >= BAADAL_PERCENTAGE;
}

// --- Partition Loader ---
bool load_partition(const string& filename) {
    ifstream infile(filename);
    if (!infile.is_open()) return false;
    int u, v;
    while (infile >> u >> v) {
        adj_list[u].push_back(v);
        if (distances.find(u) == distances.end()) distances[u] = INF;
        if (distances.find(v) == distances.end()) distances[v] = INF;
    }
    infile.close();
    return true;
}

// --- The Bulk Synchronous Parallel (BSP) Loop ---
void run_bsp_bfs(redisContext* redis, int source_vertex) {
    queue<int> current_q;
    queue<int> next_q;

    // Only the worker who "owns" the source vertex initializes it
    if (is_local(source_vertex)) {
        distances[source_vertex] = 0;
        current_q.push(source_vertex);
        cout << "[Superstep 0] Source initialized locally." << endl;
    }

    int superstep = 0;

    // Main BSP Loop
    while (true) {
        bool worked_this_step = !current_q.empty();

        // -----------------------------------------------------
        // PHASE 1: COMPUTE & COMMUNICATE
        // -----------------------------------------------------
        while (!current_q.empty()) {
            int curr = current_q.front();
            current_q.pop();

            for (int neighbor : adj_list[curr]) {
                int new_dist = distances[curr] + 1;
                
                if (is_local(neighbor)) {
                    if (new_dist < distances[neighbor]) {
                        distances[neighbor] = new_dist;
                        next_q.push(neighbor); // Push to next level
                    }
                } else {
                    // Route to the other cloud
                    string msg = to_string(neighbor) + ":" + to_string(new_dist);
                    redisReply* r = (redisReply*)redisCommand(redis, "LPUSH %s %s", other_queue.c_str(), msg.c_str());
                    if (r) freeReplyObject(r);
                }
            }
        }

        // -----------------------------------------------------
        // PHASE 2: BARRIER SYNCHRONIZATION
        // -----------------------------------------------------
        string sync_key = "sync_step_" + to_string(superstep);
        redisReply* sync_r = (redisReply*)redisCommand(redis, "INCR %s", sync_key.c_str());
        if (sync_r) freeReplyObject(sync_r);

        cout << "[Superstep " << superstep << "] Waiting at synchronization barrier..." << endl;

        // Block execution until both workers check in
        while (true) {
            redisReply* check_r = (redisReply*)redisCommand(redis, "GET %s", sync_key.c_str());
            if (check_r && check_r->str && atoi(check_r->str) == 2) {
                freeReplyObject(check_r);
                break; // Both workers reached the barrier!
            }
            if (check_r) freeReplyObject(check_r);
            usleep(100000); // Sleep for 100ms to prevent spamming Redis
        }

        // -----------------------------------------------------
        // PHASE 3: READ MESSAGES (STATE SYNTHESIS)
        // -----------------------------------------------------
        bool received_messages = false;
        while (true) {
            // Read incoming edges from the other cloud
            redisReply* pop_r = (redisReply*)redisCommand(redis, "RPOP %s", my_queue.c_str());
            if (pop_r && pop_r->str) {
                received_messages = true;
                string msg = pop_r->str;
                
                // Parse "node:distance"
                int delim = msg.find(':');
                int node = stoi(msg.substr(0, delim));
                int dist = stoi(msg.substr(delim + 1));

                // If this is a new shortest path, accept it and schedule for next round
                if (dist < distances[node]) {
                    distances[node] = dist;
                    next_q.push(node);
                }
                freeReplyObject(pop_r);
            } else {
                if (pop_r) freeReplyObject(pop_r);
                break; // Queue is empty
            }
        }

        // -----------------------------------------------------
        // PHASE 4: GLOBAL TERMINATION CHECK
        // -----------------------------------------------------
        string active_key = "active_step_" + to_string(superstep);
        if (worked_this_step || received_messages) {
            redisReply* act_r = (redisReply*)redisCommand(redis, "INCR %s", active_key.c_str());
            if (act_r) freeReplyObject(act_r);
        }

        usleep(50000); // Small buffer to ensure both workers report activity

        redisReply* check_act_r = (redisReply*)redisCommand(redis, "GET %s", active_key.c_str());
        int active_count = 0;
        if (check_act_r && check_act_r->str) active_count = atoi(check_act_r->str);
        if (check_act_r) freeReplyObject(check_act_r);

        // If neither worker did local work AND neither received messages, the graph is fully explored.
        if (active_count == 0) {
            cout << "Global BFS Termination reached at Superstep " << superstep << "!" << endl;
            break; 
        }

        // Prepare for the next level
        swap(current_q, next_q);
        superstep++;
    }
}

int main(int argc, char* argv[]) {
    // Requires Worker ID and Partition File
    if (argc != 3) {
        cerr << "Usage: ./worker <worker_id_1_or_2> <partition_file.txt>" << endl;
        return 1;
    }

    worker_id = stoi(argv[1]);
    
    // Assign Redis queues based on identity
    if (worker_id == 1) {
        my_queue = "queue_baadal";
        other_queue = "queue_gcp";
    } else {
        my_queue = "queue_gcp";
        other_queue = "queue_baadal";
    }

    if (!load_partition(argv[2])) return 1;

    redisContext *redis = redisConnect("127.0.0.1", 6379);
    if (redis == NULL || redis->err) return 1;
    
    // Clear old state from Redis to ensure a clean run
    if (worker_id == 1) {
        redisCommand(redis, "FLUSHALL");
        cout << "Redis state cleared by Master Node." << endl;
    }
    
    usleep(500000); // Wait half a second for Worker 2 to catch up
    
    // Start BFS from Source Node 1
    run_bsp_bfs(redis, 1);

    redisFree(redis);
    return 0;
}
